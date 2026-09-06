#include "shinkou/editor/FileSystem.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace shinkou::editor {
namespace {

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

std::string key_for(const std::filesystem::path& path) {
    return path.generic_string();
}

} // namespace

FileSystemService::FileSystemService(std::filesystem::path root) { set_root(std::move(root)); }

void FileSystemService::set_root(std::filesystem::path root) {
    std::error_code error;
    if (root.empty()) root = std::filesystem::current_path(error);
    root_ = std::filesystem::weakly_canonical(root, error);
    if (error) root_ = std::filesystem::absolute(root, error).lexically_normal();
    snapshot_.clear();
    snapshotScope_.clear();
}

std::filesystem::path FileSystemService::resolve(std::filesystem::path relative, bool allowMissing) const {
    if (relative.empty()) return root_;
    if (relative.is_absolute()) return {};
    const auto candidate = (root_ / relative).lexically_normal();
    std::error_code error;
    const auto canonical = allowMissing ? std::filesystem::weakly_canonical(candidate, error)
                                        : std::filesystem::canonical(candidate, error);
    if (error) return {};
    const auto base = std::filesystem::weakly_canonical(root_, error);
    if (error) return {};
    const auto baseString = base.generic_string();
    const auto candidateString = canonical.generic_string();
    if (candidateString != baseString && candidateString.rfind(baseString + '/', 0) != 0) return {};
    return canonical;
}

std::uint64_t FileSystemService::stamp(const std::filesystem::directory_entry& entry) noexcept {
    std::error_code error;
    const auto time = entry.last_write_time(error);
    if (error) return 0;
    return static_cast<std::uint64_t>(time.time_since_epoch().count());
}

std::vector<FileEntry> FileSystemService::list(std::filesystem::path relative, bool recursive,
                                               std::size_t maxEntries) const {
    std::vector<FileEntry> result;
    const auto directory = resolve(std::move(relative), false);
    if (directory.empty() || maxEntries == 0) return result;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return result;
    auto append = [&](const auto& entry) {
        if (result.size() >= maxEntries || error) return;
        const auto relativePath = std::filesystem::relative(entry.path(), root_, error);
        if (error) return;
        FileEntry item;
        item.relativePath = relativePath.lexically_normal();
        item.name = entry.path().filename().string();
        item.directory = entry.is_directory(error);
        item.size = item.directory ? 0 : entry.file_size(error);
        item.writeStamp = stamp(entry);
        result.push_back(std::move(item));
    };
    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(directory, error), end; it != end && !error && result.size() < maxEntries; it.increment(error)) append(*it);
    } else {
        for (std::filesystem::directory_iterator it(directory, error), end; it != end && !error; it.increment(error)) append(*it);
    }
    std::sort(result.begin(), result.end(), [](const FileEntry& left, const FileEntry& right) {
        if (left.directory != right.directory) return left.directory > right.directory;
        return left.relativePath.generic_string() < right.relativePath.generic_string();
    });
    return result;
}

bool FileSystemService::read_text(const std::filesystem::path& relative, std::string& output,
                                  std::string* error) const {
    const auto path = resolve(relative, false);
    if (path.empty()) { set_error(error, "file path is outside the project root"); return false; }
    std::ifstream file(path, std::ios::binary);
    std::error_code sizeError;
    if (std::filesystem::file_size(path, sizeError) > 16 * 1024 * 1024 || sizeError) {
        set_error(error, "editor text file exceeds 16 MiB or its size is unavailable"); return false;
    }
    if (!file) { set_error(error, "cannot open file: " + path.string()); return false; }
    output.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (!file.good() && !file.eof()) { set_error(error, "cannot read file: " + path.string()); return false; }
    return true;
}

bool FileSystemService::write_text_atomic(const std::filesystem::path& relative, std::string_view content,
                                          std::string* error) const {
    const auto path = resolve(relative, true);
    if (path.empty()) { set_error(error, "file path is outside the project root"); return false; }
    std::error_code fsError;
    std::filesystem::create_directories(path.parent_path(), fsError);
    if (fsError) { set_error(error, "cannot create parent directory: " + fsError.message()); return false; }
    auto temporary = path;
    temporary += ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) { set_error(error, "cannot open temporary file: " + temporary.string()); return false; }
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.flush();
        if (!file) { file.close(); std::filesystem::remove(temporary, fsError); set_error(error, "cannot write temporary file: " + temporary.string()); return false; }
    }
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fsError = std::error_code(GetLastError(), std::system_category());
#else
    std::filesystem::rename(temporary, path, fsError);
#endif
    if (fsError) { std::filesystem::remove(temporary, fsError); set_error(error, "cannot commit file: " + path.string()); return false; }
    return true;
}

bool FileSystemService::ensure_directory(const std::filesystem::path& relative, std::string* error) const {
    const auto path = resolve(relative, true);
    if (path.empty()) { set_error(error, "directory path is outside the project root"); return false; }
    std::error_code fsError;
    std::filesystem::create_directories(path, fsError);
    if (fsError) { set_error(error, fsError.message()); return false; }
    return true;
}

bool FileSystemService::exists(const std::filesystem::path& relative, bool* directory) const noexcept {
    if (directory) *directory = false;
    const auto path = resolve(relative, false);
    if (path.empty()) return false;
    std::error_code error;
    const bool present = std::filesystem::exists(path, error);
    if (present && directory) *directory = std::filesystem::is_directory(path, error);
    return present && !error;
}

bool FileSystemService::remove(const std::filesystem::path& relative, std::string* error) const {
    bool directory = false;
    const auto path = resolve(relative, false);
    if (path.empty() || path == root_) {
        set_error(error, "cannot remove the project root or an outside path");
        return false;
    }
    std::error_code fsError;
    if (!std::filesystem::exists(path, fsError) || fsError) {
        set_error(error, fsError ? fsError.message() : "path does not exist");
        return false;
    }
    directory = std::filesystem::is_directory(path, fsError);
    if (fsError) { set_error(error, fsError.message()); return false; }
    const auto removed = directory ? std::filesystem::remove_all(path, fsError)
                                   : (std::filesystem::remove(path, fsError) ? 1u : 0u);
    if (fsError || removed == 0) {
        set_error(error, fsError ? fsError.message() : "path could not be removed");
        return false;
    }
    return true;
}

bool FileSystemService::rename(const std::filesystem::path& from,
                               const std::filesystem::path& to, std::string* error) const {
    const auto source = resolve(from, false);
    const auto destination = resolve(to, true);
    if (source.empty() || destination.empty() || source == root_ || destination == root_) {
        set_error(error, "rename path is outside the project root");
        return false;
    }
    std::error_code fsError;
    if (!std::filesystem::exists(source, fsError) || fsError) {
        set_error(error, fsError ? fsError.message() : "source path does not exist");
        return false;
    }
    if (std::filesystem::exists(destination, fsError) || fsError) {
        set_error(error, fsError ? fsError.message() : "destination path already exists");
        return false;
    }
    std::filesystem::rename(source, destination, fsError);
    if (fsError) { set_error(error, fsError.message()); return false; }
    return true;
}

std::vector<FileChange> FileSystemService::poll_changes(bool recursive, std::size_t maxEntries) {
    return scan(recursive, maxEntries).changes;
}

FileScanResult FileSystemService::scan(bool recursive, std::size_t maxEntries) {
    return scan({}, recursive, maxEntries);
}

FileScanResult FileSystemService::scan(std::filesystem::path relative, bool recursive, std::size_t maxEntries) {
    FileScanResult result;
    const auto scope = relative.lexically_normal() == std::filesystem::path(".")
        ? std::string{} : relative.lexically_normal().generic_string();
    if (scope != snapshotScope_) {
        snapshot_.clear();
        snapshotScope_ = scope;
    }
    result.entries = list(std::move(relative), recursive, maxEntries);
    std::unordered_map<std::string, std::uint64_t> current;
    current.reserve(result.entries.size());
    for (const auto& entry : result.entries) current[key_for(entry.relativePath)] = entry.writeStamp;
    result.changes.reserve(current.size() + snapshot_.size());
    for (const auto& [path, value] : current) {
        const auto found = snapshot_.find(path);
        if (found == snapshot_.end()) result.changes.push_back({path, FileChangeType::Added});
        else if (found->second != value) result.changes.push_back({path, FileChangeType::Modified});
    }
    for (const auto& [path, value] : snapshot_)
        if (current.find(path) == current.end()) result.changes.push_back({path, FileChangeType::Removed});
    snapshot_ = std::move(current);
    std::sort(result.changes.begin(), result.changes.end(), [](const FileChange& left, const FileChange& right) {
        return left.relativePath.generic_string() < right.relativePath.generic_string();
    });
    return result;
}

} // namespace shinkou::editor

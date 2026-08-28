#include "shinkou/editor/FileSystem.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>

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
        for (std::filesystem::recursive_directory_iterator it(directory, error), end; it != end && !error; it.increment(error)) append(*it);
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
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) { set_error(error, "cannot open temporary file: " + temporary); return false; }
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.flush();
        if (!file) { set_error(error, "cannot write temporary file: " + temporary); return false; }
    }
    std::filesystem::rename(temporary, path, fsError);
    if (fsError) {
        std::filesystem::remove(path, fsError);
        fsError.clear();
        std::filesystem::rename(temporary, path, fsError);
    }
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

std::vector<FileChange> FileSystemService::poll_changes(bool recursive, std::size_t maxEntries) {
    std::unordered_map<std::string, std::uint64_t> current;
    for (const auto& entry : list({}, recursive, maxEntries)) current[key_for(entry.relativePath)] = entry.writeStamp;
    std::vector<FileChange> changes;
    for (const auto& [path, value] : current) {
        const auto found = snapshot_.find(path);
        if (found == snapshot_.end()) changes.push_back({path, FileChangeType::Added});
        else if (found->second != value) changes.push_back({path, FileChangeType::Modified});
    }
    for (const auto& [path, value] : snapshot_) if (current.find(path) == current.end()) changes.push_back({path, FileChangeType::Removed});
    snapshot_ = std::move(current);
    std::sort(changes.begin(), changes.end(), [](const FileChange& left, const FileChange& right) {
        return left.relativePath.generic_string() < right.relativePath.generic_string();
    });
    return changes;
}

} // namespace shinkou::editor

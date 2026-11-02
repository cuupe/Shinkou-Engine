#include "shinkou/editor/FileSystem.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace shinkou::editor {
namespace {

void set_error(std::string *error, std::string message) {
    if (error)
        *error = std::move(message);
}

std::string key_for(const std::filesystem::path &path) {
    return path.generic_string();
}

bool is_inside(const std::filesystem::path &root, const std::filesystem::path &path) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.has_root_path() &&
           *relative.begin() != std::filesystem::path("..");
}

std::filesystem::path canonical_path(const std::filesystem::path &path, bool allowMissing,
                                     std::error_code &error) {
#if defined(_WIN32)
    // Some MinGW standard libraries treat junctions as ordinary directories.
    // Resolve through a native handle before enforcing the project boundary.
    auto existing = std::filesystem::absolute(path, error);
    if (error)
        return {};
    std::filesystem::path missing;
    for (;;) {
        const auto handle =
            CreateFileW(existing.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            std::wstring buffer(512, L'\0');
            auto size =
                GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (size >= buffer.size()) {
                buffer.resize(static_cast<std::size_t>(size) + 1);
                size = GetFinalPathNameByHandleW(handle, buffer.data(),
                                                 static_cast<DWORD>(buffer.size()),
                                                 FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            }
            const auto code = size == 0 ? GetLastError() : ERROR_SUCCESS;
            CloseHandle(handle);
            if (code != ERROR_SUCCESS || size >= buffer.size()) {
                error = std::error_code(code ? code : ERROR_INSUFFICIENT_BUFFER,
                                        std::system_category());
                return {};
            }
            buffer.resize(size);
            if (buffer.rfind(L"\\\\?\\UNC\\", 0) == 0)
                buffer = L"\\\\" + buffer.substr(8);
            else if (buffer.rfind(L"\\\\?\\", 0) == 0)
                buffer.erase(0, 4);
            error.clear();
            return (missing.empty() ? std::filesystem::path(buffer)
                                    : std::filesystem::path(buffer) / missing)
                .lexically_normal();
        }
        const auto code = GetLastError();
        const auto attributes = GetFileAttributesW(existing.c_str());
        const auto parent = existing.parent_path();
        if (!allowMissing || (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) ||
            (attributes != INVALID_FILE_ATTRIBUTES &&
             (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) ||
            parent == existing || parent.empty()) {
            error = std::error_code(code, std::system_category());
            return {};
        }
        missing = missing.empty() ? existing.filename() : existing.filename() / missing;
        existing = parent;
    }
#else
    return allowMissing ? std::filesystem::weakly_canonical(path, error)
                        : std::filesystem::canonical(path, error);
#endif
}

bool is_link(const std::filesystem::path &path, std::error_code &error) {
#if defined(_WIN32)
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = std::error_code(GetLastError(), std::system_category());
        return false;
    }
    error.clear();
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
#endif
}

bool remove_entry_tree(const std::filesystem::path &path, std::error_code &error) {
    const bool link = is_link(path, error);
    if (error)
        return false;
#if defined(_WIN32)
    if (link) {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            error = std::error_code(GetLastError(), std::system_category());
            return false;
        }
        const bool removed = (attributes & FILE_ATTRIBUTE_DIRECTORY)
                                 ? RemoveDirectoryW(path.c_str()) != 0
                                 : DeleteFileW(path.c_str()) != 0;
        if (!removed)
            error = std::error_code(GetLastError(), std::system_category());
        return removed;
    }
#endif
    if (!link && std::filesystem::is_directory(path, error)) {
        for (std::filesystem::directory_iterator it(path, error), end; it != end && !error;
             it.increment(error)) {
            if (!remove_entry_tree(it->path(), error))
                return false;
        }
    }
    return !error && std::filesystem::remove(path, error);
}

} // namespace

FileSystemService::FileSystemService(std::filesystem::path root) {
    set_root(std::move(root));
}

void FileSystemService::set_root(std::filesystem::path root) {
    std::error_code error;
    if (root.empty())
        root = std::filesystem::current_path(error);
    root_ = canonical_path(root, true, error);
    if (error)
        root_ = std::filesystem::absolute(root, error).lexically_normal();
    snapshot_.clear();
    snapshotScope_.clear();
    snapshotRecursive_ = true;
}

std::filesystem::path
FileSystemService::resolve_existing(const std::filesystem::path &relative) const {
    return resolve(relative, false);
}

std::filesystem::path
FileSystemService::project_relative_existing(const std::filesystem::path &absolute) const {
    if (absolute.empty() || !absolute.is_absolute() || root_.empty())
        return {};
    std::error_code error;
    const auto canonicalFile = canonical_path(absolute, false, error);
    if (error)
        return {};
    const auto canonicalRoot = canonical_path(root_, true, error);
    if (error)
        return {};
    const auto relative = canonicalFile.lexically_relative(canonicalRoot);
    if (relative.empty())
        return {};
    const auto normalized = relative.lexically_normal();
    const auto text = normalized.generic_string();
    if (normalized.is_absolute() || normalized == ".." || text.rfind("../", 0) == 0)
        return {};
    return normalized;
}

std::filesystem::path FileSystemService::resolve(std::filesystem::path relative,
                                                 bool allowMissing) const {
    if (root_.empty())
        return {};
    if (relative.empty())
        return root_;
    if (relative.has_root_path())
        return {};
    const auto candidate = root_ / relative;
    std::error_code error;
    const auto canonical = canonical_path(candidate, allowMissing, error);
    if (error)
        return {};
    const auto base = canonical_path(root_, true, error);
    if (error)
        return {};
    if (!is_inside(base, canonical))
        return {};
    return canonical;
}

std::filesystem::path FileSystemService::resolve_entry(std::filesystem::path relative) const {
    if (relative.empty() || relative.has_root_path())
        return {};
    while (!relative.empty() && relative.filename().empty())
        relative = relative.parent_path();
    const auto name = relative.filename();
    if (name.empty() || name == "." || name == "..")
        return {};
    const auto parent = resolve(relative.parent_path(), false);
    return parent.empty() ? std::filesystem::path{} : parent / name;
}

std::vector<FileEntry> FileSystemService::list(std::filesystem::path relative, bool recursive,
                                               std::size_t maxEntries) const {
    return list_detailed(std::move(relative), recursive, maxEntries).entries;
}

FileListResult FileSystemService::list_detailed(std::filesystem::path relative, bool recursive,
                                                std::size_t maxEntries) const {
    FileListResult result;
    const auto fail = [&](std::string message) {
        result.complete = false;
        result.error = std::move(message);
    };
    const auto directory = resolve(std::move(relative), true);
    if (directory.empty()) {
        fail("directory path cannot be resolved inside the project root");
        return result;
    }
    std::error_code error;
    const auto status = std::filesystem::status(directory, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && status.type() == std::filesystem::file_type::not_found))
        return result;
    if (error || !std::filesystem::is_directory(status)) {
        fail(error ? error.message() : "scan path is not a directory");
        return result;
    }
    auto append = [&](const auto &entry) {
        std::error_code entryError;
        const auto link = is_link(entry.path(), entryError);
        if (entryError) {
            fail(entryError.message());
            return;
        }
        // Do not canonicalize the published name: an in-project link has an
        // identity of its own. Resolve only to check its target's boundary.
        const auto relativePath = entry.path().lexically_relative(root_);
        if (!is_inside(root_, entry.path()))
            return;
        if (link && resolve(relativePath, false).empty())
            return;
        const auto entryStatus = entry.status(entryError);
        if (entryError) {
            fail(entryError.message());
            return;
        }
        if (!std::filesystem::is_directory(entryStatus) &&
            !std::filesystem::is_regular_file(entryStatus))
            return;
        if (result.entries.size() >= maxEntries) {
            result.complete = false;
            result.truncated = true;
            return;
        }
        FileEntry item;
        item.relativePath = relativePath.lexically_normal();
        item.name = entry.path().filename().string();
        item.directory = std::filesystem::is_directory(entryStatus);
        item.size = item.directory ? 0 : entry.file_size(entryError);
        if (entryError) {
            fail(entryError.message());
            return;
        }
        const auto time = entry.last_write_time(entryError);
        if (entryError) {
            fail(entryError.message());
            return;
        }
        item.writeStamp = static_cast<std::uint64_t>(time.time_since_epoch().count());
        result.entries.push_back(std::move(item));
    };
    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(directory, error), end;
             it != end && !error && result.complete; it.increment(error)) {
            // Disable recursion explicitly: MinGW may not recognize a
            // junction as a symlink even though the OS marks it as a link.
            std::error_code linkError;
            if (is_link(it->path(), linkError))
                it.disable_recursion_pending();
            if (linkError) {
                fail(linkError.message());
                break;
            }
            append(*it);
        }
    } else {
        for (std::filesystem::directory_iterator it(directory, error), end;
             it != end && !error && result.complete; it.increment(error))
            append(*it);
    }
    if (error)
        fail(error.message());
    std::sort(result.entries.begin(), result.entries.end(),
              [](const FileEntry &left, const FileEntry &right) {
                  if (left.directory != right.directory)
                      return left.directory > right.directory;
                  return left.relativePath.generic_string() < right.relativePath.generic_string();
              });
    return result;
}

bool FileSystemService::read_text(const std::filesystem::path &relative, std::string &output,
                                  std::string *error) const {
    bool truncated = false;
    if (!read_text_limited(relative, 16u * 1024u * 1024u, output, &truncated, error))
        return false;
    if (truncated) {
        output.clear();
        set_error(error, "editor text file exceeds 16 MiB");
        return false;
    }
    return true;
}

bool FileSystemService::read_text_limited(const std::filesystem::path &relative,
                                          std::size_t maxBytes, std::string &output,
                                          bool *truncated, std::string *error) const {
    output.clear();
    if (error)
        error->clear();
    if (truncated)
        *truncated = false;
    const auto path = resolve(relative, false);
    if (path.empty()) {
        set_error(error, "file path is outside the project root");
        return false;
    }
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(path, sizeError);
    const auto limit = std::min<std::uintmax_t>(maxBytes, 16u * 1024u * 1024u);
    if (sizeError) {
        set_error(error, "editor text file size is unavailable");
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        set_error(error, "cannot open file: " + path.string());
        return false;
    }
    const auto readSize = std::min<std::uintmax_t>(size, limit);
    output.resize(static_cast<std::size_t>(readSize));
    if (readSize != 0 && !file.read(output.data(), static_cast<std::streamsize>(readSize))) {
        output.clear();
        set_error(error, "cannot read file: " + path.string());
        return false;
    }
    if (truncated)
        *truncated = size > limit;
    return true;
}

bool FileSystemService::write_text_atomic(const std::filesystem::path &relative,
                                          std::string_view content, std::string *error) const {
    if (error)
        error->clear();
    const auto path = resolve(relative, true);
    if (path.empty() || path == root_) {
        set_error(error, "cannot write the project root or an outside path");
        return false;
    }
    std::error_code fsError;
    std::filesystem::create_directories(path.parent_path(), fsError);
    if (fsError) {
        set_error(error, "cannot create parent directory: " + fsError.message());
        return false;
    }
    auto temporary = path;
    temporary +=
        ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            set_error(error, "cannot open temporary file: " + temporary.string());
            return false;
        }
        if (!content.empty())
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.flush();
        file.close();
        if (!file) {
            file.close();
            std::filesystem::remove(temporary, fsError);
            set_error(error, "cannot write temporary file: " + temporary.string());
            return false;
        }
    }
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fsError = std::error_code(GetLastError(), std::system_category());
#else
    std::filesystem::rename(temporary, path, fsError);
#endif
    if (fsError) {
        std::filesystem::remove(temporary, fsError);
        set_error(error, "cannot commit file: " + path.string());
        return false;
    }
    return true;
}

bool FileSystemService::ensure_directory(const std::filesystem::path &relative,
                                         std::string *error) const {
    if (error)
        error->clear();
    const auto path = resolve(relative, true);
    if (path.empty()) {
        set_error(error, "directory path is outside the project root");
        return false;
    }
    std::error_code fsError;
    std::filesystem::create_directories(path, fsError);
    if (fsError) {
        set_error(error, fsError.message());
        return false;
    }
    return true;
}

bool FileSystemService::exists(const std::filesystem::path &relative,
                               bool *directory) const noexcept {
    if (directory)
        *directory = false;
    const auto path = resolve(relative, false);
    if (path.empty())
        return false;
    std::error_code error;
    const bool present = std::filesystem::exists(path, error);
    if (present && directory)
        *directory = std::filesystem::is_directory(path, error);
    return present && !error;
}

bool FileSystemService::remove(const std::filesystem::path &relative, std::string *error) const {
    if (error)
        error->clear();
    const auto path = resolve_entry(relative);
    if (path.empty() || path == root_) {
        set_error(error, "cannot remove the project root or an outside path");
        return false;
    }
    std::error_code fsError;
#if defined(_WIN32)
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        set_error(error, std::error_code(GetLastError(), std::system_category()).message());
        return false;
    }
#else
    const auto status = std::filesystem::symlink_status(path, fsError);
    if (!std::filesystem::exists(status) || fsError) {
        set_error(error, fsError ? fsError.message() : "path does not exist");
        return false;
    }
#endif
    if (!remove_entry_tree(path, fsError)) {
        set_error(error, fsError ? fsError.message() : "path could not be removed");
        return false;
    }
    return true;
}

bool FileSystemService::rename(const std::filesystem::path &from, const std::filesystem::path &to,
                               std::string *error) const {
    if (error)
        error->clear();
    const auto source = resolve_entry(from);
    const auto destination = resolve_entry(to);
    if (source.empty() || destination.empty() || source == root_ || destination == root_) {
        set_error(error, "rename path is outside the project root");
        return false;
    }
    std::error_code fsError;
#if defined(_WIN32)
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        set_error(error, std::error_code(GetLastError(), std::system_category()).message());
        return false;
    }
    if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES) {
        set_error(error, "destination path already exists");
        return false;
    }
    const auto destinationError = GetLastError();
    if (destinationError != ERROR_FILE_NOT_FOUND && destinationError != ERROR_PATH_NOT_FOUND) {
        set_error(error, std::error_code(destinationError, std::system_category()).message());
        return false;
    }
#else
    const auto sourceStatus = std::filesystem::symlink_status(source, fsError);
    if (!std::filesystem::exists(sourceStatus) || fsError) {
        set_error(error, fsError ? fsError.message() : "source path does not exist");
        return false;
    }
    const auto destinationStatus = std::filesystem::symlink_status(destination, fsError);
    if (fsError == std::errc::no_such_file_or_directory)
        fsError.clear();
    if (std::filesystem::exists(destinationStatus) || fsError) {
        set_error(error, fsError ? fsError.message() : "destination path already exists");
        return false;
    }
#endif
#if defined(_WIN32)
    // Readers and directory scanners can briefly deny DELETE sharing on
    // Windows. Retry only these transient failures, with a bounded 94 ms
    // budget. MoveFileEx without REPLACE_EXISTING preserves the no-overwrite
    // contract even if another process creates the destination after checking.
    constexpr unsigned delays[]{2, 4, 8, 16, 32, 32};
    for (std::size_t attempt = 0;; ++attempt) {
        if (MoveFileExW(source.c_str(), destination.c_str(), 0))
            return true;
        const auto code = GetLastError();
        if ((code != ERROR_SHARING_VIOLATION && code != ERROR_ACCESS_DENIED) ||
            attempt == std::size(delays)) {
            set_error(error, std::error_code(code, std::system_category()).message());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delays[attempt]));
    }
#else
    std::filesystem::rename(source, destination, fsError);
    if (fsError) {
        set_error(error, fsError.message());
        return false;
    }
    return true;
#endif
}

std::vector<FileChange> FileSystemService::poll_changes(bool recursive, std::size_t maxEntries) {
    return scan(recursive, maxEntries).changes;
}

FileScanResult FileSystemService::scan(bool recursive, std::size_t maxEntries) {
    return scan({}, recursive, maxEntries);
}

FileScanResult FileSystemService::scan(std::filesystem::path relative, bool recursive,
                                       std::size_t maxEntries) {
    FileScanResult result;
    auto listing = list_detailed(relative, recursive, maxEntries);
    result.entries = std::move(listing.entries);
    result.complete = listing.complete;
    result.truncated = listing.truncated;
    result.error = std::move(listing.error);
    // An unvisited entry is not evidence of deletion. Keep the last complete
    // baseline intact until a full scan can describe the same population.
    if (!result.complete)
        return result;
    const auto scope = relative.lexically_normal() == std::filesystem::path(".")
                           ? std::string{}
                           : relative.lexically_normal().generic_string();
    if (scope != snapshotScope_ || recursive != snapshotRecursive_) {
        snapshot_.clear();
        snapshotScope_ = scope;
        snapshotRecursive_ = recursive;
    }
    std::unordered_map<std::string, SnapshotValue> current;
    current.reserve(result.entries.size());
    for (const auto &entry : result.entries) {
        current[key_for(entry.relativePath)] = {entry.size, entry.writeStamp, entry.directory};
    }
    result.changes.reserve(current.size() + snapshot_.size());
    for (const auto &[path, value] : current) {
        const auto found = snapshot_.find(path);
        if (found == snapshot_.end()) {
            result.changes.push_back({path, FileChangeType::Added, 0, value.size, 0,
                                      value.writeStamp, false, value.directory});
        } else if (!(found->second == value)) {
            result.changes.push_back({path, FileChangeType::Modified, found->second.size,
                                      value.size, found->second.writeStamp, value.writeStamp,
                                      found->second.directory, value.directory});
        }
    }
    for (const auto &[path, value] : snapshot_) {
        if (current.find(path) == current.end()) {
            result.changes.push_back({path, FileChangeType::Removed, value.size, 0,
                                      value.writeStamp, 0, value.directory, false});
        }
    }
    snapshot_ = std::move(current);
    std::sort(result.changes.begin(), result.changes.end(),
              [](const FileChange &left, const FileChange &right) {
                  return left.relativePath.generic_string() < right.relativePath.generic_string();
              });
    return result;
}

} // namespace shinkou::editor

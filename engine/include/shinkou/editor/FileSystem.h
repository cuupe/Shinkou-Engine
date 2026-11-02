#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shinkou::editor {

struct FileEntry {
    std::filesystem::path relativePath;
    std::string name;
    bool directory{false};
    std::uintmax_t size{0};
    std::uint64_t writeStamp{0};
};

enum class FileChangeType : std::uint8_t { Added, Modified, Removed };

struct FileChange {
    std::filesystem::path relativePath;
    FileChangeType type{FileChangeType::Modified};
    std::uintmax_t previousSize{0};
    std::uintmax_t currentSize{0};
    std::uint64_t previousWriteStamp{0};
    std::uint64_t currentWriteStamp{0};
    bool previousDirectory{false};
    bool currentDirectory{false};
};

struct FileListResult {
    std::vector<FileEntry> entries;
    bool complete{true};
    bool truncated{false};
    std::string error;
};

struct FileScanResult {
    std::vector<FileEntry> entries;
    std::vector<FileChange> changes;
    bool complete{true};
    bool truncated{false};
    std::string error;
};

// Editor-scoped file access. All paths are relative to the project root and
// are checked before reading or writing, preventing accidental traversal.
class FileSystemService final {
    struct SnapshotValue {
        std::uintmax_t size{0};
        std::uint64_t writeStamp{0};
        bool directory{false};

        bool operator==(const SnapshotValue &other) const noexcept {
            return size == other.size && writeStamp == other.writeStamp &&
                   directory == other.directory;
        }
    };

    std::filesystem::path root_;
    std::unordered_map<std::string, SnapshotValue> snapshot_;
    std::string snapshotScope_;
    bool snapshotRecursive_{true};

    std::filesystem::path resolve(std::filesystem::path relative, bool allowMissing) const;
    std::filesystem::path resolve_entry(std::filesystem::path relative) const;

  public:
    explicit FileSystemService(std::filesystem::path root = {});

    void set_root(std::filesystem::path root);
    const std::filesystem::path &root() const noexcept {
        return root_;
    }
    // Returns a canonical existing path only when the relative file remains
    // inside the project root. Callers may pass the result to a subsystem
    // that requires an absolute path, but must not use it for writes.
    std::filesystem::path resolve_existing(const std::filesystem::path &relative) const;
    // Convert an existing native absolute path into a project-relative path
    // only when its canonical target remains inside root_. Symlinks and
    // junctions are resolved before the boundary check.
    std::filesystem::path project_relative_existing(const std::filesystem::path &absolute) const;

    std::vector<FileEntry> list(std::filesystem::path relative = {}, bool recursive = false,
                                std::size_t maxEntries = 4096) const;
    // Unlike list(), reports a budget limit or an IO failure. Links outside
    // the project and dangling links are omitted; directory links are never
    // traversed. Entry paths retain their own names rather than link targets.
    FileListResult list_detailed(std::filesystem::path relative = {}, bool recursive = false,
                                 std::size_t maxEntries = 4096) const;
    bool read_text(const std::filesystem::path &relative, std::string &output,
                   std::string *error = nullptr) const;
    bool read_text_limited(const std::filesystem::path &relative, std::size_t maxBytes,
                           std::string &output, bool *truncated = nullptr,
                           std::string *error = nullptr) const;
    bool write_text_atomic(const std::filesystem::path &relative, std::string_view content,
                           std::string *error = nullptr) const;
    bool ensure_directory(const std::filesystem::path &relative,
                          std::string *error = nullptr) const;
    bool exists(const std::filesystem::path &relative, bool *directory = nullptr) const noexcept;
    // A final path component that is a link is removed/renamed itself.
    bool remove(const std::filesystem::path &relative, std::string *error = nullptr) const;
    bool rename(const std::filesystem::path &from, const std::filesystem::path &to,
                std::string *error = nullptr) const;

    // Only complete scans publish changes and replace the baseline. Changing
    // the directory or recursion mode starts a new baseline. Missing scan
    // directories are complete empty results, so real deletions are reported.
    FileScanResult scan(bool recursive = true, std::size_t maxEntries = 8192);
    FileScanResult scan(std::filesystem::path relative, bool recursive,
                        std::size_t maxEntries = 8192);
    std::vector<FileChange> poll_changes(bool recursive = true, std::size_t maxEntries = 8192);
};

} // namespace shinkou::editor

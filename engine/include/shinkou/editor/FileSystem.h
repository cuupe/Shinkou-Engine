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
};

// Editor-scoped file access. All paths are relative to the project root and
// are checked before reading or writing, preventing accidental traversal.
class FileSystemService final {
    std::filesystem::path root_;
    std::unordered_map<std::string, std::uint64_t> snapshot_;

    std::filesystem::path resolve(std::filesystem::path relative, bool allowMissing) const;
    static std::uint64_t stamp(const std::filesystem::directory_entry& entry) noexcept;

public:
    explicit FileSystemService(std::filesystem::path root = {});

    void set_root(std::filesystem::path root);
    const std::filesystem::path& root() const noexcept { return root_; }

    std::vector<FileEntry> list(std::filesystem::path relative = {}, bool recursive = false,
                                std::size_t maxEntries = 4096) const;
    bool read_text(const std::filesystem::path& relative, std::string& output,
                   std::string* error = nullptr) const;
    bool write_text_atomic(const std::filesystem::path& relative, std::string_view content,
                           std::string* error = nullptr) const;
    bool ensure_directory(const std::filesystem::path& relative, std::string* error = nullptr) const;

    std::vector<FileChange> poll_changes(bool recursive = true, std::size_t maxEntries = 8192);
};

} // namespace shinkou::editor

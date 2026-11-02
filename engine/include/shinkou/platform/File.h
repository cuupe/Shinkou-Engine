#pragma once

// Shinkou Engine - native file and directory services.
//
// This is the raw OS layer: UTF-8 paths, native handles and explicit sharing
// modes. Project-relative path validation stays in the editor's
// FileSystemService; both layers are intentionally separate so the engine can
// use files without depending on editor policy.

#include "shinkou/platform/Error.h"
#include "shinkou/platform/Platform.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::platform {

enum class OpenMode : std::uint8_t { Read = 0, Write, ReadWrite, Append };

enum class ShareMode : std::uint8_t {
    // Exclusive: other processes cannot open the file while this handle lives.
    None = 0,
    Read,
    ReadWrite,
    // Allows readers, writers and renames/deletes (POSIX close-on-delete
    // semantics on Windows).
    ReadWriteDelete,
};

enum class SeekOrigin : std::uint8_t { Begin = 0, Current, End };

struct OpenOptions {
    OpenMode mode{OpenMode::Read};
    // Sharing policy for the lifetime of the handle. Non-Windows backends
    // implement the closest advisory equivalent.
    ShareMode share{ShareMode::ReadWrite};
    bool create_if_missing{false};
    bool truncate{false};
    // Bypass the OS write cache where supported.
    bool write_through{false};
};

// Timestamps are nanoseconds since the Unix epoch, UTC. Missing timestamps are
// reported as 0.
struct FileInfo {
    std::string path;
    std::string name;
    std::uint64_t size{0};
    bool is_directory{false};
    bool is_symlink{false};
    bool is_read_only{false};
    std::int64_t created_nanoseconds{0};
    std::int64_t modified_nanoseconds{0};
    std::int64_t accessed_nanoseconds{0};
};

class IFileStream {
  public:
    virtual ~IFileStream() = default;

    // Returns the number of bytes transferred; a short read means end of file.
    virtual Result<std::size_t> read(void *buffer, std::size_t bytes) = 0;
    virtual Result<std::size_t> write(const void *buffer, std::size_t bytes) = 0;
    // Loops until every byte was written.
    virtual Status write_all(const void *buffer, std::size_t bytes) = 0;

    virtual std::uint64_t size() const = 0;
    virtual std::uint64_t position() const = 0;
    virtual Status seek(std::int64_t offset, SeekOrigin origin) = 0;
    virtual Status flush() = 0;
    virtual Status truncate(std::uint64_t size) = 0;

    virtual void *native_handle() noexcept = 0;
};

class IDirectoryIterator {
  public:
    virtual ~IDirectoryIterator() = default;

    // Fills `entry` and returns true for the next item. At the end it returns
    // false and leaves `error` clean; on failure it returns false and sets
    // `error`.
    virtual bool next(FileInfo &entry, SystemError &error) = 0;
};

enum class FileChangeKind : std::uint8_t { Added = 0, Removed, Modified, Renamed };

struct FileChangeEvent {
    FileChangeKind kind{FileChangeKind::Modified};
    std::string path;
    // Only set for FileChangeKind::Renamed.
    std::string previous_path;
};

// Directory change notifications. The Windows backend uses the kernel change
// journal (ReadDirectoryChangesW); the portable backend polls metadata and
// therefore may miss timestamp-preserving rewrites.
class IFileWatcher {
  public:
    virtual ~IFileWatcher() = default;

    // Blocks until at least one event was buffered or the timeout expires.
    // A negative timeout waits forever; zero polls.
    virtual bool wait_for_change(Nanoseconds timeout) = 0;

    // Moves all buffered events to `events` and clears the internal queue.
    virtual void drain(std::vector<FileChangeEvent> &events) = 0;
};

} // namespace shinkou::platform

// Portable files, directories and polling directory watcher built on
// std::filesystem plus the C stdio API.

#include "PortableBackend.h"

#include "PortableCommon.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace shinkou::platform::detail {
namespace {

namespace fs = std::filesystem;

fs::path to_path(std::string_view text) {
    return portable_path_from_utf8(text);
}

const char *portable_open_mode(const OpenOptions &options) {
    switch (options.mode) {
    case OpenMode::Read:
        return "rb";
    case OpenMode::Write:
    case OpenMode::ReadWrite:
        return options.truncate ? "w+b" : "r+b";
    case OpenMode::Append:
        return "ab";
    }
    return "rb";
}

void fill_native_times(const fs::path &path, FileInfo &info) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.native().c_str(), GetFileExInfoStandard, &data))
        return;
    info.created_nanoseconds = unix_nanoseconds_from_filetime(data.ftCreationTime);
    info.modified_nanoseconds = unix_nanoseconds_from_filetime(data.ftLastWriteTime);
    info.accessed_nanoseconds = unix_nanoseconds_from_filetime(data.ftLastAccessTime);
#else
    struct stat status {};
    if (stat(path.c_str(), &status) != 0)
        return;
    info.size = static_cast<std::uint64_t>(status.st_size);
#if defined(SHINKOU_PLATFORM_APPLE)
    const auto &modified = status.st_mtimespec;
    const auto &accessed = status.st_atimespec;
#else
    const auto &modified = status.st_mtim;
    const auto &accessed = status.st_atim;
#endif
    info.modified_nanoseconds =
        static_cast<std::int64_t>(modified.tv_sec) * kNanosecondsPerSecond + modified.tv_nsec;
    info.accessed_nanoseconds =
        static_cast<std::int64_t>(accessed.tv_sec) * kNanosecondsPerSecond + accessed.tv_nsec;
#if defined(SHINKOU_PLATFORM_APPLE)
    info.created_nanoseconds = static_cast<std::int64_t>(status.st_birthtimespec.tv_sec) *
                                   kNanosecondsPerSecond +
                               status.st_birthtimespec.tv_nsec;
#endif
#endif
}

void fill_file_info(const fs::path &path, FileInfo &info) {
    info.path = portable_path_to_utf8(path);
    info.name = portable_path_to_utf8(path.filename());
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (!error) {
        info.is_directory = fs::is_directory(status);
        info.is_symlink = fs::is_symlink(status);
    }
    error.clear();
    const std::uintmax_t size = fs::file_size(path, error);
    if (!error)
        info.size = static_cast<std::uint64_t>(size);
    fill_native_times(path, info);
}

} // namespace

// ---------------------------------------------------------------------------
// File stream
// ---------------------------------------------------------------------------
class PortableFileStream final : public IFileStream {
  public:
    PortableFileStream(const std::string &path, const OpenOptions &options) {
        if (path.empty()) {
            error_ = make_error(ErrorCode::InvalidArgument, 0, "file path is empty");
            return;
        }
        file_ = portable_fopen(path, portable_open_mode(options));
        if (file_ == nullptr && options.create_if_missing &&
            (options.mode == OpenMode::Write || options.mode == OpenMode::ReadWrite))
            file_ = portable_fopen(path, "w+b");
        if (file_ == nullptr) {
            error_ = portable_last_error(ErrorCode::IoError, "fopen");
            return;
        }
    }

    ~PortableFileStream() override {
        if (file_ != nullptr)
            std::fclose(file_);
    }

    bool valid() const noexcept {
        return file_ != nullptr;
    }

    const SystemError &error() const noexcept {
        return error_;
    }

    Result<std::size_t> read(void *buffer, std::size_t bytes) override {
        if (file_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "file is not open");
        if (bytes == 0)
            return static_cast<std::size_t>(0);
        const std::size_t read = std::fread(buffer, 1, bytes, file_);
        if (read == 0 && std::ferror(file_) != 0)
            return portable_last_error(ErrorCode::IoError, "fread");
        return read;
    }

    Result<std::size_t> write(const void *buffer, std::size_t bytes) override {
        if (file_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "file is not open");
        if (bytes == 0)
            return static_cast<std::size_t>(0);
        const std::size_t written = std::fwrite(buffer, 1, bytes, file_);
        if (written == 0 && std::ferror(file_) != 0)
            return portable_last_error(ErrorCode::IoError, "fwrite");
        return written;
    }

    Status write_all(const void *buffer, std::size_t bytes) override {
        const auto *pointer = static_cast<const std::uint8_t *>(buffer);
        std::size_t remaining = bytes;
        while (remaining > 0) {
            const auto result = write(pointer, remaining);
            if (!result.ok())
                return result.error();
            if (result.value() == 0)
                return make_error(ErrorCode::IoError, 0, "write made no progress");
            pointer += result.value();
            remaining -= result.value();
        }
        return Status{};
    }

    std::uint64_t size() const override {
        if (file_ == nullptr)
            return 0;
        const std::int64_t position = portable_ftell(file_);
        if (position < 0)
            return 0;
        if (!portable_fseek(file_, 0, SEEK_END))
            return 0;
        const std::int64_t end = portable_ftell(file_);
        (void)portable_fseek(file_, position, SEEK_SET);
        return end < 0 ? 0 : static_cast<std::uint64_t>(end);
    }

    std::uint64_t position() const override {
        if (file_ == nullptr)
            return 0;
        const std::int64_t position = portable_ftell(file_);
        return position < 0 ? 0 : static_cast<std::uint64_t>(position);
    }

    Status seek(std::int64_t offset, SeekOrigin origin) override {
        if (file_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "file is not open");
        int native = SEEK_SET;
        switch (origin) {
        case SeekOrigin::Begin:
            native = SEEK_SET;
            break;
        case SeekOrigin::Current:
            native = SEEK_CUR;
            break;
        case SeekOrigin::End:
            native = SEEK_END;
            break;
        }
        if (!portable_fseek(file_, offset, native))
            return portable_last_error(ErrorCode::IoError, "fseek");
        return Status{};
    }

    Status flush() override {
        if (file_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "file is not open");
        if (std::fflush(file_) != 0)
            return portable_last_error(ErrorCode::IoError, "fflush");
        return Status{};
    }

    Status truncate(std::uint64_t size) override {
        if (file_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "file is not open");
        if (std::fflush(file_) != 0)
            return portable_last_error(ErrorCode::IoError, "fflush");
        if (!portable_truncate(file_, static_cast<std::int64_t>(size)))
            return portable_last_error(ErrorCode::IoError, "truncate");
        return Status{};
    }

    void *native_handle() noexcept override {
        return file_;
    }

  private:
    std::FILE *file_{nullptr};
    SystemError error_;
};

// ---------------------------------------------------------------------------
// Directory iterator
// ---------------------------------------------------------------------------
class PortableDirectoryIterator final : public IDirectoryIterator {
  public:
    explicit PortableDirectoryIterator(const std::string &path) {
        if (path.empty()) {
            error_ = make_error(ErrorCode::InvalidArgument, 0, "directory path is empty");
            return;
        }
        std::error_code error;
        const fs::path native = to_path(path);
        if (!fs::is_directory(native, error)) {
            if (error)
                error_ = portable_error(error, ErrorCode::IoError, "directory_iterator");
            return;
        }
        iterator_ = fs::directory_iterator(native, error);
        if (error) {
            error_ = portable_error(error, ErrorCode::IoError, "directory_iterator");
            return;
        }
        valid_ = true;
    }

    bool next(FileInfo &entry, SystemError &error) override {
        if (!valid_) {
            error = error_;
            return false;
        }
        if (iterator_ == fs::directory_iterator{})
            return false;
        std::error_code code;
        fill_file_info(iterator_->path(), entry);
        iterator_.increment(code);
        if (code) {
            error = portable_error(code, ErrorCode::IoError, "directory_iterator::increment");
            valid_ = false;
        }
        return true;
    }

  private:
    fs::directory_iterator iterator_;
    bool valid_{false};
    SystemError error_;
};

// ---------------------------------------------------------------------------
// Polling directory watcher
// ---------------------------------------------------------------------------
namespace {

struct WatchedEntry {
    std::uint64_t size{0};
    std::int64_t modified{0};
    bool is_directory{false};

    bool operator==(const WatchedEntry &other) const noexcept {
        return size == other.size && modified == other.modified &&
               is_directory == other.is_directory;
    }
};

using Snapshot = std::map<std::string, WatchedEntry>;

Snapshot take_snapshot(const fs::path &root, bool recursive) {
    Snapshot snapshot;
    std::error_code error;
    const auto add_entry = [&snapshot, &error](const fs::directory_entry &entry) {
        WatchedEntry value;
        value.is_directory = entry.is_directory(error);
        const std::uintmax_t size = entry.file_size(error);
        value.size = error ? 0 : static_cast<std::uint64_t>(size);
        error.clear();
        const auto time = entry.last_write_time(error);
        value.modified = error ? 0
                               : static_cast<std::int64_t>(
                                     std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         time.time_since_epoch())
                                         .count());
        error.clear();
        snapshot[portable_path_to_utf8(entry.path())] = value;
    };

    if (recursive) {
        fs::recursive_directory_iterator iterator(
            root, fs::directory_options::skip_permission_denied, error);
        if (error)
            return snapshot;
        const fs::recursive_directory_iterator end;
        for (; iterator != end; iterator.increment(error)) {
            if (error)
                break;
            add_entry(*iterator);
        }
    } else {
        fs::directory_iterator iterator(root, error);
        if (error)
            return snapshot;
        const fs::directory_iterator end;
        for (; iterator != end; iterator.increment(error)) {
            if (error)
                break;
            add_entry(*iterator);
        }
    }
    return snapshot;
}

} // namespace

class PortableFileWatcher final : public IFileWatcher {
  public:
    PortableFileWatcher(const std::string &path, bool recursive)
        : root_(to_path(path)), recursive_(recursive) {
        std::error_code error;
        if (!fs::is_directory(root_, error)) {
            error_ = error ? portable_error(error, ErrorCode::IoError, "watch_directory")
                           : make_error(ErrorCode::NotFound, 0, "watch path is not a directory");
            return;
        }
        snapshot_ = take_snapshot(root_, recursive_);
        valid_ = true;
    }

    bool valid() const noexcept {
        return valid_;
    }

    const SystemError &error() const noexcept {
        return error_;
    }

    bool wait_for_change(Nanoseconds timeout) override {
        if (!valid_)
            return false;
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            if (poll())
                return true;
            if (timeout == 0)
                return false;
            if (timeout > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count();
                if (elapsed >= timeout)
                    return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }

    void drain(std::vector<FileChangeEvent> &events) override {
        events.insert(events.end(), std::make_move_iterator(pending_.begin()),
                      std::make_move_iterator(pending_.end()));
        pending_.clear();
    }

  private:
    bool poll() {
        Snapshot next = take_snapshot(root_, recursive_);
        std::vector<FileChangeEvent> changes;
        for (const auto &[path, value] : next) {
            const auto found = snapshot_.find(path);
            if (found == snapshot_.end()) {
                FileChangeEvent event;
                event.kind = FileChangeKind::Added;
                event.path = path;
                changes.push_back(std::move(event));
            } else if (!(found->second == value)) {
                FileChangeEvent event;
                event.kind = FileChangeKind::Modified;
                event.path = path;
                changes.push_back(std::move(event));
            }
        }
        for (const auto &[path, value] : snapshot_) {
            if (next.find(path) != next.end())
                continue;
            FileChangeEvent event;
            event.kind = FileChangeKind::Removed;
            event.path = path;
            changes.push_back(std::move(event));
        }
        snapshot_ = std::move(next);
        if (changes.empty())
            return false;
        pending_.insert(pending_.end(), std::make_move_iterator(changes.begin()),
                        std::make_move_iterator(changes.end()));
        return true;
    }

    fs::path root_;
    bool recursive_{false};
    bool valid_{false};
    Snapshot snapshot_;
    std::vector<FileChangeEvent> pending_;
    SystemError error_;
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IFileStream>> make_portable_file_stream(const std::string &path,
                                                               const OpenOptions &options) {
    auto stream = std::make_unique<PortableFileStream>(path, options);
    if (!stream->valid())
        return stream->error();
    return std::unique_ptr<IFileStream>(std::move(stream));
}

Result<std::unique_ptr<IDirectoryIterator>>
make_portable_directory_iterator(const std::string &path) {
    return std::unique_ptr<IDirectoryIterator>(std::make_unique<PortableDirectoryIterator>(path));
}

Result<std::unique_ptr<IFileWatcher>> make_portable_file_watcher(const std::string &path,
                                                                 bool recursive) {
    auto watcher = std::make_unique<PortableFileWatcher>(path, recursive);
    if (!watcher->valid())
        return watcher->error();
    return std::unique_ptr<IFileWatcher>(std::move(watcher));
}

// ---------------------------------------------------------------------------
// PortableSystem filesystem entry points
// ---------------------------------------------------------------------------
Result<FileInfo> PortableSystem::stat(const std::string &path) {
    if (path.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "path is empty");
    const fs::path native = to_path(path);
    std::error_code error;
    const fs::file_status status = fs::symlink_status(native, error);
    if (error)
        return portable_error(error, ErrorCode::IoError, "stat");
    if (status.type() == fs::file_type::not_found)
        return make_error(ErrorCode::NotFound, 0, "path does not exist");
    FileInfo info;
    fill_file_info(native, info);
    return info;
}

bool PortableSystem::exists(const std::string &path) {
    if (path.empty())
        return false;
    std::error_code error;
    return fs::exists(to_path(path), error) && !error;
}

Status PortableSystem::create_directory(const std::string &path, bool recursive) {
    if (path.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "directory path is empty");
    const fs::path native = to_path(path);
    std::error_code error;
    if (recursive) {
        fs::create_directories(native, error);
        if (error)
            return portable_error(error, ErrorCode::IoError, "create_directories");
        return Status{};
    }
    if (fs::exists(native, error)) {
        if (!error && fs::is_directory(native, error))
            return Status{};
        return make_error(ErrorCode::AlreadyExists, 0, "path exists and is not a directory");
    }
    fs::create_directory(native, error);
    if (error)
        return portable_error(error, ErrorCode::IoError, "create_directory");
    return Status{};
}

Status PortableSystem::remove_file(const std::string &path) {
    if (path.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "file path is empty");
    const fs::path native = to_path(path);
    std::error_code error;
    if (!fs::remove(native, error)) {
        if (error)
            return portable_error(error, ErrorCode::IoError, "remove");
        return make_error(ErrorCode::NotFound, 0, "file does not exist");
    }
    if (error)
        return portable_error(error, ErrorCode::IoError, "remove");
    return Status{};
}

Status PortableSystem::remove_directory(const std::string &path, bool recursive) {
    if (path.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "directory path is empty");
    const fs::path native = to_path(path);
    std::error_code error;
    if (recursive) {
        const std::uintmax_t removed = fs::remove_all(native, error);
        if (error)
            return portable_error(error, ErrorCode::IoError, "remove_all");
        if (removed == 0)
            return make_error(ErrorCode::NotFound, 0, "directory does not exist");
        return Status{};
    }
    if (!fs::remove(native, error)) {
        if (error)
            return portable_error(error, ErrorCode::IoError, "remove");
        return make_error(ErrorCode::NotFound, 0, "directory does not exist");
    }
    return Status{};
}

Status PortableSystem::rename(const std::string &from, const std::string &to) {
    if (from.empty() || to.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "rename path is empty");
    std::error_code error;
    fs::rename(to_path(from), to_path(to), error);
    if (error)
        return portable_error(error, ErrorCode::IoError, "rename");
    return Status{};
}

Status PortableSystem::copy_file(const std::string &from, const std::string &to, bool overwrite) {
    if (from.empty() || to.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "copy path is empty");
    std::error_code error;
    fs::copy_file(to_path(from), to_path(to),
                  overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none, error);
    if (error)
        return portable_error(error, ErrorCode::IoError, "copy_file");
    return Status{};
}

Status PortableSystem::set_current_directory(const std::string &path) {
    if (path.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "directory path is empty");
    std::error_code error;
    fs::current_path(to_path(path), error);
    if (error)
        return portable_error(error, ErrorCode::IoError, "current_path");
    return Status{};
}

Result<std::string> PortableSystem::create_temp_file(const std::string &prefix) {
    std::error_code error;
    const fs::path directory = fs::temp_directory_path(error);
    if (error)
        return portable_error(error, ErrorCode::IoError, "temp_directory_path");

    std::string safe_prefix = prefix.empty() ? "shinkou" : prefix;
    for (char &character : safe_prefix) {
        const bool valid = (character >= 'a' && character <= 'z') ||
                           (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') || character == '_';
        if (!valid)
            character = '_';
    }

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 64; ++attempt) {
        const std::string name =
            safe_prefix + "_" + std::to_string(stamp) + "_" + std::to_string(attempt) + ".tmp";
        const fs::path candidate = directory / to_path(name);
        std::FILE *file = portable_fopen(portable_path_to_utf8(candidate), "w+b");
        if (file != nullptr) {
            std::fclose(file);
            return portable_path_to_utf8(candidate);
        }
    }
    return make_error(ErrorCode::IoError, 0, "could not create a temporary file");
}

} // namespace shinkou::platform::detail

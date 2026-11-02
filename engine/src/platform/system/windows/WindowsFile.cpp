// Windows native files, directories and directory-change notifications.

#include "WindowsBackend.h"

#include "WindowsCommon.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace shinkou::platform::detail {
namespace {

std::wstring normalize_wide_path(const std::string &path) {
    std::wstring wide = wide_from_utf8(path);
    for (wchar_t &character : wide) {
        if (character == L'/')
            character = L'\\';
    }
    return wide;
}

std::string join_path(const std::string &base, const std::string &name) {
    if (base.empty())
        return name;
    if (base.back() == '\\' || base.back() == '/')
        return base + name;
    return base + "\\" + name;
}

FileInfo make_file_info(const std::string &base, const WIN32_FIND_DATAW &data) {
    FileInfo info;
    info.name = utf8_from_wide(data.cFileName);
    info.path = join_path(base, info.name);
    info.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    info.is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.is_symlink = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    info.is_read_only = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    info.created_nanoseconds = unix_nanoseconds_from_filetime(data.ftCreationTime);
    info.modified_nanoseconds = unix_nanoseconds_from_filetime(data.ftLastWriteTime);
    info.accessed_nanoseconds = unix_nanoseconds_from_filetime(data.ftLastAccessTime);
    return info;
}

FileInfo make_file_info(const std::string &path, const WIN32_FILE_ATTRIBUTE_DATA &data) {
    FileInfo info;
    info.path = path;
    const std::size_t separator = path.find_last_of("\\/");
    info.name = separator == std::string::npos ? path : path.substr(separator + 1);
    info.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    info.is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.is_symlink = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    info.is_read_only = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    info.created_nanoseconds = unix_nanoseconds_from_filetime(data.ftCreationTime);
    info.modified_nanoseconds = unix_nanoseconds_from_filetime(data.ftLastWriteTime);
    info.accessed_nanoseconds = unix_nanoseconds_from_filetime(data.ftLastAccessTime);
    return info;
}

bool is_existing_directory(const std::wstring &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

Status create_directory_tree(const std::wstring &path) {
    // Create every prefix in order. Paths that already exist as directories
    // count as success, matching the recursive contract.
    for (std::size_t index = 0; index <= path.size(); ++index) {
        const bool at_end = index == path.size();
        const bool separator = !at_end && (path[index] == L'\\' || path[index] == L'/');
        if (!at_end && !separator)
            continue;
        std::wstring prefix = path.substr(0, index);
        // Skip a drive root such as "C:" (but not "C:\").
        if (prefix.size() == 2 && prefix[1] == L':')
            continue;
        if (prefix.empty())
            continue;
        if (!CreateDirectoryW(prefix.c_str(), nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_ALREADY_EXISTS && is_existing_directory(prefix))
                continue;
            return win32_error(error, ErrorCode::IoError, "CreateDirectoryW");
        }
    }
    return Status{};
}

Status remove_directory_tree(const std::wstring &path) {
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = path + L"\\*";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..")
                continue;
            const std::wstring child = path + L"\\" + name;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    // Delete the junction/link itself, never its target.
                    if (!RemoveDirectoryW(child.c_str()))
                        return win32_error(ErrorCode::IoError, "RemoveDirectoryW(link)");
                } else {
                    const Status nested = remove_directory_tree(child);
                    if (!nested.ok())
                        return nested;
                }
            } else {
                if (!DeleteFileW(child.c_str())) {
                    const DWORD error = GetLastError();
                    if (error != ERROR_FILE_NOT_FOUND)
                        return win32_error(error, ErrorCode::IoError, "DeleteFileW");
                }
            }
        } while (FindNextFileW(find, &data) != FALSE);
        FindClose(find);
    }
    if (!RemoveDirectoryW(path.c_str())) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND)
            return win32_error(error, ErrorCode::IoError, "RemoveDirectoryW");
    }
    return Status{};
}

} // namespace

// ---------------------------------------------------------------------------
// File stream
// ---------------------------------------------------------------------------
class WindowsFileStream final : public IFileStream {
  public:
    WindowsFileStream(const std::string &path, const OpenOptions &options) {
        const std::wstring wide_path = normalize_wide_path(path);
        if (wide_path.empty()) {
            error_ = make_error(ErrorCode::InvalidArgument, 0, "file path is empty");
            return;
        }

        DWORD desired_access = 0;
        switch (options.mode) {
        case OpenMode::Read:
            desired_access = GENERIC_READ;
            break;
        case OpenMode::Write:
        case OpenMode::Append:
            desired_access = GENERIC_WRITE;
            break;
        case OpenMode::ReadWrite:
            desired_access = GENERIC_READ | GENERIC_WRITE;
            break;
        }

        DWORD creation = OPEN_EXISTING;
        if (options.truncate && options.mode != OpenMode::Read)
            creation = CREATE_ALWAYS;
        else if (options.create_if_missing && options.mode != OpenMode::Read)
            creation = OPEN_ALWAYS;

        DWORD sharing = 0;
        switch (options.share) {
        case ShareMode::None:
            sharing = 0;
            break;
        case ShareMode::Read:
            sharing = FILE_SHARE_READ;
            break;
        case ShareMode::ReadWrite:
            sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
            break;
        case ShareMode::ReadWriteDelete:
            sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
            break;
        }

        const DWORD attributes = options.write_through ? FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH
                                                       : FILE_ATTRIBUTE_NORMAL;
        handle_ = CreateFileW(wide_path.c_str(), desired_access, sharing, nullptr, creation,
                              attributes, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            handle_ = nullptr;
            error_ = win32_error(ErrorCode::IoError, "CreateFileW");
            return;
        }
        if (options.mode == OpenMode::Append) {
            LARGE_INTEGER distance{};
            LARGE_INTEGER position{};
            distance.QuadPart = 0;
            if (!SetFilePointerEx(handle_, distance, &position, FILE_END)) {
                error_ = win32_error(ErrorCode::IoError, "SetFilePointerEx(append)");
                CloseHandle(handle_);
                handle_ = nullptr;
            }
        }
    }

    ~WindowsFileStream() override {
        if (handle_ != nullptr)
            CloseHandle(handle_);
    }

    bool valid() const noexcept {
        return handle_ != nullptr;
    }

    const SystemError &error() const noexcept {
        return error_;
    }

    Result<std::size_t> read(void *buffer, std::size_t bytes) override {
        if (handle_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "file is not open");
        if (bytes > 0xFFFFFFFFu)
            return make_error(ErrorCode::InvalidArgument, 0, "read size exceeds 4 GiB");
        DWORD read = 0;
        if (!ReadFile(handle_, buffer, static_cast<DWORD>(bytes), &read, nullptr))
            return win32_error(ErrorCode::IoError, "ReadFile");
        return static_cast<std::size_t>(read);
    }

    Result<std::size_t> write(const void *buffer, std::size_t bytes) override {
        if (handle_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "file is not open");
        if (bytes > 0xFFFFFFFFu)
            return make_error(ErrorCode::InvalidArgument, 0, "write size exceeds 4 GiB");
        DWORD written = 0;
        if (!WriteFile(handle_, buffer, static_cast<DWORD>(bytes), &written, nullptr))
            return win32_error(ErrorCode::IoError, "WriteFile");
        return static_cast<std::size_t>(written);
    }

    Status write_all(const void *buffer, std::size_t bytes) override {
        const auto *bytes_pointer = static_cast<const std::uint8_t *>(buffer);
        std::size_t remaining = bytes;
        while (remaining > 0) {
            const auto result = write(bytes_pointer, remaining);
            if (!result.ok())
                return result.error();
            if (result.value() == 0)
                return make_error(ErrorCode::IoError, 0, "write made no progress");
            bytes_pointer += result.value();
            remaining -= result.value();
        }
        return Status{};
    }

    std::uint64_t size() const override {
        if (handle_ == nullptr)
            return 0;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle_, &size))
            return 0;
        return static_cast<std::uint64_t>(size.QuadPart);
    }

    std::uint64_t position() const override {
        if (handle_ == nullptr)
            return 0;
        LARGE_INTEGER distance{};
        LARGE_INTEGER position{};
        if (!SetFilePointerEx(handle_, distance, &position, FILE_CURRENT))
            return 0;
        return static_cast<std::uint64_t>(position.QuadPart);
    }

    Status seek(std::int64_t offset, SeekOrigin origin) override {
        if (handle_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "file is not open");
        LARGE_INTEGER distance{};
        distance.QuadPart = offset;
        DWORD method = FILE_BEGIN;
        switch (origin) {
        case SeekOrigin::Begin:
            method = FILE_BEGIN;
            break;
        case SeekOrigin::Current:
            method = FILE_CURRENT;
            break;
        case SeekOrigin::End:
            method = FILE_END;
            break;
        }
        if (!SetFilePointerEx(handle_, distance, nullptr, method))
            return win32_error(ErrorCode::IoError, "SetFilePointerEx");
        return Status{};
    }

    Status flush() override {
        if (handle_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "file is not open");
        if (!FlushFileBuffers(handle_))
            return win32_error(ErrorCode::IoError, "FlushFileBuffers");
        return Status{};
    }

    Status truncate(std::uint64_t size) override {
        if (handle_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "file is not open");
        LARGE_INTEGER distance{};
        distance.QuadPart = static_cast<LONGLONG>(size);
        if (!SetFilePointerEx(handle_, distance, nullptr, FILE_BEGIN))
            return win32_error(ErrorCode::IoError, "SetFilePointerEx(truncate)");
        if (!SetEndOfFile(handle_))
            return win32_error(ErrorCode::IoError, "SetEndOfFile");
        return Status{};
    }

    void *native_handle() noexcept override {
        return handle_;
    }

  private:
    HANDLE handle_{nullptr};
    SystemError error_;
};

// ---------------------------------------------------------------------------
// Directory iterator
// ---------------------------------------------------------------------------
class WindowsDirectoryIterator final : public IDirectoryIterator {
  public:
    explicit WindowsDirectoryIterator(const std::string &path) : base_(path) {
        const std::wstring wide = normalize_wide_path(path);
        if (wide.empty()) {
            error_ = make_error(ErrorCode::InvalidArgument, 0, "directory path is empty");
            return;
        }
        std::wstring pattern = wide;
        while (pattern.size() > 1 && (pattern.back() == L'\\' || pattern.back() == L'/'))
            pattern.pop_back();
        pattern += L"\\*";
        find_ = FindFirstFileW(pattern.c_str(), &data_);
        if (find_ == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND)
                error_ = win32_error(error, ErrorCode::IoError, "FindFirstFileW");
        }
    }

    ~WindowsDirectoryIterator() override {
        if (find_ != INVALID_HANDLE_VALUE)
            FindClose(find_);
    }

    bool next(FileInfo &entry, SystemError &error) override {
        if (find_ == INVALID_HANDLE_VALUE) {
            error = error_;
            return false;
        }
        for (;;) {
            if (!pending_first_) {
                if (!FindNextFileW(find_, &data_)) {
                    const DWORD code = GetLastError();
                    if (code != ERROR_NO_MORE_FILES)
                        error = win32_error(code, ErrorCode::IoError, "FindNextFileW");
                    return false;
                }
            }
            pending_first_ = false;
            if (std::wstring(data_.cFileName) == L"." || std::wstring(data_.cFileName) == L"..")
                continue;
            entry = make_file_info(base_, data_);
            return true;
        }
    }

  private:
    HANDLE find_{INVALID_HANDLE_VALUE};
    WIN32_FIND_DATAW data_{};
    std::string base_;
    bool pending_first_{true};
    SystemError error_;
};

// ---------------------------------------------------------------------------
// Directory watcher
// ---------------------------------------------------------------------------
class WindowsFileWatcher final : public IFileWatcher {
  public:
    WindowsFileWatcher(const std::string &path, bool recursive) : base_(path), recursive_(recursive) {
        const std::wstring wide = normalize_wide_path(path);
        if (wide.empty()) {
            error_ = make_error(ErrorCode::InvalidArgument, 0, "watch path is empty");
            return;
        }
        directory_ = CreateFileW(wide.c_str(), FILE_LIST_DIRECTORY,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                 nullptr);
        if (directory_ == INVALID_HANDLE_VALUE) {
            directory_ = nullptr;
            error_ = win32_error(ErrorCode::IoError, "CreateFileW(watch)");
            return;
        }
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        change_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        notify_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (stop_event_ == nullptr || change_event_ == nullptr || notify_event_ == nullptr) {
            error_ = win32_error(ErrorCode::SystemFailure, "CreateEventW(watch)");
            close_handles();
            return;
        }
        thread_ = CreateThread(nullptr, 0, &WindowsFileWatcher::entry, this, 0, nullptr);
        if (thread_ == nullptr) {
            error_ = win32_error(ErrorCode::SystemFailure, "CreateThread(watch)");
            close_handles();
        }
    }

    ~WindowsFileWatcher() override {
        if (stop_event_ != nullptr)
            SetEvent(stop_event_);
        if (thread_ != nullptr) {
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        close_handles();
    }

    bool valid() const noexcept {
        return directory_ != nullptr && thread_ != nullptr;
    }

    const SystemError &error() const noexcept {
        return error_;
    }

    bool wait_for_change(Nanoseconds timeout) override {
        if (notify_event_ == nullptr)
            return false;
        return WaitForSingleObject(notify_event_, win32_timeout(timeout)) == WAIT_OBJECT_0;
    }

    void drain(std::vector<FileChangeEvent> &events) override {
        std::lock_guard<std::mutex> lock(mutex_);
        events.insert(events.end(), std::make_move_iterator(pending_.begin()),
                      std::make_move_iterator(pending_.end()));
        pending_.clear();
    }

  private:
    static DWORD WINAPI entry(LPVOID parameter) {
        static_cast<WindowsFileWatcher *>(parameter)->run();
        return 0;
    }

    void run() {
        OVERLAPPED overlapped{};
        overlapped.hEvent = change_event_;
        std::vector<std::uint8_t> buffer(64 * 1024);
        constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                  FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
        for (;;) {
            DWORD returned = 0;
            const BOOL issued =
                ReadDirectoryChangesW(directory_, buffer.data(), static_cast<DWORD>(buffer.size()),
                                      recursive_ ? TRUE : FALSE, kFilter, &returned, &overlapped,
                                      nullptr);
            if (!issued && GetLastError() != ERROR_IO_PENDING)
                break;

            // ReadDirectoryChangesW may return nonzero while the overlapped
            // operation is still pending, so always wait on the event; a
            // synchronous completion signals it immediately.
            HANDLE handles[2] = {overlapped.hEvent, stop_event_};
            const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0 + 1) {
                (void)CancelIoEx(directory_, &overlapped);
                break;
            }
            if (wait != WAIT_OBJECT_0)
                break;
            if (!GetOverlappedResult(directory_, &overlapped, &returned, FALSE)) {
                const DWORD error = GetLastError();
                if (error == ERROR_OPERATION_ABORTED)
                    break;
                ResetEvent(overlapped.hEvent);
                continue;
            }
            ResetEvent(overlapped.hEvent);
            if (returned == 0) {
                // Overflow: the kernel filled the buffer and discarded entries.
                // Report a synthetic modification so pollers rescan.
                publish_rescan();
                continue;
            }
            parse(buffer.data(), returned);
        }
    }

    void publish_rescan() {
        FileChangeEvent event;
        event.kind = FileChangeKind::Modified;
        event.path = base_;
        publish(std::move(event));
    }

    void parse(const std::uint8_t *data, DWORD size) {
        std::size_t offset = 0;
        std::string pending_old_path;
        while (offset + sizeof(FILE_NOTIFY_INFORMATION) <= size) {
            const auto *info = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(data + offset);
            const std::wstring wide_name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            const std::string name = utf8_from_wide(wide_name);
            switch (info->Action) {
            case FILE_ACTION_ADDED:
                publish(make_event(FileChangeKind::Added, name, {}));
                break;
            case FILE_ACTION_REMOVED:
                publish(make_event(FileChangeKind::Removed, name, {}));
                break;
            case FILE_ACTION_MODIFIED:
                publish(make_event(FileChangeKind::Modified, name, {}));
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                pending_old_path = name;
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                publish(make_event(FileChangeKind::Renamed, name, pending_old_path));
                pending_old_path.clear();
                break;
            default:
                break;
            }
            if (info->NextEntryOffset == 0)
                break;
            offset += info->NextEntryOffset;
        }
    }

    FileChangeEvent make_event(FileChangeKind kind, const std::string &name,
                               const std::string &previous) const {
        FileChangeEvent event;
        event.kind = kind;
        event.path = join_path(base_, name);
        event.previous_path = previous.empty() ? previous : join_path(base_, previous);
        return event;
    }

    void publish(FileChangeEvent event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.push_back(std::move(event));
        }
        if (notify_event_ != nullptr)
            SetEvent(notify_event_);
    }

    void close_handles() noexcept {
        if (directory_ != nullptr) {
            CloseHandle(directory_);
            directory_ = nullptr;
        }
        if (stop_event_ != nullptr) {
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
        }
        if (change_event_ != nullptr) {
            CloseHandle(change_event_);
            change_event_ = nullptr;
        }
        if (notify_event_ != nullptr) {
            CloseHandle(notify_event_);
            notify_event_ = nullptr;
        }
    }

    HANDLE directory_{nullptr};
    HANDLE stop_event_{nullptr};
    HANDLE change_event_{nullptr};
    HANDLE notify_event_{nullptr};
    HANDLE thread_{nullptr};
    std::string base_;
    bool recursive_{false};
    std::mutex mutex_;
    std::vector<FileChangeEvent> pending_;
    SystemError error_;
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IFileStream>> make_windows_file_stream(const std::string &path,
                                                              const OpenOptions &options) {
    auto stream = std::make_unique<WindowsFileStream>(path, options);
    if (!stream->valid())
        return stream->error();
    return std::unique_ptr<IFileStream>(std::move(stream));
}

Result<std::unique_ptr<IDirectoryIterator>>
make_windows_directory_iterator(const std::string &path) {
    auto iterator = std::make_unique<WindowsDirectoryIterator>(path);
    return std::unique_ptr<IDirectoryIterator>(std::move(iterator));
}

Result<std::unique_ptr<IFileWatcher>> make_windows_file_watcher(const std::string &path,
                                                                bool recursive) {
    auto watcher = std::make_unique<WindowsFileWatcher>(path, recursive);
    if (!watcher->valid())
        return watcher->error();
    return std::unique_ptr<IFileWatcher>(std::move(watcher));
}

// ---------------------------------------------------------------------------
// WindowsSystem entry points
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IFileStream>> WindowsSystem::open_file(const std::string &path,
                                                              const OpenOptions &options) {
    return make_windows_file_stream(path, options);
}

Result<std::unique_ptr<IDirectoryIterator>>
WindowsSystem::enumerate_directory(const std::string &path) {
    return make_windows_directory_iterator(path);
}

Result<FileInfo> WindowsSystem::stat(const std::string &path) {
    const std::wstring wide = normalize_wide_path(path);
    if (wide.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "path is empty");
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data))
        return win32_error(ErrorCode::IoError, "GetFileAttributesExW");
    return make_file_info(path, data);
}

bool WindowsSystem::exists(const std::string &path) {
    const std::wstring wide = normalize_wide_path(path);
    if (wide.empty())
        return false;
    return GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

Status WindowsSystem::create_directory(const std::string &path, bool recursive) {
    const std::wstring wide = normalize_wide_path(path);
    if (wide.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "directory path is empty");
    if (recursive)
        return create_directory_tree(wide);
    if (CreateDirectoryW(wide.c_str(), nullptr))
        return Status{};
    const DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS && is_existing_directory(wide))
        return Status{};
    return win32_error(error, ErrorCode::IoError, "CreateDirectoryW");
}

Status WindowsSystem::remove_file(const std::string &path) {
    const std::wstring wide = normalize_wide_path(path);
    if (wide.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "file path is empty");
    if (!DeleteFileW(wide.c_str())) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND)
            return make_error(ErrorCode::NotFound, static_cast<std::int32_t>(error),
                              "file does not exist");
        return win32_error(error, ErrorCode::IoError, "DeleteFileW");
    }
    return Status{};
}

Status WindowsSystem::remove_directory(const std::string &path, bool recursive) {
    const std::wstring wide = normalize_wide_path(path);
    if (wide.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "directory path is empty");
    if (recursive)
        return remove_directory_tree(wide);
    if (!RemoveDirectoryW(wide.c_str())) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return make_error(ErrorCode::NotFound, static_cast<std::int32_t>(error),
                              "directory does not exist");
        return win32_error(error, ErrorCode::IoError, "RemoveDirectoryW");
    }
    return Status{};
}

Status WindowsSystem::rename(const std::string &from, const std::string &to) {
    const std::wstring wide_from = normalize_wide_path(from);
    const std::wstring wide_to = normalize_wide_path(to);
    if (wide_from.empty() || wide_to.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "rename path is empty");
    if (!MoveFileExW(wide_from.c_str(), wide_to.c_str(), MOVEFILE_REPLACE_EXISTING))
        return win32_error(ErrorCode::IoError, "MoveFileExW");
    return Status{};
}

Status WindowsSystem::copy_file(const std::string &from, const std::string &to, bool overwrite) {
    const std::wstring wide_from = normalize_wide_path(from);
    const std::wstring wide_to = normalize_wide_path(to);
    if (wide_from.empty() || wide_to.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "copy path is empty");
    if (!CopyFileW(wide_from.c_str(), wide_to.c_str(), overwrite ? FALSE : TRUE)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
            return make_error(ErrorCode::AlreadyExists, static_cast<std::int32_t>(error),
                              "destination already exists");
        return win32_error(error, ErrorCode::IoError, "CopyFileW");
    }
    return Status{};
}

std::string WindowsSystem::current_directory() const {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0)
            return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            return utf8_from_wide(buffer);
        }
        buffer.resize(length + 1);
    }
}

Status WindowsSystem::set_current_directory(const std::string &path) {
    const std::wstring wide = normalize_wide_path(path);
    if (wide.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "directory path is empty");
    if (!SetCurrentDirectoryW(wide.c_str()))
        return win32_error(ErrorCode::IoError, "SetCurrentDirectoryW");
    return Status{};
}

std::string WindowsSystem::temp_directory() const {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0)
            return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            std::string result = utf8_from_wide(buffer);
            while (result.size() > 3 && (result.back() == '\\' || result.back() == '/'))
                result.pop_back();
            return result;
        }
        buffer.resize(length + 1);
    }
}

Result<std::string> WindowsSystem::create_temp_file(const std::string &prefix) {
    std::wstring directory(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetTempPathW(static_cast<DWORD>(directory.size()), directory.data());
        if (length == 0)
            return win32_error(ErrorCode::IoError, "GetTempPathW");
        if (length < directory.size()) {
            directory.resize(length);
            break;
        }
        directory.resize(length + 1);
    }

    std::string safe_prefix = prefix.empty() ? "shinkou" : prefix;
    for (char &character : safe_prefix) {
        const bool valid = (character >= 'a' && character <= 'z') ||
                           (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') || character == '_';
        if (!valid)
            character = '_';
    }
    if (safe_prefix.size() > 3)
        safe_prefix.resize(3);
    const std::wstring wide_prefix = wide_from_utf8(safe_prefix);

    for (int attempt = 0; attempt < 16; ++attempt) {
        wchar_t name[MAX_PATH]{};
        if (GetTempFileNameW(directory.c_str(), wide_prefix.c_str(), 0, name) == 0)
            return win32_error(ErrorCode::IoError, "GetTempFileNameW");
        return utf8_from_wide(name);
    }
    return make_error(ErrorCode::IoError, 0, "could not create a temporary file");
}

Result<std::unique_ptr<IFileWatcher>>
WindowsSystem::watch_directory(const std::string &path, bool recursive) {
    return make_windows_file_watcher(path, recursive);
}

} // namespace shinkou::platform::detail

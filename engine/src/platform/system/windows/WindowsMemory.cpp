// Windows kernel virtual memory: reservations, page protection, page locking,
// file mappings and named shared memory.

#include "WindowsBackend.h"

#include "WindowsCommon.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace shinkou::platform::detail {
namespace {

DWORD win32_protection(MemoryProtection protection) noexcept {
    const auto flags = static_cast<std::uint32_t>(protection);
    constexpr auto read = static_cast<std::uint32_t>(MemoryProtection::Read);
    constexpr auto write = static_cast<std::uint32_t>(MemoryProtection::Write);
    constexpr auto execute = static_cast<std::uint32_t>(MemoryProtection::Execute);
    if ((flags & (read | write | execute)) == 0)
        return PAGE_NOACCESS;
    if (flags & execute) {
        if ((flags & write) != 0)
            return PAGE_EXECUTE_READWRITE;
        if ((flags & read) != 0)
            return PAGE_EXECUTE_READ;
        return PAGE_EXECUTE;
    }
    if ((flags & write) != 0)
        return PAGE_READWRITE;
    return PAGE_READONLY;
}

std::size_t system_page_size() {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwPageSize == 0 ? 4096 : info.dwPageSize;
}

std::size_t system_granularity() {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwAllocationGranularity == 0 ? 65536 : info.dwAllocationGranularity;
}

// Rounds `bytes` up to `alignment`; returns 0 when the value would overflow.
std::size_t round_up(std::size_t bytes, std::size_t alignment) {
    if (alignment == 0)
        return bytes;
    const std::size_t remainder = bytes % alignment;
    if (remainder == 0)
        return bytes;
    const std::size_t padding = alignment - remainder;
    if (bytes > (std::numeric_limits<std::size_t>::max)() - padding)
        return 0;
    return bytes + padding;
}

} // namespace

// ---------------------------------------------------------------------------
// Reserved virtual address range
// ---------------------------------------------------------------------------
class WindowsReservedRegion final : public IReservedRegion {
  public:
    WindowsReservedRegion(std::size_t bytes, MemoryFlags flags) {
        const std::size_t page = system_page_size();
        const std::size_t rounded = round_up(bytes, page);
        if (bytes == 0 || rounded == 0) {
            error_ = make_error(ErrorCode::InvalidArgument, 0, "reservation size is invalid");
            return;
        }
        size_ = rounded;

        const bool wants_large_pages =
            (static_cast<std::uint32_t>(flags) & static_cast<std::uint32_t>(MemoryFlags::LargePages)) !=
            0;
        if (wants_large_pages) {
            const std::size_t large_page = GetLargePageMinimum();
            if (large_page != 0) {
                const std::size_t large_size = round_up(bytes, large_page);
                base_ = VirtualAlloc(nullptr, large_size, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
                                     PAGE_READWRITE);
                if (base_ != nullptr) {
                    size_ = large_size;
                    committed_ = large_size;
                    large_pages_ = true;
                    return;
                }
            }
            // Fall through to a normal reservation; the caller can inspect
            // uses_large_pages().
        }

        DWORD allocation_type = MEM_RESERVE;
        if ((static_cast<std::uint32_t>(flags) &
             static_cast<std::uint32_t>(MemoryFlags::CommitOnReserve)) != 0)
            allocation_type |= MEM_COMMIT;
        base_ = VirtualAlloc(nullptr, size_, allocation_type, PAGE_READWRITE);
        if (base_ == nullptr) {
            error_ = win32_error(ErrorCode::OutOfMemory, "VirtualAlloc(reserve)");
            return;
        }
        if ((allocation_type & MEM_COMMIT) != 0)
            committed_ = size_;
    }

    ~WindowsReservedRegion() override {
        (void)release();
    }

    bool valid() const noexcept {
        return base_ != nullptr;
    }

    const SystemError &error() const noexcept {
        return error_;
    }

    void *address() noexcept override {
        return base_;
    }

    std::size_t size() const noexcept override {
        return size_;
    }

    std::size_t committed_size() const noexcept override {
        return committed_;
    }

    bool uses_large_pages() const noexcept override {
        return large_pages_;
    }

    Status commit(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "region was not reserved");
        if (!in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "commit range is outside the reservation");
        if (VirtualAlloc(static_cast<std::uint8_t *>(base_) + offset, bytes, MEM_COMMIT,
                         PAGE_READWRITE) == nullptr)
            return win32_error(ErrorCode::OutOfMemory, "VirtualAlloc(commit)");
        committed_ += round_up(bytes, system_page_size());
        if (committed_ > size_)
            committed_ = size_;
        return Status{};
    }

    Status decommit(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "region was not reserved");
        if (!in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "decommit range is outside the reservation");
        if (!VirtualFree(static_cast<std::uint8_t *>(base_) + offset, bytes, MEM_DECOMMIT))
            return win32_error(ErrorCode::IoError, "VirtualFree(decommit)");
        const std::size_t pages = round_up(bytes, system_page_size());
        committed_ = committed_ > pages ? committed_ - pages : 0;
        return Status{};
    }

    Status protect(std::size_t offset, std::size_t bytes,
                   MemoryProtection protection) override {
        if (base_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "region was not reserved");
        if (!in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "protect range is outside the reservation");
        DWORD previous = 0;
        if (!VirtualProtect(static_cast<std::uint8_t *>(base_) + offset, bytes,
                            win32_protection(protection), &previous))
            return win32_error(ErrorCode::AccessDenied, "VirtualProtect");
        return Status{};
    }

    Status lock(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr || !in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "lock range is outside the reservation");
        if (!VirtualLock(static_cast<std::uint8_t *>(base_) + offset, bytes))
            return win32_error(ErrorCode::AccessDenied, "VirtualLock");
        return Status{};
    }

    Status unlock(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr || !in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "unlock range is outside the reservation");
        if (!VirtualUnlock(static_cast<std::uint8_t *>(base_) + offset, bytes))
            return win32_error(ErrorCode::AccessDenied, "VirtualUnlock");
        return Status{};
    }

    Status flush_instruction_cache(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr || !in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "flush range is outside the reservation");
        if (!FlushInstructionCache(GetCurrentProcess(), static_cast<std::uint8_t *>(base_) + offset,
                                   bytes))
            return win32_error(ErrorCode::SystemFailure, "FlushInstructionCache");
        return Status{};
    }

    Status release() override {
        if (base_ == nullptr)
            return Status{};
        if (!VirtualFree(base_, 0, MEM_RELEASE)) {
            // The region stays owned so a later call can retry.
            return win32_error(ErrorCode::SystemFailure, "VirtualFree(release)");
        }
        base_ = nullptr;
        committed_ = 0;
        size_ = 0;
        large_pages_ = false;
        return Status{};
    }

    void *native_handle() noexcept override {
        return base_;
    }

  private:
    bool in_range(std::size_t offset, std::size_t bytes) const noexcept {
        return bytes != 0 && offset <= size_ && bytes <= size_ - offset;
    }

    void *base_{nullptr};
    std::size_t size_{0};
    std::size_t committed_{0};
    bool large_pages_{false};
    SystemError error_;
};

// ---------------------------------------------------------------------------
// File and shared memory mappings
// ---------------------------------------------------------------------------
class WindowsMappedRegion final : public IMappedRegion {
  public:
    WindowsMappedRegion(void *data, std::size_t size, bool writable, HANDLE mapping)
        : data_(data), size_(size), writable_(writable), mapping_(mapping) {
    }

    ~WindowsMappedRegion() override {
        release();
    }

    void *data() noexcept override {
        return data_;
    }

    std::size_t size() const noexcept override {
        return size_;
    }

    bool writable() const noexcept override {
        return writable_;
    }

    Status flush() override {
        if (data_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "mapping is not active");
        if (!FlushViewOfFile(data_, size_))
            return win32_error(ErrorCode::IoError, "FlushViewOfFile");
        return Status{};
    }

    void *native_handle() noexcept override {
        return data_;
    }

  private:
    void release() noexcept {
        if (data_ != nullptr) {
            (void)UnmapViewOfFile(data_);
            data_ = nullptr;
        }
        if (mapping_ != nullptr) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
    }

    void *data_{nullptr};
    std::size_t size_{0};
    bool writable_{false};
    HANDLE mapping_{nullptr};
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IReservedRegion>> make_windows_reserved_region(std::size_t bytes,
                                                                      MemoryFlags flags) {
    auto region = std::make_unique<WindowsReservedRegion>(bytes, flags);
    if (!region->valid())
        return region->error();
    return std::unique_ptr<IReservedRegion>(std::move(region));
}

Result<std::unique_ptr<IMappedRegion>> make_windows_file_mapping(const std::string &path,
                                                                 std::size_t offset,
                                                                 std::size_t bytes,
                                                                 MemoryProtection protection) {
    if (path.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "mapping path is empty");
    const std::size_t granularity = system_granularity();
    if (offset % granularity != 0)
        return make_error(ErrorCode::InvalidArgument, 0,
                          "file mapping offset must be a multiple of the allocation granularity");

    const bool writable = (static_cast<std::uint32_t>(protection) &
                           static_cast<std::uint32_t>(MemoryProtection::Write)) != 0;
    const std::wstring wide_path = wide_from_utf8(path);
    if (wide_path.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "mapping path is not valid UTF-8");

    const DWORD access = GENERIC_READ | (writable ? GENERIC_WRITE : 0);
    HANDLE file = CreateFileW(wide_path.c_str(), access, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return win32_error(ErrorCode::IoError, "CreateFileW");

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file, &file_size)) {
        const SystemError error = win32_error(ErrorCode::IoError, "GetFileSizeEx");
        CloseHandle(file);
        return error;
    }
    const std::uint64_t total = static_cast<std::uint64_t>(file_size.QuadPart);
    if (static_cast<std::uint64_t>(offset) >= total) {
        CloseHandle(file);
        return make_error(ErrorCode::OutOfRange, 0, "mapping offset is past the end of the file");
    }
    const std::uint64_t available = total - static_cast<std::uint64_t>(offset);
    const std::uint64_t requested = bytes == 0 ? available : std::min<std::uint64_t>(bytes, available);
    if (requested == 0) {
        CloseHandle(file);
        return make_error(ErrorCode::InvalidArgument, 0, "mapping size is zero");
    }

    const DWORD page_protection = writable ? PAGE_READWRITE : PAGE_READONLY;
    const DWORD view_access = writable ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ;
    HANDLE mapping = CreateFileMappingW(file, nullptr, page_protection, 0, 0, nullptr);
    if (mapping == nullptr) {
        const SystemError error = win32_error(ErrorCode::IoError, "CreateFileMappingW");
        CloseHandle(file);
        return error;
    }

    void *data = MapViewOfFile(mapping, view_access, static_cast<DWORD>(static_cast<std::uint64_t>(offset) >> 32),
                               static_cast<DWORD>(static_cast<std::uint64_t>(offset) & 0xFFFFFFFFu),
                               static_cast<std::size_t>(requested));
    const SystemError error = data == nullptr ? win32_error(ErrorCode::IoError, "MapViewOfFile")
                                              : SystemError{};
    CloseHandle(file);
    if (data == nullptr) {
        CloseHandle(mapping);
        return error;
    }
    return std::unique_ptr<IMappedRegion>(
        new WindowsMappedRegion(data, static_cast<std::size_t>(requested), writable, mapping));
}

Result<std::unique_ptr<IMappedRegion>> make_windows_shared_memory(const std::string &name,
                                                                 std::size_t bytes,
                                                                 MemoryProtection protection,
                                                                 bool create) {
    if (name.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "shared memory name is empty");
    if (bytes == 0)
        return make_error(ErrorCode::InvalidArgument, 0, "shared memory size is zero");

    const bool writable = (static_cast<std::uint32_t>(protection) &
                           static_cast<std::uint32_t>(MemoryProtection::Write)) != 0;
    const std::wstring wide_name = wide_from_utf8(name);
    if (wide_name.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "shared memory name is not valid UTF-8");

    const DWORD page_protection = writable ? PAGE_READWRITE : PAGE_READONLY;
    const DWORD view_access = writable ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ;

    HANDLE mapping = nullptr;
    if (create) {
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, page_protection,
                                     static_cast<DWORD>(static_cast<std::uint64_t>(bytes) >> 32),
                                     static_cast<DWORD>(bytes & 0xFFFFFFFFu), wide_name.c_str());
        if (mapping == nullptr)
            return win32_error(ErrorCode::SystemFailure, "CreateFileMappingW(shared)");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(mapping);
            return make_error(ErrorCode::AlreadyExists, 0, "shared memory name is already in use");
        }
    } else {
        mapping = OpenFileMappingW(view_access, FALSE, wide_name.c_str());
        if (mapping == nullptr)
            return win32_error(ErrorCode::NotFound, "OpenFileMappingW");
    }

    void *data = MapViewOfFile(mapping, view_access, 0, 0, bytes);
    if (data == nullptr) {
        const SystemError error = win32_error(ErrorCode::SystemFailure, "MapViewOfFile(shared)");
        CloseHandle(mapping);
        return error;
    }
    return std::unique_ptr<IMappedRegion>(
        new WindowsMappedRegion(data, bytes, writable, mapping));
}

// ---------------------------------------------------------------------------
// WindowsSystem entry points
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IReservedRegion>> WindowsSystem::reserve_memory(std::size_t bytes,
                                                                       MemoryFlags flags) {
    return make_windows_reserved_region(bytes, flags);
}

Result<std::unique_ptr<IMappedRegion>> WindowsSystem::map_file(const std::string &path,
                                                               std::size_t offset,
                                                               std::size_t bytes,
                                                               MemoryProtection protection) {
    return make_windows_file_mapping(path, offset, bytes, protection);
}

Result<std::unique_ptr<IMappedRegion>>
WindowsSystem::create_shared_memory(const std::string &name, std::size_t bytes,
                                    MemoryProtection protection) {
    return make_windows_shared_memory(name, bytes, protection, true);
}

Result<std::unique_ptr<IMappedRegion>> WindowsSystem::open_shared_memory(
    const std::string &name, std::size_t bytes, MemoryProtection protection) {
    return make_windows_shared_memory(name, bytes, protection, false);
}

} // namespace shinkou::platform::detail

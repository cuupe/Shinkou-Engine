// Portable virtual memory: reserve/commit/protect/lock on top of the standard
// allocator plus the platform's page-protection and instruction-cache calls.

#include "PortableBackend.h"

#include "PortableCommon.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace shinkou::platform::detail {
namespace {

constexpr std::size_t round_up_to(std::size_t value, std::size_t alignment) {
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

} // namespace

class PortableReservedRegion final : public IReservedRegion {
  public:
    PortableReservedRegion(std::size_t bytes, MemoryFlags flags) {
        if (bytes == 0) {
            error_ = make_error(ErrorCode::InvalidArgument, 0, "reservation size is zero");
            return;
        }
        if ((static_cast<std::uint32_t>(flags) &
             static_cast<std::uint32_t>(MemoryFlags::LargePages)) != 0) {
            error_ = make_error(ErrorCode::NotSupported, 0,
                                "large pages are not available in the portable backend");
            return;
        }
        page_ = portable_page_size();
        size_ = round_up_to(bytes, page_);
        base_ = ::operator new(size_, std::align_val_t{page_});
        if (base_ == nullptr) {
            error_ = make_error(ErrorCode::OutOfMemory, 0, "aligned allocation failed");
            return;
        }
        committed_ = (static_cast<std::uint32_t>(flags) &
                      static_cast<std::uint32_t>(MemoryFlags::CommitOnReserve)) != 0
                         ? size_
                         : 0;
    }

    ~PortableReservedRegion() override {
        release_storage();
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
        return false;
    }

    Status commit(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr || !in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "commit range is outside the reservation");
        committed_ = committed_ > offset + bytes ? committed_ : offset + bytes;
        return Status{};
    }

    Status decommit(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr || !in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "decommit range is outside the reservation");
        committed_ = committed_ > offset ? offset : 0;
        return Status{};
    }

    Status protect(std::size_t offset, std::size_t bytes, MemoryProtection protection) override {
        if (base_ == nullptr || !in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "protect range is outside the reservation");
        auto *address = static_cast<std::uint8_t *>(base_) + offset;
        const std::size_t rounded = round_up_to(bytes, page_);
#if defined(SHINKOU_PLATFORM_WINDOWS)
        DWORD previous = 0;
        if (!VirtualProtect(address, rounded, win32_page_protection(protection), &previous))
            return win32_error(ErrorCode::AccessDenied, "VirtualProtect");
#else
        const auto flags = static_cast<std::uint32_t>(protection);
        const bool read = (flags & static_cast<std::uint32_t>(MemoryProtection::Read)) != 0;
        const bool write = (flags & static_cast<std::uint32_t>(MemoryProtection::Write)) != 0;
        const bool execute = (flags & static_cast<std::uint32_t>(MemoryProtection::Execute)) != 0;
        int native = PROT_NONE;
        if (read && write)
            native = PROT_READ | PROT_WRITE;
        else if (read)
            native = PROT_READ;
        else if (write)
            native = PROT_WRITE;
        if (execute)
            native |= PROT_EXEC;
        if (mprotect(address, rounded, native) != 0)
            return portable_last_error(ErrorCode::AccessDenied, "mprotect");
#endif
        return Status{};
    }

    Status lock(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr || !in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "lock range is outside the reservation");
        auto *address = static_cast<std::uint8_t *>(base_) + offset;
#if defined(SHINKOU_PLATFORM_WINDOWS)
        if (!VirtualLock(address, bytes))
            return win32_error(ErrorCode::AccessDenied, "VirtualLock");
#else
        if (mlock(address, bytes) != 0)
            return portable_last_error(ErrorCode::AccessDenied, "mlock");
#endif
        return Status{};
    }

    Status unlock(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr || !in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "unlock range is outside the reservation");
        auto *address = static_cast<std::uint8_t *>(base_) + offset;
#if defined(SHINKOU_PLATFORM_WINDOWS)
        if (!VirtualUnlock(address, bytes))
            return win32_error(ErrorCode::AccessDenied, "VirtualUnlock");
#else
        if (munlock(address, bytes) != 0)
            return portable_last_error(ErrorCode::AccessDenied, "munlock");
#endif
        return Status{};
    }

    Status flush_instruction_cache(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr || !in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0,
                              "instruction-cache range is outside the reservation");
        auto *address = static_cast<std::uint8_t *>(base_) + offset;
#if defined(SHINKOU_PLATFORM_WINDOWS)
        if (!FlushInstructionCache(GetCurrentProcess(), address, bytes))
            return win32_error(ErrorCode::SystemFailure, "FlushInstructionCache");
#elif defined(SHINKOU_COMPILER_GCC) || defined(SHINKOU_COMPILER_CLANG)
        __builtin___clear_cache(reinterpret_cast<char *>(address),
                                reinterpret_cast<char *>(address) + bytes);
#else
        (void)address;
        (void)bytes;
#endif
        return Status{};
    }

    Status release() override {
        release_storage();
        return Status{};
    }

    void *native_handle() noexcept override {
        return base_;
    }

  private:
#if defined(SHINKOU_PLATFORM_WINDOWS)
    static DWORD win32_page_protection(MemoryProtection protection) noexcept {
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
#endif

    bool in_range(std::size_t offset, std::size_t bytes) const noexcept {
        return bytes != 0 && offset <= size_ && bytes <= size_ - offset;
    }

    void release_storage() noexcept {
        if (base_ == nullptr)
            return;
        ::operator delete(base_, std::align_val_t{page_});
        base_ = nullptr;
        size_ = 0;
        committed_ = 0;
    }

    void *base_{nullptr};
    std::size_t size_{0};
    std::size_t committed_{0};
    std::size_t page_{4096};
    bool commit_on_reserve_{false};
    SystemError error_;
};

Result<std::unique_ptr<IReservedRegion>> make_portable_reserved_region(std::size_t bytes,
                                                                       MemoryFlags flags) {
    auto region = std::make_unique<PortableReservedRegion>(bytes, flags);
    if (!region->valid())
        return region->error();
    return std::unique_ptr<IReservedRegion>(std::move(region));
}

} // namespace shinkou::platform::detail

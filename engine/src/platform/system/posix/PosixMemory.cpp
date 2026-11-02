// POSIX kernel virtual memory: mmap reservations, file mappings and named
// shared memory (shm_open).

#include "PosixConfig.h"

#include "PosixBackend.h"
#include "PosixCommon.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

namespace shinkou::platform::detail {
namespace {

std::size_t round_up_to_page(std::size_t value, std::size_t page) {
    return page == 0 ? value : (value + page - 1) / page * page;
}

int protection_flags(MemoryProtection protection) {
    const auto flags = static_cast<std::uint32_t>(protection);
    int native = 0;
    if ((flags & static_cast<std::uint32_t>(MemoryProtection::Read)) != 0)
        native |= PROT_READ;
    if ((flags & static_cast<std::uint32_t>(MemoryProtection::Write)) != 0)
        native |= PROT_WRITE;
    if ((flags & static_cast<std::uint32_t>(MemoryProtection::Execute)) != 0)
        native |= PROT_EXEC;
    return native;
}

bool writable(MemoryProtection protection) {
    return (static_cast<std::uint32_t>(protection) &
            static_cast<std::uint32_t>(MemoryProtection::Write)) != 0;
}

class PosixReservedRegion final : public IReservedRegion {
  public:
    PosixReservedRegion(std::size_t bytes, MemoryFlags flags) {
        if (bytes == 0) {
            error_ = make_error(ErrorCode::InvalidArgument, 0, "reservation size is zero");
            return;
        }
        if ((static_cast<std::uint32_t>(flags) &
             static_cast<std::uint32_t>(MemoryFlags::LargePages)) != 0) {
            error_ = make_error(ErrorCode::NotSupported, 0,
                                "large pages are not available in the POSIX backend");
            return;
        }
        page_ = posix_page_size();
        size_ = round_up_to_page(bytes, page_);
        void *region = mmap(nullptr, size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (region == MAP_FAILED) {
            error_ = posix_last_error(ErrorCode::OutOfMemory, "mmap(reserve)");
            return;
        }
        base_ = region;
        if ((static_cast<std::uint32_t>(flags) &
             static_cast<std::uint32_t>(MemoryFlags::CommitOnReserve)) != 0) {
            if (mprotect(base_, size_, PROT_READ | PROT_WRITE) != 0) {
                error_ = posix_last_error(ErrorCode::OutOfMemory, "mprotect(commit)");
                (void)munmap(base_, size_);
                base_ = nullptr;
                size_ = 0;
                return;
            }
            committed_ = size_;
        }
    }

    ~PosixReservedRegion() override {
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
        return false;
    }

    Status commit(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "region was not reserved");
        if (!in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "commit range is outside the reservation");
        if (offset % page_ != 0)
            return make_error(ErrorCode::InvalidArgument, 0, "commit offset is not page aligned");
        const std::size_t length = round_up_to_page(bytes, page_);
        if (mprotect(static_cast<std::uint8_t *>(base_) + offset, length,
                     PROT_READ | PROT_WRITE) != 0)
            return posix_last_error(ErrorCode::OutOfMemory, "mprotect(commit)");
        committed_ = std::max(committed_, offset + length);
        if (committed_ > size_)
            committed_ = size_;
        return Status{};
    }

    Status decommit(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "region was not reserved");
        if (!in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "decommit range is outside the reservation");
        if (offset % page_ != 0)
            return make_error(ErrorCode::InvalidArgument, 0, "decommit offset is not page aligned");
        const std::size_t length = round_up_to_page(bytes, page_);
        auto *address = static_cast<std::uint8_t *>(base_) + offset;
#if defined(MADV_DONTNEED)
        (void)madvise(address, length, MADV_DONTNEED);
#elif defined(MADV_FREE)
        (void)madvise(address, length, MADV_FREE);
#endif
        if (mprotect(address, length, PROT_NONE) != 0)
            return posix_last_error(ErrorCode::SystemFailure, "mprotect(decommit)");
        committed_ = committed_ > offset ? offset : 0;
        return Status{};
    }

    Status protect(std::size_t offset, std::size_t bytes, MemoryProtection protection) override {
        if (base_ == nullptr)
            return make_error(ErrorCode::InvalidArgument, 0, "region was not reserved");
        if (!in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "protect range is outside the reservation");
        if (offset % page_ != 0)
            return make_error(ErrorCode::InvalidArgument, 0, "protect offset is not page aligned");
        const std::size_t length = round_up_to_page(bytes, page_);
        if (mprotect(static_cast<std::uint8_t *>(base_) + offset, length,
                     protection_flags(protection)) != 0)
            return posix_last_error(ErrorCode::AccessDenied, "mprotect(protect)");
        return Status{};
    }

    Status lock(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr || !in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "lock range is outside the reservation");
        if (mlock(static_cast<std::uint8_t *>(base_) + offset, bytes) != 0)
            return posix_last_error(ErrorCode::AccessDenied, "mlock");
        return Status{};
    }

    Status unlock(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr || !in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0, "unlock range is outside the reservation");
        if (munlock(static_cast<std::uint8_t *>(base_) + offset, bytes) != 0)
            return posix_last_error(ErrorCode::AccessDenied, "munlock");
        return Status{};
    }

    Status flush_instruction_cache(std::size_t offset, std::size_t bytes) override {
        if (base_ == nullptr || !in_range(offset, bytes))
            return make_error(ErrorCode::OutOfRange, 0,
                              "instruction-cache range is outside the reservation");
#if defined(__GNUC__) || defined(__clang__)
        auto *start = static_cast<char *>(base_) + offset;
        __builtin___clear_cache(start, start + bytes);
#else
        (void)offset;
        (void)bytes;
#endif
        return Status{};
    }

    Status release() override {
        if (base_ == nullptr)
            return Status{};
        if (munmap(base_, size_) != 0)
            return posix_last_error(ErrorCode::SystemFailure, "munmap");
        base_ = nullptr;
        size_ = 0;
        committed_ = 0;
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
    std::size_t page_{4096};
    SystemError error_;
};

class PosixMappedRegion final : public IMappedRegion {
  public:
    PosixMappedRegion(void *data, std::size_t size, bool writable, bool owns_shared_name,
                      std::string shared_name)
        : data_(data),
          size_(size),
          writable_(writable),
          owns_shared_name_(owns_shared_name),
          shared_name_(std::move(shared_name)) {
    }

    ~PosixMappedRegion() override {
        if (data_ != nullptr)
            (void)munmap(data_, size_);
        if (owns_shared_name_ && !shared_name_.empty())
            (void)shm_unlink(shared_name_.c_str());
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
        const std::size_t page = posix_page_size();
        const std::size_t length = round_up_to_page(size_, page);
        if (msync(data_, length, MS_SYNC) != 0)
            return posix_last_error(ErrorCode::IoError, "msync");
        return Status{};
    }

    void *native_handle() noexcept override {
        return data_;
    }

  private:
    void *data_{nullptr};
    std::size_t size_{0};
    bool writable_{false};
    bool owns_shared_name_{false};
    std::string shared_name_;
};

} // namespace

Result<std::unique_ptr<IReservedRegion>> make_posix_reserved_region(std::size_t bytes,
                                                                    MemoryFlags flags) {
    auto region = std::make_unique<PosixReservedRegion>(bytes, flags);
    if (!region->valid())
        return region->error();
    return std::unique_ptr<IReservedRegion>(std::move(region));
}

Result<std::unique_ptr<IMappedRegion>> make_posix_file_mapping(const std::string &path,
                                                               std::size_t offset,
                                                               std::size_t bytes,
                                                               MemoryProtection protection) {
    if (path.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "mapping path is empty");
    const std::size_t page = posix_page_size();
    if (offset % page != 0)
        return make_error(ErrorCode::InvalidArgument, 0,
                          "mapping offset must be a multiple of the page size");

    const bool write = writable(protection);
    const int descriptor = open(path.c_str(), write ? O_RDWR : O_RDONLY);
    if (descriptor < 0)
        return posix_last_error(ErrorCode::IoError, "open");

    struct stat status {};
    if (fstat(descriptor, &status) != 0) {
        const SystemError error = posix_last_error(ErrorCode::IoError, "fstat");
        close(descriptor);
        return error;
    }
    const std::uint64_t file_size =
        status.st_size > 0 ? static_cast<std::uint64_t>(status.st_size) : 0;
    if (static_cast<std::uint64_t>(offset) >= file_size) {
        close(descriptor);
        return make_error(ErrorCode::OutOfRange, 0, "mapping offset is past the end of the file");
    }
    const std::uint64_t available = file_size - static_cast<std::uint64_t>(offset);
    const std::uint64_t requested =
        bytes == 0 ? available : std::min<std::uint64_t>(bytes, available);
    if (requested == 0) {
        close(descriptor);
        return make_error(ErrorCode::InvalidArgument, 0, "mapping size is zero");
    }

    void *data = mmap(nullptr, static_cast<std::size_t>(requested), protection_flags(protection),
                      MAP_SHARED, descriptor, static_cast<off_t>(offset));
    const int mapping_error = errno;
    close(descriptor);
    if (data == MAP_FAILED)
        return posix_error(mapping_error, ErrorCode::IoError, "mmap(file)");
    return std::unique_ptr<IMappedRegion>(new PosixMappedRegion(
        data, static_cast<std::size_t>(requested), write, false, std::string{}));
}

Result<std::unique_ptr<IMappedRegion>> make_posix_shared_memory(const std::string &name,
                                                                std::size_t bytes,
                                                                MemoryProtection protection,
                                                                bool create) {
    if (name.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "shared memory name is empty");
    if (name.front() != '/')
        return make_error(ErrorCode::InvalidArgument, 0,
                          "POSIX shared memory names must start with '/'");
    if (name.find('/', 1) != std::string::npos)
        return make_error(ErrorCode::InvalidArgument, 0,
                          "POSIX shared memory names must not contain a second '/'");
    if (bytes == 0)
        return make_error(ErrorCode::InvalidArgument, 0, "shared memory size is zero");

    const bool write = writable(protection);
    const int flags = create ? (O_CREAT | O_EXCL | O_RDWR) : (write ? O_RDWR : O_RDONLY);
    const int descriptor = shm_open(name.c_str(), flags, 0600);
    if (descriptor < 0) {
        const int code = errno;
        if (create && code == EEXIST)
            return make_error(ErrorCode::AlreadyExists, code,
                              "shared memory name is already in use");
        return posix_error(code, create ? ErrorCode::SystemFailure : ErrorCode::NotFound,
                           "shm_open");
    }

    if (create) {
        if (ftruncate(descriptor, static_cast<off_t>(bytes)) != 0) {
            const SystemError error = posix_last_error(ErrorCode::SystemFailure, "ftruncate");
            close(descriptor);
            (void)shm_unlink(name.c_str());
            return error;
        }
    } else {
        struct stat status {};
        if (fstat(descriptor, &status) != 0) {
            const SystemError error = posix_last_error(ErrorCode::IoError, "fstat(shared)");
            close(descriptor);
            return error;
        }
        if (status.st_size <= 0 || static_cast<std::uint64_t>(status.st_size) < bytes) {
            close(descriptor);
            return make_error(ErrorCode::OutOfRange, 0,
                              "requested size exceeds the shared memory segment");
        }
    }

    void *data = mmap(nullptr, bytes, protection_flags(protection), MAP_SHARED, descriptor, 0);
    const int mapping_error = errno;
    close(descriptor);
    if (data == MAP_FAILED)
        return posix_error(mapping_error, ErrorCode::IoError, "mmap(shared)");
    return std::unique_ptr<IMappedRegion>(
        new PosixMappedRegion(data, bytes, write, create, create ? name : std::string{}));
}

// ---------------------------------------------------------------------------
// PosixSystem memory entry points
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IReservedRegion>> PosixSystem::reserve_memory(std::size_t bytes,
                                                                     MemoryFlags flags) {
    return make_posix_reserved_region(bytes, flags);
}

Result<std::unique_ptr<IMappedRegion>> PosixSystem::map_file(const std::string &path,
                                                             std::size_t offset,
                                                             std::size_t bytes,
                                                             MemoryProtection protection) {
    return make_posix_file_mapping(path, offset, bytes, protection);
}

Result<std::unique_ptr<IMappedRegion>>
PosixSystem::create_shared_memory(const std::string &name, std::size_t bytes,
                                  MemoryProtection protection) {
    return make_posix_shared_memory(name, bytes, protection, true);
}

Result<std::unique_ptr<IMappedRegion>>
PosixSystem::open_shared_memory(const std::string &name, std::size_t bytes,
                                MemoryProtection protection) {
    return make_posix_shared_memory(name, bytes, protection, false);
}

} // namespace shinkou::platform::detail

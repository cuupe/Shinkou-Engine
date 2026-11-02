#pragma once

// Shinkou Engine - kernel virtual-memory services.
//
// The engine allocators are ordinary heap allocators; this interface is for the
// cases where the process must talk to the kernel's virtual-memory manager:
// large aligned reservations, guard pages, executable JIT pages, page locking
// for latency-sensitive buffers and memory-mapped files.
//
// A reservation does not touch the address space until commit() is called, and
// commit/decommit can be repeated. release() hands the whole range back; the
// destructor does it automatically.

#include "shinkou/platform/Error.h"
#include "shinkou/platform/Platform.h"
#include "shinkou/platform/SystemInfo.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace shinkou::platform {

enum class MemoryProtection : std::uint32_t {
    None = 0,
    Read = 1u << 0,
    Write = 1u << 1,
    Execute = 1u << 2,
    ReadWrite = Read | Write,
    ReadExecute = Read | Execute,
    ReadWriteExecute = Read | Write | Execute,
};

constexpr MemoryProtection operator|(MemoryProtection left, MemoryProtection right) noexcept {
    return static_cast<MemoryProtection>(static_cast<std::uint32_t>(left) |
                                         static_cast<std::uint32_t>(right));
}

enum class MemoryFlags : std::uint32_t {
    None = 0,
    // Try the kernel's large-page pool (2 MiB+). Requires the caller/process to
    // hold the corresponding privilege; backends fall back to normal pages and
    // report it through IReservedRegion::uses_large_pages().
    LargePages = 1u << 0,
    // Commit while reserving, so callers that never decommit keep the simple
    // "allocate bytes, get usable memory" shape.
    CommitOnReserve = 1u << 1,
};

constexpr MemoryFlags operator|(MemoryFlags left, MemoryFlags right) noexcept {
    return static_cast<MemoryFlags>(static_cast<std::uint32_t>(left) |
                                    static_cast<std::uint32_t>(right));
}

// A reserved virtual address range owned by the backend. Offsets and sizes are
// byte counts that must be page-aligned unless the backend documents a looser
// rounding; the Windows backend rounds down/up to the page size.
class IReservedRegion {
  public:
    virtual ~IReservedRegion() = default;

    virtual void *address() noexcept = 0;
    virtual std::size_t size() const noexcept = 0;            // reserved bytes
    // Sum of committed requests; overlapping commits are not deduplicated, so
    // this is an upper bound rather than an exact page count.
    virtual std::size_t committed_size() const noexcept = 0;
    virtual bool uses_large_pages() const noexcept = 0;

    virtual Status commit(std::size_t offset, std::size_t bytes) = 0;
    virtual Status decommit(std::size_t offset, std::size_t bytes) = 0;
    virtual Status protect(std::size_t offset, std::size_t bytes,
                           MemoryProtection protection) = 0;

    // Pins committed pages in physical memory (VirtualLock / mlock).
    virtual Status lock(std::size_t offset, std::size_t bytes) = 0;
    virtual Status unlock(std::size_t offset, std::size_t bytes) = 0;

    // Flushes generated code from the CPU instruction cache (JIT support).
    virtual Status flush_instruction_cache(std::size_t offset, std::size_t bytes) = 0;

    // Releases the reservation early; the destructor does the same.
    virtual Status release() = 0;

    virtual void *native_handle() noexcept = 0;
};

// A view of a file or a named shared mapping.
class IMappedRegion {
  public:
    virtual ~IMappedRegion() = default;

    virtual void *data() noexcept = 0;
    virtual std::size_t size() const noexcept = 0;
    virtual bool writable() const noexcept = 0;

    // Writes dirty pages back to the backing file / mapping.
    virtual Status flush() = 0;

    virtual void *native_handle() noexcept = 0;
};

} // namespace shinkou::platform

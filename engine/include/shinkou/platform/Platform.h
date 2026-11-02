#pragma once

// Shinkou Engine - portable platform detection and low-level primitives.
//
// This header is the root of the platform abstraction. It carries only
// compile-time facts (OS, architecture, compiler, endianness) plus a handful
// of header-only helpers, so it can be included by every other platform
// header without pulling in an OS SDK. Nothing here opens a window or calls
// the kernel; the runtime services live behind shinkou/platform/System.h.
//
// The macros are usually supplied by CMake for the engine target. They are
// derived from the compiler predefines as a fallback so standalone tools and
// tests compile without extra definitions.

// clang-format off
#if !defined(SHINKOU_PLATFORM_WINDOWS) && defined(_WIN32) && !defined(__CYGWIN__)
#define SHINKOU_PLATFORM_WINDOWS 1
#endif
#if !defined(SHINKOU_PLATFORM_LINUX) && defined(__linux__) && !defined(__ANDROID__)
#define SHINKOU_PLATFORM_LINUX 1
#endif
#if !defined(SHINKOU_PLATFORM_ANDROID) && defined(__ANDROID__)
#define SHINKOU_PLATFORM_ANDROID 1
#endif
#if !defined(SHINKOU_PLATFORM_APPLE) && defined(__APPLE__)
#define SHINKOU_PLATFORM_APPLE 1
#endif
#if !defined(SHINKOU_PLATFORM_BSD) && (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__))
#define SHINKOU_PLATFORM_BSD 1
#endif
#if !defined(SHINKOU_PLATFORM_EMSCRIPTEN) && defined(__EMSCRIPTEN__)
#define SHINKOU_PLATFORM_EMSCRIPTEN 1
#endif
#if !defined(SHINKOU_PLATFORM_UNIX) && (defined(SHINKOU_PLATFORM_LINUX) || defined(SHINKOU_PLATFORM_APPLE) || defined(SHINKOU_PLATFORM_ANDROID) || defined(SHINKOU_PLATFORM_BSD))
#define SHINKOU_PLATFORM_UNIX 1
#endif
// The native POSIX backend relies on ucontext, pthreads and dl_iterate_phdr;
// Android and Emscripten keep the portable backend instead.
#if !defined(SHINKOU_PLATFORM_HAS_POSIX_BACKEND) && defined(SHINKOU_PLATFORM_UNIX) && !defined(SHINKOU_PLATFORM_ANDROID) && !defined(SHINKOU_PLATFORM_EMSCRIPTEN)
#define SHINKOU_PLATFORM_HAS_POSIX_BACKEND 1
#endif

#if defined(_M_X64) || defined(__x86_64__)
#define SHINKOU_ARCH_X64 1
#elif defined(_M_IX86) || defined(__i386__)
#define SHINKOU_ARCH_X86 1
#elif defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
#define SHINKOU_ARCH_ARM64 1
#elif defined(_M_ARM) || defined(__arm__)
#define SHINKOU_ARCH_ARM 1
#elif defined(__riscv) && (__riscv_xlen == 64)
#define SHINKOU_ARCH_RISCV64 1
#elif defined(__wasm64__)
#define SHINKOU_ARCH_WASM64 1
#elif defined(__wasm32__) || defined(__EMSCRIPTEN__)
#define SHINKOU_ARCH_WASM32 1
#endif

#if defined(_MSC_VER)
#define SHINKOU_COMPILER_MSVC 1
#elif defined(__clang__)
#define SHINKOU_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define SHINKOU_COMPILER_GCC 1
#endif
// clang-format on

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#if defined(SHINKOU_COMPILER_MSVC)
#include <intrin.h>
#endif

// ---------------------------------------------------------------------------
// Shared-library visibility. The engine is built as a static library today, so
// the default is empty. Consumers that build a shared library can opt in.
// ---------------------------------------------------------------------------
#if defined(SHINKOU_PLATFORM_WINDOWS)
#if defined(SHINKOU_PLATFORM_BUILD_SHARED)
#define SHINKOU_API __declspec(dllexport)
#elif defined(SHINKOU_PLATFORM_USE_SHARED)
#define SHINKOU_API __declspec(dllimport)
#else
#define SHINKOU_API
#endif
#elif defined(SHINKOU_COMPILER_GCC) || defined(SHINKOU_COMPILER_CLANG)
#if defined(SHINKOU_PLATFORM_BUILD_SHARED)
#define SHINKOU_API __attribute__((visibility("default")))
#else
#define SHINKOU_API
#endif
#else
#define SHINKOU_API
#endif

// ---------------------------------------------------------------------------
// Debugging aids.
// ---------------------------------------------------------------------------
#if defined(SHINKOU_COMPILER_MSVC)
#define SHINKOU_BREAK_INTO_DEBUGGER() __debugbreak()
#elif defined(SHINKOU_COMPILER_GCC) || defined(SHINKOU_COMPILER_CLANG)
#define SHINKOU_BREAK_INTO_DEBUGGER() __builtin_trap()
#else
#include <cstdlib>
#define SHINKOU_BREAK_INTO_DEBUGGER() std::abort()
#endif

#if defined(__GNUC__) || defined(__clang__)
#define SHINKOU_LIKELY(condition) (__builtin_expect(!!(condition), 1))
#define SHINKOU_UNLIKELY(condition) (__builtin_expect(!!(condition), 0))
#else
#define SHINKOU_LIKELY(condition) (condition)
#define SHINKOU_UNLIKELY(condition) (condition)
#endif

#define SHINKOU_UNUSED(expression) static_cast<void>(expression)

namespace shinkou::platform {

// Signed count of nanoseconds. Wall-clock and monotonic time points use this
// unit throughout the platform interface; conversion helpers keep call sites
// readable. A negative timeout means "wait forever" where documented.
using Nanoseconds = std::int64_t;

inline constexpr Nanoseconds kNanosecondsPerMicrosecond = 1000;
inline constexpr Nanoseconds kNanosecondsPerMillisecond = 1000 * kNanosecondsPerMicrosecond;
inline constexpr Nanoseconds kNanosecondsPerSecond = 1000 * kNanosecondsPerMillisecond;
inline constexpr Nanoseconds kNanosecondsPerMinute = 60 * kNanosecondsPerSecond;
inline constexpr Nanoseconds kNanosecondsPerHour = 60 * kNanosecondsPerMinute;

// Sentinel for an unbounded wait; used by the synchronization primitives.
inline constexpr Nanoseconds kInfiniteTimeout = -1;

constexpr Nanoseconds seconds_to_nanoseconds(double seconds) noexcept {
    return static_cast<Nanoseconds>(seconds * static_cast<double>(kNanosecondsPerSecond));
}

constexpr double nanoseconds_to_seconds(Nanoseconds value) noexcept {
    return static_cast<double>(value) / static_cast<double>(kNanosecondsPerSecond);
}

constexpr Nanoseconds milliseconds(std::int64_t value) noexcept {
    return value * kNanosecondsPerMillisecond;
}

// ---------------------------------------------------------------------------
// Alignment helpers. The engine's debug allocator and mapped regions use the
// same values as the OS: page size for commit granularity and the allocation
// granularity where a reservation needs one.
// ---------------------------------------------------------------------------
constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

constexpr std::size_t align_down(std::size_t value, std::size_t alignment) noexcept {
    return value / alignment * alignment;
}

template <class T> constexpr bool is_aligned(T *pointer, std::size_t alignment) noexcept {
    return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}

constexpr bool is_power_of_two(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

inline constexpr std::size_t kDefaultAlignment = alignof(std::max_align_t);
inline constexpr std::size_t kCacheLineSize = 64;

// Hints the processor that the calling thread is spinning, without entering
// the kernel. Used by SpinLock and by lock-free queues on the contention path.
inline void cpu_relax() noexcept {
#if defined(SHINKOU_ARCH_X64) || defined(SHINKOU_ARCH_X86)
#if defined(SHINKOU_COMPILER_MSVC)
    _mm_pause();
#elif defined(SHINKOU_COMPILER_GCC) || defined(SHINKOU_COMPILER_CLANG)
    __builtin_ia32_pause();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
#elif defined(SHINKOU_ARCH_ARM64)
#if defined(SHINKOU_COMPILER_MSVC)
    __yield();
#elif defined(SHINKOU_COMPILER_GCC) || defined(SHINKOU_COMPILER_CLANG)
    __asm__ __volatile__("yield");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
#elif defined(SHINKOU_ARCH_ARM)
#if defined(SHINKOU_COMPILER_MSVC)
    __yield();
#elif defined(SHINKOU_COMPILER_GCC) || defined(SHINKOU_COMPILER_CLANG)
    __asm__ __volatile__("yield");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// Endianness. Every engine target today is little-endian; the byte-order
// probe keeps the file formats and hashing code honest if that changes.
#if defined(SHINKOU_PLATFORM_WINDOWS) || defined(_WIN32)
inline constexpr bool kLittleEndian = true;
#elif defined(__BYTE_ORDER__)
inline constexpr bool kLittleEndian = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
#else
inline constexpr bool kLittleEndian = true;
#endif

} // namespace shinkou::platform

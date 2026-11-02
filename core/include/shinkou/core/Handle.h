#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace shinkou::core {
template<class Tag, class Id = std::uint32_t, class Generation = std::uint32_t,
         bool RequireGeneration = true>
struct GenerationalHandle {
    static_assert(std::is_unsigned_v<Id> && std::is_unsigned_v<Generation>);
    Id id{0};
    Generation generation{0};
    explicit operator bool() const noexcept { return id != 0 && (!RequireGeneration || generation != 0); }
    friend bool operator==(GenerationalHandle a, GenerationalHandle b) noexcept {
        return a.id == b.id && a.generation == b.generation;
    }
    friend bool operator!=(GenerationalHandle a, GenerationalHandle b) noexcept { return !(a == b); }
};
template<class Handle> struct HandleHash {
    std::size_t operator()(Handle value) const noexcept {
        const auto id = static_cast<std::size_t>(value.id);
        const auto generation = static_cast<std::size_t>(value.generation);
        return id ^ (generation + static_cast<std::size_t>(0x9e3779b9u) + (id << 6u) + (id >> 2u));
    }
};
// The low field is a one-based index; zero is reserved for invalid handles.
template<class UInt, unsigned IndexBits> struct PackedHandleCodec {
    static_assert(std::is_unsigned_v<UInt> && IndexBits > 0 && IndexBits < std::numeric_limits<UInt>::digits);
    static constexpr UInt indexMask = (UInt{1} << IndexBits) - 1;
    static constexpr UInt generationMask = (std::numeric_limits<UInt>::max)() >> IndexBits;
    static constexpr UInt pack(UInt index, UInt generation) noexcept {
        return ((generation & generationMask) << IndexBits) | ((index + 1) & indexMask);
    }
    static constexpr UInt index(UInt value) noexcept {
        const auto encoded = value & indexMask;
        return encoded ? encoded - 1 : indexMask;
    }
    static constexpr UInt generation(UInt value) noexcept { return (value >> IndexBits) & generationMask; }
};
}

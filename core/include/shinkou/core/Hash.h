#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
namespace shinkou::core {
inline constexpr std::uint64_t fnv1aOffset = 14695981039346656037ull;
inline constexpr std::uint64_t legacyFnvOffset = 1469598103934665603ull;
inline constexpr std::uint64_t fnv1aPrime = 1099511628211ull;
constexpr std::uint64_t hash_string(std::string_view text, std::uint64_t seed = fnv1aOffset) noexcept {
    for (unsigned char byte : text) { seed ^= byte; seed *= fnv1aPrime; }
    return seed;
}
inline std::uint64_t hash_bytes(const void* data, std::size_t size, std::uint64_t seed = fnv1aOffset) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) { seed ^= bytes[i]; seed *= fnv1aPrime; }
    return seed;
}
}

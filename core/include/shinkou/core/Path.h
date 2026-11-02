#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace shinkou::core {
// Persistent/interchange paths are UTF-8 with generic '/' separators. Native
// filesystem paths remain std::filesystem::path until crossing that boundary.
inline std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
inline std::filesystem::path path_from_utf8(std::string_view value) {
    return std::filesystem::u8path(value.begin(), value.end());
}
}

#pragma once

#include <algorithm>
#include <cstdint>

namespace shinkou::renderlib {

struct Size {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};
struct Point { float x = 0.0f; float y = 0.0f; };
struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
    static Color from_rgba8(std::uint32_t value);
    std::uint32_t to_rgba8() const;
};
enum class PixelFormat { RGBA8Unorm };

inline std::uint8_t to_byte(float value) { return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); }

} // namespace shinkou::renderlib


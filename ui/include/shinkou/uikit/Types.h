#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace shinkou::uikit {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
    Vec2() = default;
    Vec2(float xValue, float yValue) : x(xValue), y(yValue) {}
};

struct Size {
    float width = 0.0f;
    float height = 0.0f;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool contains(Vec2 point) const {
        return point.x >= x && point.y >= y && point.x <= x + width && point.y <= y + height;
    }
};

struct Insets {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    Insets() = default;
    explicit Insets(float all) : left(all), top(all), right(all), bottom(all) {}
    Insets(float horizontal, float vertical) : left(horizontal), top(vertical), right(horizontal), bottom(vertical) {}
    Insets(float leftValue, float topValue, float rightValue, float bottomValue) : left(leftValue), top(topValue), right(rightValue), bottom(bottomValue) {}
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    static Color from_rgba8(std::uint32_t value);
    static Color from_hex(const std::string& value);
    std::uint32_t to_rgba8() const;
    bool operator==(const Color& other) const;
};

using WidgetId = std::uint64_t;

enum class LayoutMode { Overlay, Vertical, Horizontal };
enum class Align { Start, Center, End, Stretch };
enum class PointerButton { None, Left, Middle, Right };
enum class UiEventType { PointerMove, PointerDown, PointerUp, PointerEnter, PointerLeave, Wheel, KeyDown, KeyUp, TextInput, Focus, Blur, ValueChanged, Command };
enum class UiEventPhase { Capture, Target, Bubble };

struct UiEvent {
    UiEventType type = UiEventType::PointerMove;
    Vec2 position{};
    Vec2 delta{};
    float wheelDelta = 0.0f;
    PointerButton button = PointerButton::None;
    int key = 0;
    std::string text;
    std::string command;
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    UiEventPhase phase = UiEventPhase::Target;
    WidgetId target = 0;
    WidgetId currentTarget = 0;
    bool handled = false;
    bool stopPropagation = false;
};

inline float clamp01(float value) { return std::max(0.0f, std::min(1.0f, value)); }

} // namespace shinkou::uikit

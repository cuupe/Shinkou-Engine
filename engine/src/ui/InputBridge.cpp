#include "shinkou/ui/InputBridge.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace shinkou::ui {
namespace {

PointerButton pointer_button(std::string_view control) noexcept {
    const auto separator = control.find(':');
    const auto name = separator == std::string_view::npos ? control : control.substr(separator + 1);
    if (name == "left" || name == "1") return PointerButton::Left;
    if (name == "middle" || name == "2") return PointerButton::Middle;
    if (name == "right" || name == "3") return PointerButton::Right;
    if (name == "x1" || name == "4") return PointerButton::X1;
    if (name == "x2" || name == "5") return PointerButton::X2;
    return PointerButton::None;
}

char32_t first_code_point(std::string_view text) noexcept {
    if (text.empty()) return U'\0';
    const auto byte = [](unsigned char value) { return static_cast<char32_t>(value); };
    const auto first = static_cast<unsigned char>(text[0]);
    if (first < 0x80u) return byte(first);
    if ((first & 0xe0u) == 0xc0u && text.size() >= 2) {
        return ((byte(first) & 0x1fu) << 6u) | (byte(static_cast<unsigned char>(text[1])) & 0x3fu);
    }
    if ((first & 0xf0u) == 0xe0u && text.size() >= 3) {
        return ((byte(first) & 0x0fu) << 12u) |
            ((byte(static_cast<unsigned char>(text[1])) & 0x3fu) << 6u) |
            (byte(static_cast<unsigned char>(text[2])) & 0x3fu);
    }
    if ((first & 0xf8u) == 0xf0u && text.size() >= 4) {
        return ((byte(first) & 0x07u) << 18u) |
            ((byte(static_cast<unsigned char>(text[1])) & 0x3fu) << 12u) |
            ((byte(static_cast<unsigned char>(text[2])) & 0x3fu) << 6u) |
            (byte(static_cast<unsigned char>(text[3])) & 0x3fu);
    }
    return U'\ufffd';
}

bool translate(const input::InputEvent& source, const input::MouseState& mouse, UiEvent& target) {
    target.control = source.control;
    target.text = source.text;
    target.repeat = source.repeat;
    target.key = 0;
    target.modifiers = 0;
    target.position = {source.position.x, source.position.y};
    target.delta = {source.delta.x, source.delta.y};
    switch (source.type) {
    case input::InputEventType::KeyDown: target.type = UiEventType::KeyDown; break;
    case input::InputEventType::KeyUp: target.type = UiEventType::KeyUp; break;
    case input::InputEventType::TextInput:
        target.type = UiEventType::TextInput;
        target.character = first_code_point(source.text);
        return true;
    case input::InputEventType::MouseMove:
        target.type = UiEventType::PointerMove;
        if (target.position.x == 0.0f && target.position.y == 0.0f) target.position = {mouse.position.x, mouse.position.y};
        return true;
    case input::InputEventType::MouseButtonDown:
        target.type = UiEventType::PointerDown;
        target.button = pointer_button(source.control);
        if (source.position.x != 0.0f || source.position.y != 0.0f || (mouse.position.x == 0.0f && mouse.position.y == 0.0f))
            target.position = {source.position.x, source.position.y};
        else target.position = {mouse.position.x, mouse.position.y};
        return target.button != PointerButton::None;
    case input::InputEventType::MouseButtonUp:
        target.type = UiEventType::PointerUp;
        target.button = pointer_button(source.control);
        if (source.position.x != 0.0f || source.position.y != 0.0f || (mouse.position.x == 0.0f && mouse.position.y == 0.0f))
            target.position = {source.position.x, source.position.y};
        else target.position = {mouse.position.x, mouse.position.y};
        return target.button != PointerButton::None;
    case input::InputEventType::MouseWheel:
        target.type = UiEventType::Scroll;
        target.position = {mouse.position.x, mouse.position.y};
        return true;
    default:
        return false;
    }
    return true;
}

} // namespace

InputBridgeStats InputBridge::dispatch(const input::InputSystem& input, UiRuntime& runtime) {
    InputBridgeStats stats;
    runtime.begin_frame();
    for (const auto& source : input.events()) {
        UiEvent event;
        if (!translate(source, input.mouse(), event)) continue;
        ++stats.submitted;
        if (runtime.dispatch(event)) ++stats.handled;
    }
    return stats;
}

} // namespace shinkou::ui

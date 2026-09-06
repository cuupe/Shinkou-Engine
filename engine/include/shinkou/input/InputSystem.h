#pragma once

#include "shinkou/Math.h"
#include "shinkou/Types.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shinkou::input {

enum class ActionState { Up, Pressed, Held, Released };

enum class InputEventType {
    Quit,
    KeyDown,
    KeyUp,
    TextInput,
    MouseMove,
    MouseButtonDown,
    MouseButtonUp,
    MouseWheel,
    GamepadAdded,
    GamepadRemoved,
    GamepadButtonDown,
    GamepadButtonUp,
    GamepadAxisMotion,
    FocusLost
};

struct InputEvent {
    InputEventType type{InputEventType::Quit};
    std::string control{};
    std::string text{};
    std::uint32_t device{0};
    float value{0.0f};
    math::Vec2 position{};
    math::Vec2 delta{};
    bool repeat{false};
};

struct MouseState {
    math::Vec2 position{};
    math::Vec2 delta{};
    math::Vec2 wheel{};
    std::array<bool, 8> buttons{};
};

struct GamepadState {
    std::uint32_t playerIndex{0};
    std::int64_t instanceId{0};
    std::string name{};
    bool connected{false};
    std::array<bool, 32> buttons{};
    std::array<float, 6> axes{};
};

struct InputBinding {
    // Examples: "key:W", "mouse:left", "gamepad:0:south", "gamepad:0:leftx".
    std::string control;
    float scale{1.0f};
    float deadZone{0.0f};
    bool inverted{false};
};

class IInputBackend {
public:
    virtual ~IInputBackend() = default;
    virtual bool initialize() = 0;
    virtual void shutdown() {}
    virtual void attach_window(void* nativeWindow) { (void)nativeWindow; }
    virtual void poll() = 0;
    virtual float control_value(std::string_view control) const = 0;
    virtual const MouseState& mouse_state() const = 0;
    virtual const std::vector<GamepadState>& gamepads() const = 0;
    virtual const std::vector<InputEvent>& events() const = 0;
};

class InputSystem final {
    std::unique_ptr<IInputBackend> backend_;
    bool initialized_{false};
    std::unordered_map<std::string, std::vector<InputBinding>> bindings_;
    std::unordered_map<std::string, ActionState> actionStates_;
    std::unordered_map<std::string, float> actionValues_;

    float evaluate_action(const std::vector<InputBinding>& bindings) const;
public:
    explicit InputSystem(std::unique_ptr<IInputBackend> backend);
    ~InputSystem();

    bool initialize();
    void shutdown();
    void attach_window(void* nativeWindow);
    void poll();

    void bind_action(std::string action, std::vector<InputBinding> bindings);
    void add_binding(std::string_view action, InputBinding binding);
    void clear_action(std::string_view action);
    bool has_action(std::string_view action) const;
    ActionState action(std::string_view name) const;
    float action_value(std::string_view name) const;
    bool is_down(std::string_view name) const;
    bool is_pressed(std::string_view name) const;
    bool is_released(std::string_view name) const;

    const MouseState& mouse() const;
    const std::vector<GamepadState>& gamepads() const;
    const std::vector<InputEvent>& events() const;
};

std::unique_ptr<IInputBackend> create_sdl3_input_backend();
}

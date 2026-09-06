#include "shinkou/input/InputSystem.h"

#include <algorithm>
#include <cmath>
#include <string>

#if defined(SHINKOU_WITH_INPUT)
#include "shinkou_input/ShinkouInputApi.h"
#endif

namespace shinkou::input {
namespace {
constexpr float kButtonThreshold = 0.5f;

float apply_binding(float value, const InputBinding& binding) {
    if (std::abs(value) <= std::max(0.0f, binding.deadZone)) return 0.0f;
    if (binding.inverted) value = -value;
    return value * binding.scale;
}

float clamp_action_value(float value) { return std::clamp(value, -1.0f, 1.0f); }

class NullInputBackend final : public IInputBackend {
    MouseState mouse_{};
    std::vector<GamepadState> gamepads_{};
    std::vector<InputEvent> events_{};
public:
    bool initialize() override { return true; }
    void poll() override { mouse_.delta = {}; mouse_.wheel = {}; events_.clear(); }
    float control_value(std::string_view) const override { return 0.0f; }
    const MouseState& mouse_state() const override { return mouse_; }
    const std::vector<GamepadState>& gamepads() const override { return gamepads_; }
    const std::vector<InputEvent>& events() const override { return events_; }
};

#if defined(SHINKOU_WITH_INPUT)
class ShinkouInputBackend final : public IInputBackend {
    ShinkouInputHandle* handle_{nullptr};
    MouseState mouse_{};
    std::vector<GamepadState> gamepads_{};
    std::vector<InputEvent> events_{};

    static InputEventType event_type(std::uint32_t type) {
        switch (type) {
        case SHINKOU_INPUT_EVENT_KEY_DOWN: return InputEventType::KeyDown;
        case SHINKOU_INPUT_EVENT_KEY_UP: return InputEventType::KeyUp;
        case SHINKOU_INPUT_EVENT_TEXT_INPUT: return InputEventType::TextInput;
        case SHINKOU_INPUT_EVENT_MOUSE_MOVE: return InputEventType::MouseMove;
        case SHINKOU_INPUT_EVENT_MOUSE_BUTTON_DOWN: return InputEventType::MouseButtonDown;
        case SHINKOU_INPUT_EVENT_MOUSE_BUTTON_UP: return InputEventType::MouseButtonUp;
        case SHINKOU_INPUT_EVENT_MOUSE_WHEEL: return InputEventType::MouseWheel;
        case SHINKOU_INPUT_EVENT_GAMEPAD_ADDED: return InputEventType::GamepadAdded;
        case SHINKOU_INPUT_EVENT_GAMEPAD_REMOVED: return InputEventType::GamepadRemoved;
        case SHINKOU_INPUT_EVENT_GAMEPAD_BUTTON_DOWN: return InputEventType::GamepadButtonDown;
        case SHINKOU_INPUT_EVENT_GAMEPAD_BUTTON_UP: return InputEventType::GamepadButtonUp;
        case SHINKOU_INPUT_EVENT_GAMEPAD_AXIS_MOTION: return InputEventType::GamepadAxisMotion;
        case SHINKOU_INPUT_EVENT_FOCUS_LOST: return InputEventType::FocusLost;
        default: return InputEventType::Quit;
        }
    }

    void sync_snapshots() {
        ShinkouInputMouseState mouse{};
        if (shinkou_input_get_mouse(handle_, &mouse)) {
            mouse_.position = {mouse.x, mouse.y};
            mouse_.delta = {mouse.delta_x, mouse.delta_y};
            mouse_.wheel = {mouse.wheel_x, mouse.wheel_y};
            for (std::size_t index = 0; index < mouse_.buttons.size(); ++index) mouse_.buttons[index] = mouse.buttons[index] != 0;
        }

        gamepads_.clear();
        const std::uint32_t gamepadCount = shinkou_input_gamepad_count(handle_);
        gamepads_.reserve(gamepadCount);
        for (std::uint32_t index = 0; index < gamepadCount; ++index) {
            ShinkouInputGamepadState source{};
            if (!shinkou_input_get_gamepad(handle_, index, &source)) continue;
            GamepadState state{};
            state.playerIndex = source.player_index;
            state.instanceId = source.instance_id;
            state.connected = source.connected != 0;
            state.name = source.name;
            for (std::size_t button = 0; button < state.buttons.size(); ++button) state.buttons[button] = source.buttons[button] != 0;
            for (std::size_t axis = 0; axis < state.axes.size(); ++axis) state.axes[axis] = source.axes[axis];
            gamepads_.push_back(std::move(state));
        }

        events_.clear();
        const std::uint32_t eventCount = shinkou_input_event_count(handle_);
        events_.reserve(eventCount);
        for (std::uint32_t index = 0; index < eventCount; ++index) {
            ShinkouInputEvent source{};
            if (!shinkou_input_get_event(handle_, index, &source)) continue;
            InputEvent event{};
            event.type = event_type(source.type);
            event.control = source.control;
            event.text = source.text;
            event.device = source.device;
            event.value = source.value;
            event.position = {source.x, source.y};
            event.delta = {source.delta_x, source.delta_y};
            event.repeat = source.repeat != 0;
            events_.push_back(std::move(event));
        }
    }

public:
    explicit ShinkouInputBackend(void* nativeWindow) : handle_(shinkou_input_create(nativeWindow)) {}
    ~ShinkouInputBackend() override {
        if (handle_ != nullptr) shinkou_input_destroy(handle_);
    }
    bool initialize() override { return handle_ != nullptr && shinkou_input_initialize(handle_) != 0; }
    void shutdown() override { if (handle_ != nullptr) shinkou_input_shutdown(handle_); }
    void attach_window(void* nativeWindow) override { if (handle_ != nullptr) shinkou_input_attach_window(handle_, nativeWindow); }
    void poll() override {
        if (handle_ == nullptr) return;
        shinkou_input_poll(handle_);
        sync_snapshots();
    }
    float control_value(std::string_view control) const override {
        if (handle_ == nullptr) return 0.0f;
        const std::string value(control);
        return shinkou_input_control_value(handle_, value.c_str());
    }
    const MouseState& mouse_state() const override { return mouse_; }
    const std::vector<GamepadState>& gamepads() const override { return gamepads_; }
    const std::vector<InputEvent>& events() const override { return events_; }
};
#endif
}

InputSystem::InputSystem(std::unique_ptr<IInputBackend> backend) : backend_(std::move(backend)) {}
InputSystem::~InputSystem() { shutdown(); }
bool InputSystem::initialize() {
    if (initialized_) return true;
    initialized_ = backend_ && backend_->initialize();
    return initialized_;
}
void InputSystem::shutdown() {
    if (!initialized_) return;
    if (backend_) backend_->shutdown();
    initialized_ = false;
}
void InputSystem::attach_window(void* nativeWindow) { if (backend_) backend_->attach_window(nativeWindow); }

float InputSystem::evaluate_action(const std::vector<InputBinding>& bindings) const {
    float value = 0.0f;
    for (const auto& binding : bindings) if (backend_) value += apply_binding(backend_->control_value(binding.control), binding);
    return clamp_action_value(value);
}

void InputSystem::poll() {
    if (!backend_) return;
    backend_->poll();
    for (const auto& [name, bindings] : bindings_) {
        const float value = evaluate_action(bindings);
        const bool down = std::abs(value) >= kButtonThreshold;
        const bool wasDown = std::abs(actionValues_[name]) >= kButtonThreshold;
        actionValues_[name] = value;
        actionStates_[name] = down ? (wasDown ? ActionState::Held : ActionState::Pressed)
                                   : (wasDown ? ActionState::Released : ActionState::Up);
    }
}

void InputSystem::bind_action(std::string actionName, std::vector<InputBinding> bindings) {
    actionStates_[actionName] = ActionState::Up;
    actionValues_[actionName] = 0.0f;
    bindings_[std::move(actionName)] = std::move(bindings);
}
void InputSystem::add_binding(std::string_view actionName, InputBinding binding) {
    auto& bindings = bindings_[std::string(actionName)];
    bindings.push_back(std::move(binding));
    actionStates_.try_emplace(std::string(actionName), ActionState::Up);
    actionValues_.try_emplace(std::string(actionName), 0.0f);
}
void InputSystem::clear_action(std::string_view actionName) {
    bindings_.erase(std::string(actionName));
    actionStates_.erase(std::string(actionName));
    actionValues_.erase(std::string(actionName));
}
bool InputSystem::has_action(std::string_view name) const { return bindings_.find(std::string(name)) != bindings_.end(); }
ActionState InputSystem::action(std::string_view name) const {
    const auto it = actionStates_.find(std::string(name));
    return it == actionStates_.end() ? ActionState::Up : it->second;
}
float InputSystem::action_value(std::string_view name) const {
    const auto it = actionValues_.find(std::string(name));
    return it == actionValues_.end() ? 0.0f : it->second;
}
bool InputSystem::is_down(std::string_view name) const { const auto state = action(name); return state == ActionState::Pressed || state == ActionState::Held; }
bool InputSystem::is_pressed(std::string_view name) const { return action(name) == ActionState::Pressed; }
bool InputSystem::is_released(std::string_view name) const { return action(name) == ActionState::Released; }
const MouseState& InputSystem::mouse() const { static const MouseState empty{}; return backend_ ? backend_->mouse_state() : empty; }
const std::vector<GamepadState>& InputSystem::gamepads() const { static const std::vector<GamepadState> empty{}; return backend_ ? backend_->gamepads() : empty; }
const std::vector<InputEvent>& InputSystem::events() const { static const std::vector<InputEvent> empty{}; return backend_ ? backend_->events() : empty; }

std::unique_ptr<IInputBackend> create_sdl3_input_backend() {
#if defined(SHINKOU_WITH_INPUT)
    return std::make_unique<ShinkouInputBackend>(nullptr);
#else
    return std::make_unique<NullInputBackend>();
#endif
}
}

#include "shinkou/input/InputSystem.h"

#include <cassert>
#include <unordered_map>

namespace {
class TestBackend final : public shinkou::input::IInputBackend {
public:
    std::unordered_map<std::string, float> values;
    shinkou::input::MouseState mouseState{};
    std::vector<shinkou::input::GamepadState> gamepadStates;
    std::vector<shinkou::input::InputEvent> inputEvents;

    bool initialize() override { return true; }
    void poll() override {}
    float control_value(std::string_view control) const override {
        const auto it = values.find(std::string(control));
        return it == values.end() ? 0.0f : it->second;
    }
    const shinkou::input::MouseState& mouse_state() const override { return mouseState; }
    const std::vector<shinkou::input::GamepadState>& gamepads() const override { return gamepadStates; }
    const std::vector<shinkou::input::InputEvent>& events() const override { return inputEvents; }
};
}

int main() {
    auto backend = std::make_unique<TestBackend>();
    auto* backendPtr = backend.get();
    shinkou::input::InputSystem input(std::move(backend));

    input.bind_action("jump", {{"key:space"}});
    input.bind_action("move", {{"gamepad:0:leftx", 1.0f, 0.2f}});
    assert(input.initialize());

    backendPtr->values["key:space"] = 1.0f;
    input.poll();
    assert(input.is_pressed("jump"));
    assert(input.is_down("jump"));

    input.poll();
    assert(input.action("jump") == shinkou::input::ActionState::Held);

    backendPtr->values["key:space"] = 0.0f;
    input.poll();
    assert(input.is_released("jump"));
    input.poll();
    assert(input.action("jump") == shinkou::input::ActionState::Up);

    backendPtr->values["gamepad:0:leftx"] = 0.1f;
    input.poll();
    assert(input.action_value("move") == 0.0f);
    backendPtr->values["gamepad:0:leftx"] = 0.75f;
    input.poll();
    assert(input.action_value("move") == 0.75f);

    input.clear_action("jump");
    assert(!input.has_action("jump"));
    input.shutdown();
    return 0;
}

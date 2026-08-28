#include "shinkou/input/InputSystem.h"
#include "shinkou/ui/InputBridge.h"

#include <cassert>
#include <iostream>
#include <memory>

namespace {

class TestBackend final : public shinkou::input::IInputBackend {
public:
    shinkou::input::MouseState mouseState{};
    std::vector<shinkou::input::InputEvent> pending;

    bool initialize() override { return true; }
    void poll() override {}
    float control_value(std::string_view) const override { return 0.0f; }
    const shinkou::input::MouseState& mouse_state() const override { return mouseState; }
    const std::vector<shinkou::input::GamepadState>& gamepads() const override {
        static const std::vector<shinkou::input::GamepadState> empty;
        return empty;
    }
    const std::vector<shinkou::input::InputEvent>& events() const override { return pending; }
};

} // namespace

int main() {
    using namespace shinkou;
    auto backend = std::make_unique<TestBackend>();
    auto* source = backend.get();
    input::InputSystem input(std::move(backend));
    assert(input.initialize());

    ui::UiRuntime runtime;
    ui::LayoutStyle panelStyle;
    panelStyle.layout = ui::Layout::Overlay;
    const auto panel = runtime.create_widget(runtime.root(), panelStyle);
    ui::LayoutStyle buttonStyle;
    buttonStyle.size = {100.0f, 40.0f};
    buttonStyle.focusable = true;
    const auto button = runtime.create_widget(panel, buttonStyle);
    int downs = 0;
    int ups = 0;
    runtime.set_event_handler(button, [&](ui::WidgetId, ui::UiEvent& event) {
        if (event.type == ui::UiEventType::PointerDown) {
            ++downs;
            runtime.capture_pointer(button);
        } else if (event.type == ui::UiEventType::PointerUp) {
            ++ups;
        }
        return ui::EventResult::Handled;
    });
    runtime.layout({200.0f, 100.0f});

    source->mouseState.position = {10.0f, 10.0f};
    source->pending.push_back({input::InputEventType::MouseButtonDown, "mouse:left", {}, 0, 1.0f, source->mouseState.position, {}, false});
    source->pending.push_back({input::InputEventType::MouseButtonUp, "mouse:left", {}, 0, 0.0f, source->mouseState.position, {}, false});
    source->pending.push_back({input::InputEventType::TextInput, {}, "界", 0, 0.0f, {}, {}, false});
    input.poll();
    const auto stats = ui::InputBridge::dispatch(input, runtime);
    assert(stats.submitted == 3);
    assert(stats.handled == 3);
    assert(downs == 1 && ups == 1);
    assert(runtime.focused() == button);
    assert(runtime.captured_pointer() == ui::InvalidWidgetId);

    input.shutdown();
    std::cout << "InputBridge event translation and engine UI dispatch passed\n";
    return 0;
}

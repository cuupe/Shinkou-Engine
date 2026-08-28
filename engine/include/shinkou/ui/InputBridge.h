#pragma once

#include "shinkou/input/InputSystem.h"
#include "shinkou/ui/Ui.h"

#include <cstddef>

namespace shinkou::ui {

struct InputBridgeStats {
    std::size_t submitted{0};
    std::size_t handled{0};
};

// Converts the engine input snapshot/events into backend-neutral UI events.
// The bridge does not retain input state and can be called once per engine tick.
class InputBridge final {
public:
    static InputBridgeStats dispatch(const input::InputSystem& input, UiRuntime& runtime);
};

} // namespace shinkou::ui

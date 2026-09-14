#pragma once

#include "shinkou/Math.h"

#include <optional>

namespace shinkou::editor {

// Strongly named coordinate spaces keep native client pixels out of the
// retained UI and viewport-local math until the host explicitly converts
// them. This header intentionally does not define an editor::World alias.
struct WindowClientPx {
    float x{0.0f};
    float y{0.0f};
};

struct UiLogicalPx {
    float x{0.0f};
    float y{0.0f};
};

std::optional<WindowClientPx> ui_logical_to_window_client_px(UiLogicalPx point, float dpiScale) noexcept;
std::optional<UiLogicalPx> window_client_to_ui_logical_px(WindowClientPx point, float dpiScale) noexcept;

} // namespace shinkou::editor

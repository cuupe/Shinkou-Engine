#include "EditorImGuiInput.h"
#include "EditorImGuiView.h"
#include "shinkou/editor/EditorLayer.h"
#if defined(SHINKOU_WITH_IMGUI)
#include <imgui.h>

// ImGuizmo requires ImGui types to be declared before its header.
#include <ImGuizmo.h>

namespace shinkou::editor {
int editor_imgui_key(EditorKey key) noexcept {
    if (key >= EditorKey::A && key <= EditorKey::Z)
        return ImGuiKey_A + static_cast<int>(key) - static_cast<int>(EditorKey::A);
    if (key >= EditorKey::Digit0 && key <= EditorKey::Digit9)
        return ImGuiKey_0 + static_cast<int>(key) - static_cast<int>(EditorKey::Digit0);
    if (key >= EditorKey::F1 && key <= EditorKey::F12)
        return ImGuiKey_F1 + static_cast<int>(key) - static_cast<int>(EditorKey::F1);
    switch (key) {
    case EditorKey::Escape:
        return ImGuiKey_Escape;
    case EditorKey::Enter:
        return ImGuiKey_Enter;
    case EditorKey::Tab:
        return ImGuiKey_Tab;
    case EditorKey::Space:
        return ImGuiKey_Space;
    case EditorKey::Delete:
        return ImGuiKey_Delete;
    case EditorKey::Backspace:
        return ImGuiKey_Backspace;
    case EditorKey::Comma:
        return ImGuiKey_Comma;
    case EditorKey::Period:
        return ImGuiKey_Period;
    case EditorKey::Home:
        return ImGuiKey_Home;
    case EditorKey::End:
        return ImGuiKey_End;
    case EditorKey::Left:
        return ImGuiKey_LeftArrow;
    case EditorKey::Right:
        return ImGuiKey_RightArrow;
    case EditorKey::Up:
        return ImGuiKey_UpArrow;
    case EditorKey::Down:
        return ImGuiKey_DownArrow;
    case EditorKey::Shift:
        return ImGuiKey_LeftShift;
    case EditorKey::Control:
        return ImGuiKey_LeftCtrl;
    case EditorKey::Alt:
        return ImGuiKey_LeftAlt;
    default:
        return ImGuiKey_None;
    }
}
EditorInputFrame collect_editor_input() {
    const auto &io = ImGui::GetIO();
    EditorInputFrame frame;
    frame.modifiers = (io.KeyCtrl ? ControlModifier : 0) | (io.KeyShift ? ShiftModifier : 0) |
                      (io.KeyAlt ? AltModifier : 0);
    frame.textInput = io.WantTextInput;
    frame.itemActive = ImGui::IsAnyItemActive();
    frame.focused = !io.AppFocusLost;
    frame.popupOpen =
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    for (std::size_t i = 1; i < EditorInputFrame::keyCount; ++i) {
        const auto key = static_cast<ImGuiKey>(editor_imgui_key(static_cast<EditorKey>(i)));
        frame.down[i] = ImGui::IsKeyDown(key);
        frame.pressed[i] = ImGui::IsKeyPressed(key, false);
    }
    const auto modifier = [&](EditorKey key, ImGuiKey left, ImGuiKey right) {
        const auto i = static_cast<std::size_t>(key);
        frame.down[i] = ImGui::IsKeyDown(left) || ImGui::IsKeyDown(right);
        frame.pressed[i] = ImGui::IsKeyPressed(left, false) || ImGui::IsKeyPressed(right, false);
    };
    modifier(EditorKey::Shift, ImGuiKey_LeftShift, ImGuiKey_RightShift);
    modifier(EditorKey::Control, ImGuiKey_LeftCtrl, ImGuiKey_RightCtrl);
    modifier(EditorKey::Alt, ImGuiKey_LeftAlt, ImGuiKey_RightAlt);
    return frame;
}
void EditorImGuiView::route_actions(EditorLayer &e, World &world, EditorInputContext context) {
    auto frame = inputFrame_;
    frame.textInput = frame.textInput || ImGui::GetIO().WantTextInput;
    frame.itemActive = frame.itemActive || ImGui::IsAnyItemActive();
    frame.popupOpen =
        frame.popupOpen ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const auto actions =
        e.inputRouter_.route(e.actionRegistry_, e.preferences_.bindings, frame, context);
    for (std::size_t i = 0; i < actions.count; ++i)
        e.execute_action(actions.actions[i]->id, world);
}
void EditorImGuiView::shortcuts(EditorLayer &e, World &world) {
    inputFrame_ = collect_editor_input();
    if (cameraMouseButton_ < 0 && !ImGuizmo::IsUsing() && bindingCaptureAction_.empty())
        route_actions(e, world, EditorInputContext::Global);
}
} // namespace shinkou::editor
#endif

#include "EditorImGuiView.h"
#include "shinkou/World.h"
#include "shinkou/editor/EditorLayer.h"
#include "shinkou/editor/EditorLocalization.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace shinkou::editor {
void EditorLayer::ensure_viewport_camera(const World &world) {
    if (viewportController_.initialized())
        return;
    if (layout_.cameraStored &&
        viewportController_.restore(layout_.editorCamera, layout_.cameraFocusDistance,
                                    layout_.cameraOrthographicSize,
                                    layout_.cameraViewMode != "Perspective")) {
        renderViewMode_ = layout_.cameraViewMode;
        viewportController_.set_tool(
            static_cast<EditorTransformTool>(std::clamp(layout_.transformTool, -1, 2)));
    } else
        viewportController_.initialize_from_world(world);
    viewportController_.apply_settings(preferences_.camera);
}
bool EditorLayer::set_preferences(EditorPreferences preferences) {
    std::string error;
    if (!preferences.valid(actionRegistry_, error)) {
        settingsStatus_ = error;
        return false;
    }
    preferences_ = std::move(preferences);
    viewportController_.apply_settings(preferences_.camera);
    ++preferencesRevision_;
    settingsDirty_ = true;
    settingsStatus_ = "Preferences applied (not saved)";
    return true;
}
bool EditorLayer::apply_editor_style(ui::UiStyleConfig style, std::string language) {
    std::string error;
    if (!style.valid(&error) || style.fontSize < 12 || style.fontSize > 32 ||
        !EditorLocalization::supported(language) || !themeRegistry_.find(style.activeTheme)) {
        settingsStatus_ = error.empty() ? "Invalid appearance settings" : error;
        return false;
    }
    if (!style.fontPath.empty()) {
        std::error_code fsError;
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(style.fontPath), fsError)) {
            settingsStatus_ = "Font file is unavailable";
            return false;
        }
        std::ifstream font(std::filesystem::u8path(style.fontPath), std::ios::binary);
        unsigned char signature[4]{};
        font.read(reinterpret_cast<char *>(signature), 4);
        const bool supported =
            (signature[0] == 0 && signature[1] == 1 && signature[2] == 0 && signature[3] == 0) ||
            std::string_view(reinterpret_cast<const char *>(signature), 4) == "OTTO" ||
            std::string_view(reinterpret_cast<const char *>(signature), 4) == "ttcf";
        if (!font || !supported) {
            settingsStatus_ = "Select a TrueType/OpenType font file";
            return false;
        }
    }
    styleConfig_ = std::move(style);
    sync_style_to_layout();
    themeRegistry_.switch_theme(layout_.theme);
    set_language(std::move(language));
    ++preferencesRevision_;
    settingsDirty_ = true;
    settingsStatus_ = "Preferences applied (not saved)";
    return true;
}
bool EditorLayer::save_preferences() {
    std::string error;
    if (!save_editor_preferences(fileSystem_, preferences_, error)) {
        settingsStatus_ = "Preferences save failed: " + error;
        return false;
    }
    settingsDirty_ = false;
    settingsStatus_ = "Preferences saved";
    return true;
}
bool EditorLayer::reload_preferences() {
    EditorPreferences loaded;
    std::string error;
    if (!load_editor_preferences(fileSystem_, loaded, error)) {
        settingsStatus_ = "Preferences load failed: " + error;
        return false;
    }
    preferences_ = std::move(loaded);
    viewportController_.apply_settings(preferences_.camera);
    ++preferencesRevision_;
    settingsDirty_ = false;
    settingsStatus_ = "Preferences reloaded";
    return true;
}
std::string EditorLayer::binding_label(std::string_view action) const {
    for (const auto &binding : preferences_.bindings)
        if (binding.action == action)
            return binding.key ? editor_binding_name(binding) : std::string{};
    return {};
}
bool EditorLayer::can_execute_action(std::string_view id, const World &world) const {
    const auto *action = actionRegistry_.find(id);
    if (!action || action->held)
        return false;
    if (action->command == EditorCommand::Undo)
        return !undoHistory_.empty() && !inspectorEdit_;
    if (action->command == EditorCommand::Redo)
        return !redoHistory_.empty() && !inspectorEdit_;
    if (action->command == EditorCommand::PasteObject)
        return objectClipboard_.has_value() && !inspectorEdit_;
    switch (action->command) {
    case EditorCommand::DuplicateObject:
    case EditorCommand::CopyObject:
    case EditorCommand::DeleteObject:
    case EditorCommand::FrameSelection:
        return world.find_object(layout_.selectedObject) != nullptr && !inspectorEdit_;
    default:
        return true;
    }
}
bool EditorLayer::execute_action(std::string_view id, World &world) {
    activeWorld_ = &world;
    const auto *action = actionRegistry_.find(id);
    if (!action || !can_execute_action(id, world))
        return false;
    if (id == "editor.commands") {
        commandSearchRequested_ = true;
        return true;
    }
    if (id == "tool.select")
        viewportController_.set_tool(EditorTransformTool::Select);
    else if (id == "tool.move")
        viewportController_.set_tool(EditorTransformTool::Translate);
    else if (id == "tool.rotate")
        viewportController_.set_tool(EditorTransformTool::Rotate);
    else if (id == "tool.scale")
        viewportController_.set_tool(EditorTransformTool::Scale);
    else if (id == "tool.space") {
        auto next = preferences_;
        next.gizmo.localSpace = !next.gizmo.localSpace;
        return set_preferences(std::move(next));
    } else if (id == "tool.snap") {
        auto next = preferences_;
        next.gizmo.snapping = !next.gizmo.snapping;
        return set_preferences(std::move(next));
    } else if (id == "view.grid") {
        auto next = preferences_;
        next.viewport.gridVisible = !next.viewport.gridVisible;
        return set_preferences(std::move(next));
    } else if (action->command != EditorCommand::None) {
        dispatch_command(action->command, action->target, world);
        if (id == "editor.settings" && imguiView_)
            imguiView_->focus_panel("settings");
    } else
        return false;
    return true;
}
} // namespace shinkou::editor

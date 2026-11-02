#pragma once
#include "shinkou/Math.h"
#include "shinkou/editor/EditorActions.h"
#include <string>

namespace shinkou::editor {
class FileSystemService;
struct EditorCameraSettings {
    float lookSensitivity{1}, panSensitivity{1}, zoomSensitivity{1};
    float flySpeed{5}, fastMultiplier{4}, preciseMultiplier{.2f};
    float fieldOfView{60}, nearPlane{.05f}, farPlane{1000};
    bool invertY{false}, planarFlight{false}, captureMouse{true};
    std::uint32_t lookButton{1}; // 1: right, 2: middle; the other button pans.
};
struct EditorGizmoSettings {
    bool localSpace{true}, snapping{false};
    math::Vec3 translationStep{1, 1, 1};
    float rotationStep{15}, scaleStep{.1f};
};
struct EditorViewportSettings {
    float renderScale{1};
    bool gridVisible{true}, adaptiveGrid{true};
    float gridStep{1}, gridFadeDistance{0}; // Zero fade distance selects adaptive extent.
};
// Behavioral preferences are independent from workspace geometry and the
// existing UiStyleConfig appearance profile. No ImGui/platform types live here.
struct EditorPreferences {
    std::uint32_t version{1};
    EditorCameraSettings camera;
    EditorGizmoSettings gizmo;
    EditorViewportSettings viewport;
    bool detachedWindows{true};
    std::string startupBackend{"dx11"};
    std::vector<EditorKeyBinding> bindings{EditorActionRegistry{}.default_bindings()};
    bool valid(const EditorActionRegistry &, std::string &error) const;
    bool to_json(std::string &json, std::string &error) const;
    static bool from_json(std::string_view json, EditorPreferences &, std::string &error);
};
inline constexpr const char *editor_preferences_path = "Saved/Editor/Preferences.json";
bool load_editor_preferences(const FileSystemService &, EditorPreferences &, std::string &error);
bool save_editor_preferences(const FileSystemService &, const EditorPreferences &,
                             std::string &error);
} // namespace shinkou::editor

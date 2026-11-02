#include "shinkou/editor/EditorPreferences.h"
#include "shinkou/editor/FileSystem.h"
#include "shinkou/reflection/Serialization.h"
#include <cmath>
#include <unordered_set>

namespace shinkou::reflection {
SHINKOU_REFLECT_TYPE(editor::EditorCameraSettings, "editor.CameraSettings");
SHINKOU_REFLECT_TYPE(editor::EditorGizmoSettings, "editor.GizmoSettings");
SHINKOU_REFLECT_TYPE(editor::EditorViewportSettings, "editor.ViewportSettings");
SHINKOU_REFLECT_TYPE(editor::EditorKeyBinding, "editor.KeyBinding");
SHINKOU_REFLECT_TYPE(std::vector<editor::EditorKeyBinding>, "editor.KeyBindings");
SHINKOU_REFLECT_TYPE(editor::EditorPreferences, "editor.Preferences");
} // namespace shinkou::reflection
namespace shinkou::editor {
namespace {
void register_preferences(reflection::TypeRegistry &registry) {
    using namespace reflection;
    registry.register_type(TypeBuilder<EditorCameraSettings>("editor.CameraSettings")
                               .field("lookSensitivity", &EditorCameraSettings::lookSensitivity)
                               .field("panSensitivity", &EditorCameraSettings::panSensitivity)
                               .field("zoomSensitivity", &EditorCameraSettings::zoomSensitivity)
                               .field("flySpeed", &EditorCameraSettings::flySpeed)
                               .field("fastMultiplier", &EditorCameraSettings::fastMultiplier)
                               .field("preciseMultiplier", &EditorCameraSettings::preciseMultiplier)
                               .field("fieldOfView", &EditorCameraSettings::fieldOfView)
                               .field("nearPlane", &EditorCameraSettings::nearPlane)
                               .field("farPlane", &EditorCameraSettings::farPlane)
                               .field("invertY", &EditorCameraSettings::invertY)
                               .field("planarFlight", &EditorCameraSettings::planarFlight)
                               .field("captureMouse", &EditorCameraSettings::captureMouse)
                               .field("lookButton", &EditorCameraSettings::lookButton)
                               .take());
    registry.register_type(TypeBuilder<EditorGizmoSettings>("editor.GizmoSettings")
                               .field("localSpace", &EditorGizmoSettings::localSpace)
                               .field("snapping", &EditorGizmoSettings::snapping)
                               .field("translationStep", &EditorGizmoSettings::translationStep)
                               .field("rotationStep", &EditorGizmoSettings::rotationStep)
                               .field("scaleStep", &EditorGizmoSettings::scaleStep)
                               .take());
    registry.register_type(TypeBuilder<EditorViewportSettings>("editor.ViewportSettings")
                               .field("renderScale", &EditorViewportSettings::renderScale)
                               .field("gridVisible", &EditorViewportSettings::gridVisible)
                               .field("adaptiveGrid", &EditorViewportSettings::adaptiveGrid)
                               .field("gridStep", &EditorViewportSettings::gridStep)
                               .field("gridFadeDistance", &EditorViewportSettings::gridFadeDistance)
                               .take());
    registry.register_type(TypeBuilder<EditorKeyBinding>("editor.KeyBinding")
                               .field("action", &EditorKeyBinding::action)
                               .field("key", &EditorKeyBinding::key)
                               .field("modifiers", &EditorKeyBinding::modifiers)
                               .take());
    registry.register_type(make_vector_descriptor<EditorKeyBinding>("editor.KeyBindings"));
    registry.register_type(TypeBuilder<EditorPreferences>("editor.Preferences")
                               .field("version", &EditorPreferences::version)
                               .field("camera", &EditorPreferences::camera)
                               .field("gizmo", &EditorPreferences::gizmo)
                               .field("viewport", &EditorPreferences::viewport)
                               .field("detachedWindows", &EditorPreferences::detachedWindows)
                               .field("startupBackend", &EditorPreferences::startupBackend)
                               .field("bindings", &EditorPreferences::bindings)
                               .take());
}
bool range(float value, float minimum, float maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}
} // namespace
bool EditorPreferences::valid(const EditorActionRegistry &registry, std::string &error) const {
    error.clear();
    if (version != 1)
        error = "Unsupported preferences version";
    else if (!range(camera.lookSensitivity, .05f, 10) || !range(camera.panSensitivity, .05f, 10) ||
             !range(camera.zoomSensitivity, .05f, 10))
        error = "Camera sensitivity must be between 0.05 and 10";
    else if (!range(camera.flySpeed, .05f, 10000) || !range(camera.fastMultiplier, 1, 20) ||
             !range(camera.preciseMultiplier, .01f, 1))
        error = "Invalid camera movement speed";
    else if (!range(camera.fieldOfView, 5, 150) || !range(camera.nearPlane, .001f, 10) ||
             !range(camera.farPlane, 10.001f, 100000) || camera.farPlane <= camera.nearPlane)
        error = "Invalid camera field of view or clipping planes";
    else if (camera.lookButton != 1 && camera.lookButton != 2)
        error = "Look must use the right or middle mouse button";
    else if (!range(gizmo.translationStep.x, .001f, 10000) ||
             !range(gizmo.translationStep.y, .001f, 10000) ||
             !range(gizmo.translationStep.z, .001f, 10000) ||
             !range(gizmo.rotationStep, .1f, 180) || !range(gizmo.scaleStep, .001f, 10))
        error = "Invalid transform snapping step";
    else if (!range(viewport.renderScale, .5f, 2) || !range(viewport.gridStep, .01f, 10000) ||
             !range(viewport.gridFadeDistance, 0, 100000))
        error = "Invalid viewport scale or grid settings";
    else if (startupBackend != "dx11" && startupBackend != "dx12" && startupBackend != "vulkan")
        error = "Unknown startup renderer";
    if (!error.empty())
        return false;
    if (bindings.size() > 256) {
        error = "Too many shortcut bindings";
        return false;
    }
    std::unordered_set<std::string> seen;
    for (const auto &binding : bindings) {
        if (binding.action.empty() || !seen.insert(binding.action).second ||
            binding.key >= EditorInputFrame::keyCount || (binding.modifiers & ~7u)) {
            error = "Invalid or duplicate shortcut binding";
            return false;
        }
        const auto *action = registry.find(binding.action);
        if (action && action->context == EditorInputContext::FlyCamera && binding.modifiers) {
            error = "Camera movement bindings use individual keys";
            return false;
        }
        const auto other = EditorInputRouter::conflict(registry, bindings, binding);
        if (!other.empty()) {
            error = "Shortcut conflict: " + binding.action + " / " + other;
            return false;
        }
    }
    return true;
}
bool EditorPreferences::to_json(std::string &json, std::string &error) const {
    if (!valid(EditorActionRegistry{}, error))
        return false;
    reflection::TypeRegistry registry;
    register_preferences(registry);
    const auto result = reflection::serialize_json(
        registry, reflection::type_id<EditorPreferences>(), this, json, {true});
    error = result.message;
    return static_cast<bool>(result);
}
bool EditorPreferences::from_json(std::string_view json, EditorPreferences &output,
                                  std::string &error) {
    if (json.size() > 256 * 1024) {
        error = "Preferences file exceeds 256 KiB";
        return false;
    }
    reflection::TypeRegistry registry;
    register_preferences(registry);
    EditorPreferences parsed;
    const auto result = reflection::deserialize_json(
        registry, reflection::type_id<EditorPreferences>(), json, &parsed);
    if (!result) {
        error = result.message;
        return false;
    }
    const EditorActionRegistry actions;
    // New built-in actions receive defaults; unavailable extension bindings
    // remain intact and dormant rather than being silently discarded.
    for (auto binding : actions.default_bindings()) {
        bool present = false;
        for (const auto &current : parsed.bindings)
            if (current.action == binding.action) {
                present = true;
                break;
            }
        if (!present) {
            if (!EditorInputRouter::conflict(actions, parsed.bindings, binding).empty()) {
                binding.key = 0;
                binding.modifiers = 0;
            }
            parsed.bindings.push_back(std::move(binding));
        }
    }
    if (!parsed.valid(actions, error))
        return false;
    output = std::move(parsed);
    return true;
}
bool load_editor_preferences(const FileSystemService &files, EditorPreferences &output,
                             std::string &error) {
    std::string json;
    if (!files.read_text(editor_preferences_path, json, &error))
        return false;
    return EditorPreferences::from_json(json, output, error);
}
bool save_editor_preferences(const FileSystemService &files, const EditorPreferences &preferences,
                             std::string &error) {
    std::string json;
    if (!preferences.to_json(json, error))
        return false;
    return files.write_text_atomic(editor_preferences_path, json, &error);
}
} // namespace shinkou::editor

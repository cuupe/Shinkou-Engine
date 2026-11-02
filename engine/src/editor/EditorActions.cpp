#include "shinkou/editor/EditorActions.h"
#include <algorithm>

namespace shinkou::editor {
EditorActionRegistry::EditorActionRegistry() {
    const auto add = [this](const char *id, const char *label, const char *category,
                            EditorCommand command, EditorKey key, std::uint8_t mods = 0,
                            EditorInputContext context = EditorInputContext::Global,
                            bool held = false, const char *target = "") {
        register_action({id, label, category, command, context, held, key, mods, target});
    };
    add("scene.new", "New Scene", "Scene", EditorCommand::NewScene, EditorKey::N, ControlModifier);
    add("scene.open", "Open Scene", "Scene", EditorCommand::OpenScene, EditorKey::O,
        ControlModifier);
    add("scene.save", "Save Scene", "Scene", EditorCommand::SaveScene, EditorKey::S,
        ControlModifier);
    add("scene.save-as", "Save Scene As", "Scene", EditorCommand::SaveSceneAs, EditorKey::S,
        ControlModifier | ShiftModifier);
    add("edit.undo", "Undo", "Edit", EditorCommand::Undo, EditorKey::Z, ControlModifier);
    add("edit.redo", "Redo", "Edit", EditorCommand::Redo, EditorKey::Y, ControlModifier);
    add("object.empty", "Create Empty", "Objects", EditorCommand::CreateEmpty, EditorKey::N,
        ControlModifier | ShiftModifier);
    add("object.cube", "Cube", "Objects", EditorCommand::Create3DObject, EditorKey::None, 0,
        EditorInputContext::Global, false, "Cube");
    add("object.sphere", "Sphere", "Objects", EditorCommand::Create3DObject, EditorKey::None, 0,
        EditorInputContext::Global, false, "Sphere");
    add("object.plane", "Plane", "Objects", EditorCommand::Create3DObject, EditorKey::None, 0,
        EditorInputContext::Global, false, "Plane");
    add("object.camera", "Camera", "Objects", EditorCommand::CreateCamera, EditorKey::None);
    add("object.duplicate", "Duplicate", "Objects", EditorCommand::DuplicateObject, EditorKey::D,
        ControlModifier, EditorInputContext::Hierarchy);
    add("scene.duplicate", "Duplicate", "Objects", EditorCommand::DuplicateObject, EditorKey::D,
        ControlModifier, EditorInputContext::Scene);
    add("object.copy", "Copy Object", "Objects", EditorCommand::CopyObject, EditorKey::C,
        ControlModifier, EditorInputContext::Hierarchy);
    add("object.paste", "Paste Object", "Objects", EditorCommand::PasteObject, EditorKey::V,
        ControlModifier, EditorInputContext::Hierarchy);
    add("scene.copy", "Copy Object", "Objects", EditorCommand::CopyObject, EditorKey::C,
        ControlModifier, EditorInputContext::Scene);
    add("scene.paste", "Paste Object", "Objects", EditorCommand::PasteObject, EditorKey::V,
        ControlModifier, EditorInputContext::Scene);
    add("object.delete", "Delete Object", "Objects", EditorCommand::DeleteObject, EditorKey::Delete,
        0, EditorInputContext::Hierarchy);
    add("scene.delete", "Delete Object", "Objects", EditorCommand::DeleteObject, EditorKey::Delete,
        0, EditorInputContext::Scene);
    add("view.frame", "Frame Selection", "Viewport", EditorCommand::FrameSelection, EditorKey::F, 0,
        EditorInputContext::Scene);
    add("hierarchy.frame", "Frame Selection", "Viewport", EditorCommand::FrameSelection,
        EditorKey::F, 0, EditorInputContext::Hierarchy);
    add("tool.select", "Select Tool", "Tools", EditorCommand::None, EditorKey::Q, 0,
        EditorInputContext::Scene);
    add("tool.move", "Move Tool", "Tools", EditorCommand::None, EditorKey::W, 0,
        EditorInputContext::Scene);
    add("tool.rotate", "Rotate Tool", "Tools", EditorCommand::None, EditorKey::E, 0,
        EditorInputContext::Scene);
    add("tool.scale", "Scale Tool", "Tools", EditorCommand::None, EditorKey::R, 0,
        EditorInputContext::Scene);
    add("tool.space", "Local / World", "Tools", EditorCommand::None, EditorKey::X, 0,
        EditorInputContext::Scene);
    add("tool.snap", "Toggle Snapping", "Tools", EditorCommand::None, EditorKey::G, 0,
        EditorInputContext::Scene);
    add("view.grid", "Toggle Grid", "Viewport", EditorCommand::None, EditorKey::None);
    add("editor.settings", "Preferences", "Editor", EditorCommand::ProjectSettings,
        EditorKey::Comma, ControlModifier);
    add("editor.commands", "Command Search", "Editor", EditorCommand::None, EditorKey::P,
        ControlModifier | ShiftModifier);
    add("simulation.play", "Play / Stop", "Simulation", EditorCommand::Play, EditorKey::F5);
    add("simulation.pause", "Pause", "Simulation", EditorCommand::Pause, EditorKey::F6);
    add("simulation.step", "Step", "Simulation", EditorCommand::Step, EditorKey::F7);
    add("assets.refresh", "Refresh Assets", "Assets", EditorCommand::RefreshAssets,
        EditorKey::None);
    add("camera.forward", "Move Forward", "Camera", EditorCommand::None, EditorKey::W, 0,
        EditorInputContext::FlyCamera, true);
    add("camera.back", "Move Backward", "Camera", EditorCommand::None, EditorKey::S, 0,
        EditorInputContext::FlyCamera, true);
    add("camera.left", "Move Left", "Camera", EditorCommand::None, EditorKey::A, 0,
        EditorInputContext::FlyCamera, true);
    add("camera.right", "Move Right", "Camera", EditorCommand::None, EditorKey::D, 0,
        EditorInputContext::FlyCamera, true);
    add("camera.down", "Move Down", "Camera", EditorCommand::None, EditorKey::Q, 0,
        EditorInputContext::FlyCamera, true);
    add("camera.up", "Move Up", "Camera", EditorCommand::None, EditorKey::E, 0,
        EditorInputContext::FlyCamera, true);
    add("camera.boost", "Fast Movement", "Camera", EditorCommand::None, EditorKey::Shift, 0,
        EditorInputContext::FlyCamera, true);
    add("camera.precise", "Precise Movement", "Camera", EditorCommand::None, EditorKey::Control, 0,
        EditorInputContext::FlyCamera, true);
}
bool EditorActionRegistry::register_action(EditorActionDefinition definition) {
    if (definition.id.empty() || definition.label.empty() || find(definition.id))
        return false;
    actions_.push_back(std::move(definition));
    return true;
}
const EditorActionDefinition *EditorActionRegistry::find(std::string_view id) const noexcept {
    for (const auto &action : actions_)
        if (action.id == id)
            return &action;
    return nullptr;
}
const EditorActionDefinition *
EditorActionRegistry::find_command(EditorCommand command, std::string_view target) const noexcept {
    if (command == EditorCommand::None)
        return nullptr;
    for (const auto &action : actions_)
        if (action.command == command && action.target == target)
            return &action;
    return nullptr;
}
std::vector<EditorKeyBinding> EditorActionRegistry::default_bindings() const {
    std::vector<EditorKeyBinding> bindings;
    bindings.reserve(actions_.size());
    for (const auto &action : actions_)
        bindings.push_back(
            {action.id, static_cast<std::uint16_t>(action.defaultKey), action.defaultModifiers});
    return bindings;
}
bool EditorActionBatch::contains(std::string_view id) const noexcept {
    for (std::size_t i = 0; i < count; ++i)
        if (actions[i]->id == id)
            return true;
    return false;
}
EditorActionBatch EditorInputRouter::route(const EditorActionRegistry &registry,
                                           const std::vector<EditorKeyBinding> &bindings,
                                           const EditorInputFrame &frame,
                                           EditorInputContext context) const noexcept {
    EditorActionBatch result;
    if (!frame.focused || frame.textInput || frame.popupOpen ||
        (frame.itemActive && context != EditorInputContext::FlyCamera))
        return result;
    for (const auto &binding : bindings) {
        if (!binding.key || binding.key >= EditorInputFrame::keyCount)
            continue;
        const auto modifiers = context == EditorInputContext::FlyCamera ? 0 : frame.modifiers;
        if (binding.modifiers != modifiers)
            continue;
        if (!frame.pressed[binding.key] && !frame.down[binding.key])
            continue;
        const auto *action = registry.find(binding.action);
        if (!action || action->context != context ||
            !(action->held ? frame.down[binding.key] : frame.pressed[binding.key]))
            continue;
        if (result.count < result.actions.size())
            result.actions[result.count++] = action;
    }
    return result;
}
bool EditorInputRouter::contexts_overlap(EditorInputContext a, EditorInputContext b) noexcept {
    if (a == EditorInputContext::FlyCamera || b == EditorInputContext::FlyCamera)
        return a == b;
    return a == b || a == EditorInputContext::Global || b == EditorInputContext::Global;
}
std::string EditorInputRouter::conflict(const EditorActionRegistry &registry,
                                        const std::vector<EditorKeyBinding> &bindings,
                                        const EditorKeyBinding &candidate) {
    const auto *action = registry.find(candidate.action);
    if (!action || !candidate.key)
        return {};
    for (const auto &binding : bindings) {
        if (binding.action == candidate.action || binding.key != candidate.key ||
            binding.modifiers != candidate.modifiers)
            continue;
        const auto *other = registry.find(binding.action);
        if (other && contexts_overlap(action->context, other->context))
            return other->label;
    }
    return {};
}
std::string editor_key_name(EditorKey key) {
    if (key >= EditorKey::A && key <= EditorKey::Z)
        return std::string(
            1, static_cast<char>('A' + static_cast<int>(key) - static_cast<int>(EditorKey::A)));
    if (key >= EditorKey::Digit0 && key <= EditorKey::Digit9)
        return std::string(1, static_cast<char>('0' + static_cast<int>(key) -
                                                static_cast<int>(EditorKey::Digit0)));
    if (key >= EditorKey::F1 && key <= EditorKey::F12)
        return "F" + std::to_string(static_cast<int>(key) - static_cast<int>(EditorKey::F1) + 1);
    switch (key) {
    case EditorKey::None:
        return "Unbound";
    case EditorKey::Escape:
        return "Escape";
    case EditorKey::Enter:
        return "Enter";
    case EditorKey::Tab:
        return "Tab";
    case EditorKey::Space:
        return "Space";
    case EditorKey::Delete:
        return "Delete";
    case EditorKey::Backspace:
        return "Backspace";
    case EditorKey::Comma:
        return ",";
    case EditorKey::Period:
        return ".";
    case EditorKey::Home:
        return "Home";
    case EditorKey::End:
        return "End";
    case EditorKey::Left:
        return "Left";
    case EditorKey::Right:
        return "Right";
    case EditorKey::Up:
        return "Up";
    case EditorKey::Down:
        return "Down";
    case EditorKey::Shift:
        return "Shift";
    case EditorKey::Control:
        return "Ctrl";
    case EditorKey::Alt:
        return "Alt";
    default:
        return "Unknown";
    }
}
std::string editor_binding_name(const EditorKeyBinding &binding) {
    if (!binding.key)
        return "Unbound";
    std::string label;
    if (binding.modifiers & ControlModifier)
        label += "Ctrl+";
    if (binding.modifiers & ShiftModifier)
        label += "Shift+";
    if (binding.modifiers & AltModifier)
        label += "Alt+";
    return label + editor_key_name(static_cast<EditorKey>(binding.key));
}
const char *editor_context_name(EditorInputContext context) noexcept {
    switch (context) {
    case EditorInputContext::Global:
        return "Global";
    case EditorInputContext::Scene:
        return "Scene";
    case EditorInputContext::Hierarchy:
        return "Hierarchy";
    case EditorInputContext::FlyCamera:
        return "Camera (look held)";
    }
    return "";
}
} // namespace shinkou::editor

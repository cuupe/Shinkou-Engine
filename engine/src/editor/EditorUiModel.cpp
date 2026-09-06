#include "shinkou/editor/EditorUiModel.h"

#include "shinkou/GameObject.h"
#include "shinkou/World.h"
#include "shinkou/editor/EditorDocument.h"
#include <algorithm>
#include <cctype>

namespace shinkou::editor {
namespace {

bool matches_filter(std::string_view name, std::string_view filter) {
    if (filter.empty()) return true;
    if (name.size() < filter.size()) return false;
    for (std::size_t offset = 0; offset + filter.size() <= name.size(); ++offset) {
        bool matches = true;
        for (std::size_t index = 0; index < filter.size(); ++index) {
            const auto lhs = static_cast<unsigned char>(name[offset + index]);
            const auto rhs = static_cast<unsigned char>(filter[index]);
            if (std::tolower(lhs) != std::tolower(rhs)) { matches = false; break; }
        }
        if (matches) return true;
    }
    return false;
}

EditorMenuItemModel item(std::string id, std::string label, EditorCommand command,
                         std::string shortcut = {}, std::string target = {}) {
    return {std::move(id), std::move(label), std::move(shortcut), std::move(target), command, false, command != EditorCommand::None};
}

EditorMenuItemModel separator() {
    EditorMenuItemModel result;
    result.separator = true;
    return result;
}

} // namespace

EditorUiModel::EditorUiModel() {
    build_default_pages();
    build_default_menus();
}

void EditorUiModel::build_default_pages() {
    pages_ = {
        {"scene", "Scene", "", true, true},
        {"game", "Game", "Ctrl+G", false, true},
        {"hierarchy", "Hierarchy", "", true, false},
        {"inspector", "Inspector", "", true, false},
        {"project", "Project", "", true, false},
        {"console", "Console", "Ctrl+Shift+C", true, false},
        {"profiler", "Profiler", "", false, false},
        {"render-graph", "Render Graph", "", false, false},
        {"settings", "Project Settings", "", false, false},
    };
}

void EditorUiModel::build_default_menus() {
    menus_ = {
        {"file", "File", {
            item("new-scene", "New Scene", EditorCommand::NewScene, "Ctrl+N"),
            item("open-scene", "Open Scene...", EditorCommand::OpenScene, "Ctrl+O"),
            item("save-scene", "Save Scene", EditorCommand::SaveScene, "Ctrl+S"),
            item("save-scene-as", "Save Scene As...", EditorCommand::SaveSceneAs, "Ctrl+Shift+S"),
            separator(),
            item("save-layout", "Save Layout", EditorCommand::SaveLayout),
            item("reload-layout", "Reload Layout", EditorCommand::ReloadLayout),
            separator(),
            item("quit", "Exit", EditorCommand::Quit),
        }},
        {"edit", "Edit", {
            item("undo", "Undo", EditorCommand::Undo, "Ctrl+Z"),
            item("redo", "Redo", EditorCommand::Redo, "Ctrl+Y"),
            item("delete-object", "Delete Object", EditorCommand::DeleteObject, "Delete"),
            separator(),
            item("frame-selection", "Frame Selected", EditorCommand::FrameSelection, "F"),
            item("project-settings", "Project Settings", EditorCommand::ProjectSettings),
        }},
        {"assets", "Assets", {
            item("create-asset", "Create Asset", EditorCommand::None),
            item("import-asset", "Import New Asset...", EditorCommand::None),
            item("refresh-assets", "Refresh", EditorCommand::RefreshAssets),
        }},
        {"game-object", "GameObject", {
            item("create-empty", "Create Empty", EditorCommand::CreateEmpty, "Ctrl+Shift+N"),
            item("create-child", "Create Child", EditorCommand::CreateChild),
            separator(),
            item("create-3d", "3D Object (factory unavailable)", EditorCommand::None),
            item("create-2d", "2D Object (factory unavailable)", EditorCommand::None),
            item("create-ui", "UI Object (factory unavailable)", EditorCommand::None),
        }},
        {"component", "Component", {
            item("add-component", "Add Component...", EditorCommand::AddComponent),
        }},
        {"window", "Window", {
            item("scene", "Scene", EditorCommand::TogglePage, {}, "scene"),
            item("game", "Game", EditorCommand::TogglePage, {}, "game"),
            item("hierarchy", "Hierarchy", EditorCommand::TogglePage, {}, "hierarchy"),
            item("inspector", "Inspector", EditorCommand::TogglePage, {}, "inspector"),
            item("project", "Project", EditorCommand::TogglePage, {}, "project"),
            item("console", "Console", EditorCommand::TogglePage, {}, "console"),
            item("profiler", "Profiler", EditorCommand::TogglePage, {}, "profiler"),
            item("render-graph", "Render Graph", EditorCommand::TogglePage, {}, "render-graph"),
        }},
        {"theme", "Theme", {
            item("dark", "Dark", EditorCommand::SetDarkTheme),
            item("light", "Light", EditorCommand::SetLightTheme),
            item("high-contrast", "High Contrast", EditorCommand::SetHighContrastTheme),
        }},
        {"help", "Help", {
            item("documentation", "Documentation", EditorCommand::None),
            item("about", "About ShinkouEngine", EditorCommand::None),
        }},
    };
}

EditorObjectTreeNode EditorUiModel::make_tree_node(const GameObject& object, std::string_view filter,
                                                   ObjectId selected) {
    EditorObjectTreeNode node{object.id(), std::string(object.name()), object.active_in_hierarchy(), object.id() == selected, {}};
    for (const auto& child : object.children()) {
        if (!child) continue;
        auto childNode = make_tree_node(*child, filter, selected);
        if (filter.empty() || childNode.name != "" || !childNode.children.empty()) node.children.push_back(std::move(childNode));
    }
    const bool selfMatches = matches_filter(node.name, filter);
    if (!filter.empty() && !selfMatches && node.children.empty()) node.name.clear();
    return node;
}

bool EditorUiModel::contains_object(const EditorObjectTreeNode& node, ObjectId id) noexcept {
    if (node.id == id) return true;
    return std::any_of(node.children.begin(), node.children.end(), [id](const auto& child) {
        return contains_object(child, id);
    });
}

void EditorUiModel::sync(World& world) {
    // Names, active flags and parentage may change outside editor commands.
    // Hash only hierarchy data; stable trees avoid allocating/rebuilding rows.
    std::uint64_t signatureTree = 1469598103934665603ull;
    const auto mix = [&](std::uint64_t value) { signatureTree ^= value; signatureTree *= 1099511628211ull; };
    const auto visit = [&](const auto& self, const GameObject& object) -> void {
        mix(object.id()); mix(object.parent() ? object.parent()->id() : 0); mix(object.active_self());
        for (const unsigned char c : object.name()) mix(c);
        mix(0xff);
        for (const auto& child : object.children()) if (child) self(self,*child);
    };
    world.each_object([&](const GameObject& object) { visit(visit,object); });
    if (signatureTree != treeSignature_) { treeSignature_ = signatureTree; treeDirty_ = true; }
    if (!treeDirty_ && (syncedWorld_ != &world || syncedObjectCount_ != world.object_count())) treeDirty_ = true;
    std::vector<EditorInspectorField> fields;
    componentTypes_ = world.component_types();
    if (auto* object = world.find_object(selectedObject_)) {
        fields.push_back({"name", "Name", std::string(object->name()), true, false});
        fields.push_back({"active", "Active", object->active_self() ? "true" : "false", true, true});
        object->each_component([&](Component& component) {
            fields.push_back({"enabled:" + std::to_string(component.id()), std::string(component.registered_type_name()) + " enabled", component.enabled() ? "true" : "false", true, true});
            for (auto& p : component.properties()) {
                if (!p.get || has_flag(p.flags, PropertyFlags::Hidden)) continue;
                fields.push_back({std::to_string(component.id()) + ":" + p.name, p.displayName, property_text(p.get()), p.editable(), p.type == PropertyType::Bool});
            }
        });
    }
    std::string signature;
    for (const auto& f : fields) signature += f.id + "\n" + f.label + "\n" + f.value + "\n";
    if (signature != inspectorSignature_) {
        inspectorSignature_ = std::move(signature);
        inspectorFields_ = std::move(fields);
        ++revision_;
    }
    if (!treeDirty_) return;
    objectRoots_.clear();
    world.each_object([this](const GameObject& object) {
        auto node = make_tree_node(object, objectFilter_, selectedObject_);
        if (objectFilter_.empty() || !node.name.empty() || !node.children.empty()) objectRoots_.push_back(std::move(node));
    });
    if (selectedObject_ != 0 && !world.find_object(selectedObject_)) selectedObject_ = 0;
    treeDirty_ = false;
    syncedWorld_ = &world;
    syncedObjectCount_ = world.object_count();
    ++revision_;
}

void EditorUiModel::set_active_page(std::string pageId) {
    const auto it = std::find_if(pages_.begin(), pages_.end(), [&pageId](const auto& page) { return page.id == pageId; });
    if (it == pages_.end()) return;
    activePage_ = std::move(pageId);
    it->visible = true;
}

void EditorUiModel::set_page_visible(std::string_view pageId, bool visible) noexcept {
    for (auto& page : pages_) if (page.id == pageId) { page.visible = visible; return; }
}

void EditorUiModel::execute(EditorCommand command) noexcept {
    lastCommand_ = command;
    if (command == EditorCommand::Play) { playing_ = !playing_; paused_ = false; }
    else if (command == EditorCommand::Pause) { if (playing_) paused_ = !paused_; }
    else if (command == EditorCommand::Step) { playing_ = true; paused_ = true; }
}

} // namespace shinkou::editor

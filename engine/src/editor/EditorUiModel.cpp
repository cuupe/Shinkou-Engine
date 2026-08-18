#include "shinkou/editor/EditorUiModel.h"

#include "shinkou/GameObject.h"
#include "shinkou/World.h"
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
    return {std::move(id), std::move(label), std::move(shortcut), std::move(target), command, false, true};
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
            item("create-3d", "3D Object", EditorCommand::Create3DObject),
            item("create-2d", "2D Object", EditorCommand::Create2DObject),
            item("create-ui", "UI Object", EditorCommand::CreateUiObject),
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
    objectRoots_.clear();
    world.each_object([this](const GameObject& object) {
        auto node = make_tree_node(object, objectFilter_, selectedObject_);
        if (objectFilter_.empty() || !node.name.empty() || !node.children.empty()) objectRoots_.push_back(std::move(node));
    });
    if (selectedObject_ != 0 && !std::any_of(objectRoots_.begin(), objectRoots_.end(), [this](const auto& root) {
            return contains_object(root, selectedObject_);
        })) selectedObject_ = 0;
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
    if (command == EditorCommand::Play) { playing_ = true; paused_ = false; }
    else if (command == EditorCommand::Pause) { paused_ = true; }
    else if (command == EditorCommand::Step) { playing_ = true; paused_ = true; }
}

} // namespace shinkou::editor

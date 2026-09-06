#pragma once

#include "shinkou/Types.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shinkou {
class GameObject;
class World;

namespace editor {

enum class EditorCommand : std::uint8_t {
    None,
    NewScene,
    OpenScene,
    SaveScene,
    SaveSceneAs,
    SaveLayout,
    ReloadLayout,
    ResetLayout,
    Undo,
    Redo,
    Play,
    Pause,
    Step,
    FrameSelection,
    CreateEmpty,
    CreateChild,
    Create3DObject,
    Create2DObject,
    CreateUiObject,
    AddComponent,
    RefreshAssets,
    TogglePage,
    SetDarkTheme,
    SetLightTheme,
    SetHighContrastTheme,
    ProjectSettings,
    Quit,
    DeleteObject,
    ClearConsole,
    SetUiScale,
    ToggleCompact,
    ToggleDocking,
};

struct EditorMenuItemModel {
    std::string id;
    std::string label;
    std::string shortcut;
    std::string target;
    EditorCommand command{EditorCommand::None};
    bool separator{false};
    bool enabled{true};
};

struct EditorMenuModel {
    std::string id;
    std::string label;
    std::vector<EditorMenuItemModel> items;
};

struct EditorPageModel {
    std::string id;
    std::string title;
    std::string shortcut;
    bool visible{true};
    bool primary{false};
};

struct EditorObjectTreeNode {
    ObjectId id{0};
    std::string name;
    bool active{true};
    bool selected{false};
    std::vector<EditorObjectTreeNode> children;
};

struct EditorInspectorField {
    std::string id;
    std::string label;
    std::string value;
    bool editable{false};
    bool boolean{false};
};

class EditorUiModel {
    std::vector<EditorMenuModel> menus_;
    std::vector<EditorPageModel> pages_;
    std::vector<EditorObjectTreeNode> objectRoots_;
    ObjectId selectedObject_{0};
    std::string objectFilter_;
    std::string activePage_{"scene"};
    EditorCommand lastCommand_{EditorCommand::None};
    bool playing_{false};
    bool paused_{false};
    bool treeDirty_{true};
    std::uint64_t revision_{0};
    const World* syncedWorld_{nullptr};
    std::size_t syncedObjectCount_{0};
    std::uint64_t treeSignature_{0};
    std::vector<EditorInspectorField> inspectorFields_;
    std::vector<std::string> componentTypes_;
    std::string inspectorSignature_;

    static EditorObjectTreeNode make_tree_node(const GameObject& object, std::string_view filter,
                                               ObjectId selected);
    static bool contains_object(const EditorObjectTreeNode& node, ObjectId id) noexcept;
    void build_default_menus();
    void build_default_pages();

public:
    EditorUiModel();

    void sync(World& world);
    void invalidate() noexcept { treeDirty_ = true; }
    void select_object(ObjectId id) noexcept {
        if (selectedObject_ == id) return;
        selectedObject_ = id;
        treeDirty_ = true;
    }
    void set_object_filter(std::string filter) {
        if (objectFilter_ == filter) return;
        objectFilter_ = std::move(filter);
        treeDirty_ = true;
    }
    void set_active_page(std::string pageId);
    void set_page_visible(std::string_view pageId, bool visible) noexcept;
    void execute(EditorCommand command) noexcept;
    const std::vector<EditorInspectorField>& inspector_fields() const noexcept { return inspectorFields_; }
    const std::vector<std::string>& component_types() const noexcept { return componentTypes_; }

    const std::vector<EditorMenuModel>& menus() const noexcept { return menus_; }
    const std::vector<EditorPageModel>& pages() const noexcept { return pages_; }
    const std::vector<EditorObjectTreeNode>& object_roots() const noexcept { return objectRoots_; }
    ObjectId selected_object() const noexcept { return selectedObject_; }
    const std::string& object_filter() const noexcept { return objectFilter_; }
    const std::string& active_page() const noexcept { return activePage_; }
    EditorCommand last_command() const noexcept { return lastCommand_; }
    bool playing() const noexcept { return playing_; }
    bool paused() const noexcept { return paused_; }
    std::uint64_t revision() const noexcept { return revision_; }
};

} // namespace editor
} // namespace shinkou

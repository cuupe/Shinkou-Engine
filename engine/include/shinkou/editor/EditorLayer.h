#pragma once

#include "shinkou/Types.h"
#include "shinkou/editor/EditorUiModel.h"
#include "shinkou/render/Renderer.h"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shinkou {
class World;
class GameObject;

namespace editor {

struct EditorLayoutState {
    std::uint32_t layoutVersion{1};
    std::string theme{"dark"};
    std::string workspace{"Default"};
    std::string projectRoot{"."};
    std::string layoutFile{"Saved/Editor/Layouts/Default.json"};
    ObjectId selectedObject{0};
    bool showHierarchy{true};
    bool showInspector{true};
    bool showViewport{true};
    bool showGame{false};
    bool showAssets{true};
    bool showConsole{true};
    bool showProfiler{false};
    bool showRenderGraph{false};
    bool showSettings{false};
};

struct EditorPanelContext {
    render::Renderer& renderer;
    World& world;
    EditorUiModel& ui;
    const EditorLayoutState& layout;
    FrameIndex frame{0};
    Seconds deltaSeconds{0};
};

struct EditorPanel {
    std::string id;
    std::string title;
    bool defaultVisible{true};
    std::function<void(EditorPanelContext&)> draw;
};

class EditorLayer {
    EditorUiModel uiModel_;
    EditorLayoutState layout_{};
    std::vector<EditorPanel> panels_;
    std::unordered_map<std::string, bool> panelVisibility_;
    std::vector<std::string> assetEntries_;
    std::vector<std::string> consoleEntries_;
    std::vector<float> frameTimes_;
    std::string lastStatus_;
    bool initialized_{false};
    bool uiContextOwned_{false};
    void* nativeWindow_{nullptr};
    void* nativeMenu_{nullptr};
    bool assetsDirty_{true};
    float displayWidth_{1280.0f};
    float displayHeight_{720.0f};

    void register_builtin_panels();
    void sync_page_visibility() noexcept;
    void refresh_asset_cache();
    void draw_toolbar(render::Renderer& renderer, World& world);
    void draw_hierarchy(World& world);
    void draw_hierarchy_object(const EditorObjectTreeNode& object);
    void draw_inspector(World& world);
    void draw_viewport(render::Renderer& renderer);
    void draw_game_view(render::Renderer& renderer);
    void draw_assets();
    void draw_console(const render::Renderer& renderer);
    void draw_profiler(const render::Renderer& renderer);
    void draw_render_graph(render::Renderer& renderer);
    void draw_settings();
    void draw_status_bar(const render::Renderer& renderer);
    void draw_registered_panels(render::Renderer& renderer, World& world, FrameIndex frame, Seconds dt);
    void dispatch_command(EditorCommand command, std::string_view target, World& world);
    void install_native_menu();
    void uninstall_native_menu();
    bool save_layout_file();
    bool load_layout_file();
    void apply_theme();

public:
    void set_native_window(void* nativeWindow) noexcept { nativeWindow_ = nativeWindow; }
    void handle_native_menu_command(std::uint32_t command, World& world);
    bool initialize();
    void draw(render::Renderer& renderer, World& world, Seconds dt, FrameIndex frame);
    void shutdown();

    bool register_panel(EditorPanel panel);
    bool unregister_panel(std::string_view id);
    bool set_panel_visible(std::string_view id, bool visible);
    bool panel_visible(std::string_view id) const noexcept;

    void set_project_root(std::string path);
    void set_layout_path(std::string path);
    void set_display_size(float width, float height) noexcept;
    bool save_layout();
    bool load_layout();
    void reset_layout();
    void set_theme(std::string theme);
    const EditorLayoutState& layout() const noexcept { return layout_; }
    const std::string& last_status() const noexcept { return lastStatus_; }
    void push_console(std::string message);
};

} // namespace editor
} // namespace shinkou

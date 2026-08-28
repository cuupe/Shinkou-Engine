#pragma once

#include "shinkou/Types.h"
#include "shinkou/editor/DockLayout.h"
#include "shinkou/editor/FileSystem.h"
#include "shinkou/editor/EditorUiModel.h"
#include "shinkou/render/Renderer.h"
#include "shinkou/ui/InputBridge.h"
#include "shinkou/ui/Components.h"
#include "shinkou/ui/Media.h"
#include "shinkou/ui/Style.h"
#include "shinkou/ui/Theme.h"
#if defined(SHINKOU_WITH_UIKIT)
#include "shinkou/editor/UiKitPanelHost.h"
#endif
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shinkou {
class World;
class GameObject;
namespace input { class InputSystem; }

namespace editor {

struct EditorLayoutState {
    std::uint32_t layoutVersion{2};
    std::string theme{"dark"};
    std::string workspace{"Default"};
    std::string projectRoot{"."};
    std::string layoutFile{"Saved/Editor/Layouts/Default.json"};
    std::string dockLayoutFile{"Saved/Editor/Layouts/Default.dock.json"};
    std::string styleFile{"Saved/Editor/Styles/Default.json"};
    std::string fontPath{};
    float fontSize{14.0f};
    std::string backgroundColor{"#0e1117"};
    std::string backgroundImage{};
    bool compactControls{false};
    bool reduceMotion{false};
    ObjectId selectedObject{0};
    float uiScale{1.0f};
    bool allowDocking{true};
    bool showToolbar{true};
    bool showStatusBar{true};
    bool showHierarchy{true};
    bool showInspector{true};
    bool showViewport{true};
    bool showGame{false};
    bool showAssets{true};
    bool showConsole{true};
    bool showProfiler{false};
    bool showRenderGraph{false};
    bool showSettings{false};
    bool showMedia{false};
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
    bool closeable{true};
    float minWidth{160.0f};
    float minHeight{100.0f};
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
    std::string assetFilter_;
    std::string selectedAsset_;
    std::vector<FileEntry> projectFiles_;
    bool initialized_{false};
    bool uiContextOwned_{false};
    void* nativeWindow_{nullptr};
    void* nativeMenu_{nullptr};
    bool assetsDirty_{true};
    float displayWidth_{1280.0f};
    float displayHeight_{720.0f};
    float appliedUiScale_{1.0f};
    std::string appliedFontPath_;
    float appliedFontSize_{0.0f};
    DockWorkspace dockWorkspace_{};
    ui::ThemeRegistry themeRegistry_{};
    ui::UiStyleConfig styleConfig_{};
    ui::MediaPanel mediaPanel_{{"editor-media", "Media", ui::MediaKind::Video, {"asset://preview", "video/*", "Preview"}, true, true, true, true, 16.0f}};
    ui::ComponentDocument uiComponents_{};
    ui::UiRuntime uiRuntime_{};
    ui::InputBridgeStats uiInputStats_{};
    FileSystemService fileSystem_{};
#if defined(SHINKOU_WITH_UIKIT)
    std::unique_ptr<UiKitPanelHost> uiKitPanels_;
#endif
    World* activeWorld_{nullptr};

    void register_builtin_panels();
    void build_default_workspace();
    void sync_workspace_visibility() noexcept;
    void sync_page_visibility() noexcept;
    void refresh_asset_cache();
    void poll_editor_files();
    void draw_toolbar(render::Renderer& renderer, World& world);
    void draw_main_menu(render::Renderer& renderer, World& world);
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
    void draw_media();
    void draw_status_bar(const render::Renderer& renderer);
    void draw_registered_panels(render::Renderer& renderer, World& world, FrameIndex frame, Seconds dt);
    void dispatch_command(EditorCommand command, std::string_view target, World& world);
    void install_native_menu();
    void uninstall_native_menu();
    bool save_layout_file();
    bool load_layout_file();
    bool save_workspace_file();
    bool load_workspace_file();
    bool save_style_file();
    bool load_style_file();
    void sync_style_to_layout() noexcept;
    void sync_layout_to_style() noexcept;
    void apply_theme();

public:
    void set_native_window(void* nativeWindow) noexcept { nativeWindow_ = nativeWindow; }
    void handle_native_menu_command(std::uint32_t command, World& world);
    bool initialize();
    void process_input(const input::InputSystem& input, World& world);
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
    void set_style_file(std::string path);
    ui::UiStyleConfig& ui_style() noexcept { return styleConfig_; }
    const ui::UiStyleConfig& ui_style() const noexcept { return styleConfig_; }
    FileSystemService& file_system() noexcept { return fileSystem_; }
    const FileSystemService& file_system() const noexcept { return fileSystem_; }
    ui::ThemeRegistry& theme_registry() noexcept { return themeRegistry_; }
    const ui::ThemeRegistry& theme_registry() const noexcept { return themeRegistry_; }
    DockWorkspace& dock_workspace() noexcept { return dockWorkspace_; }
    const DockWorkspace& dock_workspace() const noexcept { return dockWorkspace_; }
    ui::UiRuntime& ui_runtime() noexcept { return uiRuntime_; }
    const ui::UiRuntime& ui_runtime() const noexcept { return uiRuntime_; }
    const ui::InputBridgeStats& ui_input_stats() const noexcept { return uiInputStats_; }
    const EditorLayoutState& layout() const noexcept { return layout_; }
    const std::string& last_status() const noexcept { return lastStatus_; }
    void push_console(std::string message);
};

} // namespace editor
} // namespace shinkou

#pragma once

#include "shinkou/editor/DockLayout.h"
#include "shinkou/render/RenderTypes.h"
#include "shinkou/ui/Render.h"
#include "shinkou/editor/EditorWorldViewport.h"
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace shinkou { class World; }
namespace shinkou::render { class Renderer; }
namespace shinkou::editor {
class EditorLayer;
struct EditorObjectTreeNode;
struct EditorInspectorField;

// Presentation adapter only. Commands, async assets, documents and undo stay
// in EditorLayer; upstream widgets own layout, docking, focus and hit testing.
class EditorImGuiView final {
    std::string iniPath_;
    std::string theme_;
    float scale_{0};
    float fontScale_{1};
    float fontSize_{0};
    math::Vec2 dockSize_{};
    std::string fontPath_;
    float scaleEdit_{1},fontSizeEdit_{17};
    std::array<EditorWorldViewport,2> worldViews_;
    bool resetDock_{false};
    bool plotOwned_{false};
    bool iconsReady_{false};
    bool grid_{true};
    bool localGizmo_{true};
    int operation_{0};
    bool gizmoEditing_{false};
    bool viewportVisible_{false};
    std::array<char, 256> objectFilter_{};
    std::array<char, 256> assetFilter_{};
    std::array<char, 256> textFilter_{};
    std::array<char, 256> rename_{};
    std::string actionPath_;
    std::string actionKind_;
    DockRect viewport_{}; // Physical client pixels, matching ImGui Win32 coordinates.
    std::unordered_map<std::string, ui::Rect> regions_; // Logical QA ingress.
    struct RegionSurface { std::uintptr_t window{}; ui::Rect pixels{}; };
    std::unordered_map<std::string,RegionSurface> regionSurfaces_;
    struct Image { render::ResourceHandle texture{}; std::uint64_t revision{0}; };
    std::unordered_map<std::string, Image> images_;
    std::unordered_map<std::string, std::array<char, 2048>> editBuffers_;
    std::unordered_map<std::string,PropertyValue> propertyDrafts_;
    std::string activeProperty_;
    std::uint64_t fileRevision_{~std::uint64_t{0}};
    std::string fileScope_;
    std::string assetTab_;
    std::vector<std::size_t> files_;
    std::size_t visibleAssets_{0};
    std::size_t drawCommands_{0};
    std::uint64_t lastDrawCalls_{0};
    std::uint64_t frameDrawCalls_{0};
    std::uint64_t inspectedObject_{0};
    float assetScroll_{0};
    std::string focused_;
    std::string focusPanel_{"assets"};
    std::unordered_map<std::string,bool> panelWasVisible_;

    const char* label(const EditorLayer&, const char* chinese, const char* english) const;
    std::string title(const EditorLayer&, const char* id) const;
    void region(const EditorLayer&, const std::string& id);
    void region_rect(const EditorLayer&,const std::string&,ui::Rect screen);
    void alias_region(const std::string& alias,const std::string& source);
    bool button(EditorLayer&, const char* chinese, const char* english, const char* id);
    bool begin_panel(EditorLayer&, const char* id, int flags = 0);
    void workspace(EditorLayer&, render::Renderer&, World&);
    void shortcuts(EditorLayer&, World&);
    void rebuild_fonts(EditorLayer&);
    void hierarchy(EditorLayer&, World&);
    void object_node(EditorLayer&, const EditorObjectTreeNode&);
    void inspector(EditorLayer&, render::Renderer&, World&);
    void field(EditorLayer&, const EditorInspectorField&);
    void assets(EditorLayer&);
    void viewport(EditorLayer&, render::Renderer&, World&, bool game);
    void preview(EditorLayer&, render::Renderer&);
    void asset_options(EditorLayer&);
    void image(EditorLayer&, render::Renderer&, const std::string&,
               const std::shared_ptr<const ui::UiImageSnapshot>&, float height);
    void console(EditorLayer&);
    void profiler(EditorLayer&, const render::Renderer&);
    void render_graph(EditorLayer&, render::Renderer&);
    void settings(EditorLayer&);
    void build(EditorLayer&, World&);
    void media(EditorLayer&, render::Renderer&, World&);
    void recovery(EditorLayer&, World&);
public:
    void initialize(EditorLayer&);
    void apply_theme(EditorLayer&);
    void reset_docking() noexcept { resetDock_ = true; }
    void draw(EditorLayer&, render::Renderer&, World&, float dt, std::uint64_t frame);
    void shutdown(render::Renderer*);
    DockRect viewport() const noexcept { return viewport_; }
    bool viewport_visible() const noexcept { return viewportVisible_; }
    const auto& regions() const noexcept { return regions_; }
    const auto& region_surfaces() const noexcept { return regionSurfaces_; }
    std::size_t draw_commands() const noexcept { return drawCommands_; }
    std::size_t visible_assets() const noexcept { return visibleAssets_; }
    float asset_scroll() const noexcept { return assetScroll_; }
    const char* asset_filter() const noexcept { return assetFilter_.data(); }
    const std::string& focused() const noexcept { return focused_; }
    const std::string& asset_tab() const noexcept { return assetTab_; }
    float font_pixels() const noexcept { return fontSize_*fontScale_; }
    std::string framebuffer_size() const { return std::to_string(worldViews_[0].width())+"x"+std::to_string(worldViews_[0].height()); }
};
}

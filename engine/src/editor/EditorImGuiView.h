#pragma once

#include "shinkou/editor/DockLayout.h"
#include "shinkou/editor/EditorHierarchy.h"
#include "shinkou/editor/EditorPointerCapture.h"
#include "shinkou/editor/EditorPreferences.h"
#include "shinkou/editor/EditorUiPerformance.h"
#include "shinkou/editor/EditorWorldViewport.h"
#include "shinkou/render/RenderTypes.h"
#include "shinkou/ui/Render.h"
#include "shinkou/ui/Style.h"
#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace shinkou {
class World;
}
namespace shinkou::input {
class InputSystem;
}
namespace shinkou::render {
class Renderer;
}
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
    float scaleEdit_{1}, fontSizeEdit_{17};
    std::array<EditorWorldViewport, 2> worldViews_;
    bool resetDock_{false};
    bool plotOwned_{false};
    bool iconsReady_{false};
    EditorInputFrame inputFrame_;
    std::string bindingCaptureAction_;
    bool compact_{false};
    bool settingsSession_{false}, settingsEdited_{false};
    EditorPreferences settingsDraft_;
    ui::UiStyleConfig styleDraft_;
    std::string languageDraft_, settingsMessage_;
    bool toolbarDraft_{true}, statusBarDraft_{true}, dockingDraft_{true};
    int settingsPage_{0};
    std::array<char, 128> settingsSearch_{}, commandSearch_{};
    std::optional<EditorKeyBinding> bindingCandidate_;
    std::string bindingConflict_;
    int commandSelection_{0};
    std::vector<const EditorActionDefinition *> commandMatches_;
    std::uint64_t cameraHintRevision_{0};
    std::string cameraHint_, cameraHintLanguage_;
    bool gizmoEditing_{false};
    int cameraMouseButton_{-1};
    EditorPointerCapture pointerCapture_;
    bool viewportLeftPress_{false};
    bool gizmoConsumedPress_{false};
    bool viewportVisible_{false};
    bool sceneViewportVisible_{false};
    std::array<char, 256> textFilter_{};
    std::array<char, 256> rename_{};
    std::string actionPath_;
    std::string actionKind_;
    DockRect viewport_{}; // Physical client pixels, matching ImGui Win32 coordinates.
    std::unordered_map<std::string, ui::Rect> regions_; // Logical QA ingress.
    struct RegionSurface {
        std::uintptr_t window{};
        ui::Rect pixels{};
    };
    std::unordered_map<std::string, RegionSurface> regionSurfaces_;
    bool qaEnabled_{false};
    EditorHierarchy hierarchyRows_;
    std::uint64_t hierarchyWidthRevision_{~std::uint64_t{0}};
    std::uint64_t hierarchyWidthWorld_{0};
    float hierarchyContentWidth_{0}, hierarchyWidthFontSize_{0}, hierarchyWidthIndent_{0};
    EditorUiPerformance performance_;
    struct Image {
        render::ResourceHandle texture{};
        std::uint64_t revision{0};
    };
    std::unordered_map<std::string, Image> images_;
    std::unordered_map<std::string, std::string> textDrafts_;
    std::unordered_map<std::string, PropertyValue> propertyDrafts_;
    std::string activeProperty_;
    std::string assetTab_;
    std::size_t visibleAssets_{0};
    std::size_t drawCommands_{0};
    std::size_t labeledControls_{0};
    std::uint64_t lastDrawCalls_{0};
    std::uint64_t frameDrawCalls_{0};
    std::uint64_t inspectedObject_{0};
    float assetScroll_{0};
    std::string focused_;
    std::string focusPanel_{"assets"};
    std::unordered_map<std::string, bool> panelWasVisible_;

    const char *label(const EditorLayer &, const char *chinese, const char *english) const;
    std::string title(const EditorLayer &, const char *id) const;
    void region(const EditorLayer &, std::string_view id);
    void region_rect(const EditorLayer &, std::string_view, ui::Rect screen);
    void alias_region(std::string_view alias, std::string_view source);
    bool button(EditorLayer &, const char *chinese, const char *english, const char *id);
    bool begin_panel(EditorLayer &, const char *id, int flags = 0);
    void workspace(EditorLayer &, render::Renderer &, World &);
    void shortcuts(EditorLayer &, World &);
    void rebuild_fonts(EditorLayer &);
    void hierarchy(EditorLayer &, World &);
    void object_row(EditorLayer &, const EditorHierarchyRow &);
    void inspector(EditorLayer &, render::Renderer &, World &);
    void field(EditorLayer &, const EditorInspectorField &);
    void assets(EditorLayer &);
    void viewport(EditorLayer &, render::Renderer &, World &, bool game);
    void preview(EditorLayer &, render::Renderer &);
    void asset_options(EditorLayer &);
    void image(EditorLayer &, render::Renderer &, const std::string &,
               const std::shared_ptr<const ui::UiImageSnapshot> &, float height);
    void console(EditorLayer &);
    void profiler(EditorLayer &, const render::Renderer &);
    void render_graph(EditorLayer &, render::Renderer &);
    void settings(EditorLayer &, render::Renderer &);
    void command_palette(EditorLayer &, World &);
    void route_actions(EditorLayer &, World &, EditorInputContext);
    void build(EditorLayer &, World &);
    void media(EditorLayer &, render::Renderer &, World &);
    void media_transport(EditorLayer &, World &, const std::string &prefix, bool options);
    void audio_waveform(EditorLayer &, const std::string &id, float height);
    void registered_panels(EditorLayer &, render::Renderer &, World &, std::uint64_t frame,
                           float dt);
    void recovery(EditorLayer &, World &);

  public:
    void focus_panel(std::string id) {
        focusPanel_ = std::move(id);
    }
    void set_qa_enabled(bool enabled) {
        qaEnabled_ = enabled;
        regions_.clear();
        regionSurfaces_.clear();
        focused_.clear();
    }
    const auto &performance() const noexcept {
        return performance_;
    }
    void initialize(EditorLayer &);
    void process_input(const input::InputSystem &);
    void apply_theme(EditorLayer &);
    void reset_docking() noexcept {
        resetDock_ = true;
    }
    void draw(EditorLayer &, render::Renderer &, World &, float dt, std::uint64_t frame);
    void shutdown(render::Renderer *);
    DockRect viewport() const noexcept {
        return viewport_;
    }
    bool viewport_visible() const noexcept {
        return viewportVisible_;
    }
    const auto &regions() const noexcept {
        return regions_;
    }
    const auto &region_surfaces() const noexcept {
        return regionSurfaces_;
    }
    std::size_t draw_commands() const noexcept {
        return drawCommands_;
    }
    std::size_t labeled_controls() const noexcept {
        return labeledControls_;
    }
    std::size_t visible_assets() const noexcept {
        return visibleAssets_;
    }
    float asset_scroll() const noexcept {
        return assetScroll_;
    }
    const std::string &asset_edit_target() const noexcept {
        return actionPath_;
    }
    const std::string &focused() const noexcept {
        return focused_;
    }
    const std::string &asset_tab() const noexcept {
        return assetTab_;
    }
    float font_pixels() const noexcept {
        return fontSize_ * fontScale_;
    }
    std::string framebuffer_size() const {
        return std::to_string(worldViews_[0].width()) + "x" +
               std::to_string(worldViews_[0].height());
    }
    std::size_t world_model_cache_count() const noexcept {
        return worldViews_[0].cached_model_count();
    }
};
} // namespace shinkou::editor

#pragma once

#include "shinkou/editor/AssetIconLibrary.h"
#include "shinkou/editor/DockLayout.h"
#include "shinkou/editor/EditorUiModel.h"
#include "shinkou/input/InputSystem.h"
#include "shinkou/ui/Render.h"
#include "shinkou/ui/Ui.h"

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace shinkou::render { class Renderer; }
namespace shinkou::ui { class MediaPanel; }

namespace shinkou::editor {

struct EditorLayoutState;
struct FileEntry;

enum class EditorAssetAction : std::uint8_t {
    Open,
    Navigate,
    Refresh,
    Rename,
    NewFolder,
    Delete,
};

enum class EditorAssetView : std::uint8_t {
    SmallList,
    LargeIcons,
    Tree,
    // Compatibility spelling for existing serialized layouts.
    List = SmallList,
};

enum class ViewportNavigation : std::uint8_t { Orbit, Pan, Zoom };

struct EditorUiCallbacks {
    std::function<void(EditorCommand, std::string_view)> command;
    std::function<void(ObjectId)> selectObject;
    std::function<void(ObjectId, std::string)> setObjectName;
    std::function<void(ObjectId, bool)> setObjectActive;
    std::function<void(std::string)> setObjectFilter;
    std::function<void(std::string)> selectAsset;
    std::function<void(EditorAssetAction, std::string, std::string)> assetAction;
    std::function<void(std::string_view)> setViewMode;
    std::function<void(std::string_view)> closePanel;
    std::function<bool(std::string_view, std::string_view)> editField;
    std::function<void(ViewportNavigation, math::Vec2)> navigateViewport;
};

// UE/Slate-inspired editor UI host. The native Windows window is only the
// platform shell; this class owns a persistent composed widget tree, layout
// geometry, hit-test routing and a retained paint list. Scene rendering is
// deliberately outside this class and is constrained by viewport_rect().
class EditorUi final {
public:
    EditorUi();

    bool initialize(EditorUiCallbacks callbacks = {});
    void shutdown() noexcept;
    bool initialized() const noexcept { return initialized_; }

    void set_display_size(float physicalWidth, float physicalHeight, float dpiScale = 1.0f) noexcept;
    void set_asset_directory(std::filesystem::path directory);
    void set_asset_revision(std::uint64_t revision) noexcept;
    // Windows can provide a real native menu bar through EditorLayer. Other
    // platforms keep the same model and receive the retained portable menu
    // strip instead; the core UI never needs to know which one is active.
    void set_native_main_menu_available(bool available) noexcept;
    void set_asset_view(EditorAssetView view) noexcept;
    EditorAssetView asset_view() const noexcept { return assetView_; }
    void invalidate_layout() noexcept {
        layoutPrepared_ = false;
        paintCacheValid_ = false;
        repaintFull_ = true;
        repaintRectValid_ = false;
    }
    void prepare_layout(const EditorLayoutState& layout, DockWorkspace& workspace);
    void process_input(const input::InputSystem& input);

    void build(const EditorUiModel& model, const std::vector<FileEntry>& files,
               const render::Renderer& renderer, const ui::MediaPanel& mediaPanel,
               const EditorLayoutState& layout, const std::vector<std::string>& consoleEntries,
               std::string_view status, DockWorkspace& workspace, float deltaSeconds);

    const ui::UiRenderList& render_list() const noexcept { return renderList_; }
    const ui::UiRuntime& runtime() const noexcept { return runtime_; }
    const editor::DockRect& viewport_rect() const noexcept { return viewportRect_; }
    float dpi_scale() const noexcept { return dpiScale_; }
    std::size_t visible_asset_count() const noexcept { return visibleAssetCount_; }
    const std::unordered_map<std::string, ui::Rect>& interaction_regions() const noexcept { return regionRects_; }
    const std::string& selected_asset() const noexcept { return selectedAsset_; }
    float asset_scroll_offset() const noexcept { return assetScrollOffset_; }
    void select_asset(std::string path) { selectedAsset_ = std::move(path); mark_full_repaint(); paintCacheValid_ = false; }

private:
    struct CommandAction {
        EditorCommand command{EditorCommand::None};
        std::string target;
    };

    struct TabAction {
        DockNodePath path{};
        std::size_t index{0};
        std::string panelId;
    };

    struct AssetAction {
        EditorAssetAction action{EditorAssetAction::Navigate};
        std::string path;
        std::string value;
    };

    ui::UiRuntime runtime_{};
    ui::UiRenderList renderList_{4096};
    EditorUiCallbacks callbacks_{};
    std::unordered_map<std::string, ui::WidgetId> regions_;
    std::unordered_map<std::string, ui::Rect> regionRects_;
    std::unordered_map<std::string, CommandAction> commandActions_;
    std::unordered_map<std::string, TabAction> tabActions_;
    std::unordered_map<std::string, std::string> tabCloseActions_;
    std::unordered_map<std::string, DockNodePath> splitterActions_;
    std::unordered_map<std::string, std::string> floatingHeaderActions_;
    std::unordered_map<std::string, DockRect> floatingHeaderRects_;
    std::unordered_map<std::string, AssetAction> assetActions_;
    std::unordered_map<std::string, AssetAction> assetContextActions_;
    std::unordered_set<std::string> activeRegions_;
    DockWorkspace* workspace_{nullptr};
    DockRect dockArea_{};
    DockRect viewportRect_{};
    DockLayoutOptions dockOptions_{};
    std::string hotRegion_;
    std::string activeRegion_;
    std::string filterText_;
    std::string draggedPanelId_;
    std::string viewMode_{"Perspective"};
    std::filesystem::path assetDirectory_{};
    EditorAssetView assetView_{EditorAssetView::Tree};
    struct AssetDisplayItem {
        std::filesystem::path sortPath;
        std::size_t sourceIndex{0};
        std::string path;
        std::string displayName;
        std::size_t depth{0};
        bool directory{false};
        AssetIconKind icon{AssetIconKind::File};
    };
    std::vector<AssetDisplayItem> assetItems_;
    std::uint64_t assetIndexKey_{0};
    bool assetIndexValid_{false};
    AssetIconLibrary iconLibrary_{};
    std::unordered_set<std::string> collapsedAssetDirectories_;
    std::string selectedAsset_;
    std::string assetFilter_;
    std::string assetContextPath_;
    bool assetContextDirectory_{false};
    bool assetContextBlank_{false};
    bool assetContextOpen_{false};
    ui::Vec2 assetContextPosition_{};
    float assetScrollOffset_{0.0f};
    std::size_t visibleAssetCount_{0};
    std::uint64_t assetRevision_{0};
    std::string assetDeletePath_;
    std::string assetEditTarget_;
    std::string assetEditText_;
    bool assetEditActive_{false};
    EditorAssetAction assetEditAction_{EditorAssetAction::Rename};
    std::string lastAssetClickPath_;
    std::chrono::steady_clock::time_point lastAssetClickTime_{};
    ui::Vec2 pointerDownPosition_{};
    ui::Vec2 floatingDragOffset_{};
    DockRect floatingDragBounds_{};
    bool tabDragActive_{false};
    bool floatingDragActive_{false};
    std::string inspectorName_;
    ObjectId inspectorObject_{0};
    float physicalWidth_{1280.0f};
    float physicalHeight_{720.0f};
    float dpiScale_{1.0f};
    bool initialized_{false};
    bool nativeMainMenuAvailable_{false};
    bool allowDocking_{true};
    std::string openMenuId_;
    bool layoutPrepared_{false};
    float layoutWidth_{0.0f};
    float layoutHeight_{0.0f};
    std::uint64_t layoutGeneration_{0};
    bool layoutShowToolbar_{true};
    bool layoutShowStatusBar_{true};
    std::uint64_t lastPaintKey_{0};
    bool paintCacheValid_{false};
    std::string activeTheme_{"dark"};
    ui::Rect repaintRect_{};
    bool repaintRectValid_{false};
    bool repaintFull_{true};
    std::uint64_t lastModelRevision_{0};
    std::string lastStatusText_;
    ui::Rect regionClip_{};
    bool regionClipActive_{false};
    ui::Rect assetListRect_{}, assetRail_{}, assetThumb_{};
    float assetMaxScroll_{0}, assetDragOffset_{0}, assetItemExtent_{26};
    std::size_t assetColumns_{1};
    bool revealAsset_{false}, assetKeyboardFocus_{false};
    bool controlDown_{false}, shiftDown_{false};
    bool viewportDragging_{false};
    ui::Vec2 viewportPointer_{};
    std::unordered_map<std::string, DockRect> toolRects_;
    std::unordered_map<std::string, float> toolScroll_;
    std::chrono::steady_clock::time_point diagnosticsRefresh_{};
    std::string editFieldId_, editText_, editError_;
    bool editSelectAll_{false}, assetSelectAll_{false};
    std::unordered_map<std::string, EditorInspectorField> inspectorFields_;
    float inspectorScroll_{0}, hierarchyScroll_{0}, hierarchyContentHeight_{0}, hierarchyPageHeight_{0};
    std::unordered_set<ObjectId> collapsedObjects_;
    std::vector<ObjectId> visibleObjects_;
    std::string pendingFocus_;
    bool handle_key(ui::UiEvent& event);
    bool handle_text(ui::UiEvent& event);
    void select_asset_index(std::size_t index);
    void draw_input(ui::Rect bounds, std::string id, std::string_view text, const EditorLayoutState& layout);
    void draw_tools_panel(const DockRect& rect, std::string_view id, const render::Renderer& renderer, const EditorLayoutState& layout);

    ui::WidgetId ensure_region(std::string id, bool focusable = false);
    void begin_regions();
    void prune_regions();
    void set_region(std::string_view id, ui::Rect rect, bool focusable = false);
    void mark_region_repaint(std::string_view id);
    void mark_full_repaint() noexcept { repaintFull_ = true; repaintRectValid_ = false; }
    ui::EventResult on_region(std::string_view id, ui::WidgetId widget, ui::UiEvent& event);
    void activate_region(std::string_view id, ui::Vec2 position);
    void begin_asset_edit(EditorAssetAction action, std::string path, std::string initial);
    void commit_asset_edit();

    void draw_portable_menu(const EditorUiModel& model, const EditorLayoutState& layout);
    void draw_toolbar(const EditorUiModel& model, const render::Renderer& renderer,
                     const EditorLayoutState& layout);
    void draw_dock(const EditorUiModel& model, const std::vector<FileEntry>& files,
                   const render::Renderer& renderer, const ui::MediaPanel& mediaPanel,
                   const EditorLayoutState& layout, const std::vector<std::string>& consoleEntries,
                   std::string_view status);
    void draw_panel_frame(const DockPanelLayout& panel, const EditorLayoutState& layout);
    void draw_tabs(const DockLayoutResult& result, const EditorLayoutState& layout);
    void draw_splitters(const DockLayoutResult& result, const EditorLayoutState& layout);
    void draw_hierarchy(const DockRect& rect, const EditorUiModel& model, const EditorLayoutState& layout);
    void draw_hierarchy_node(const DockRect& rect, const EditorObjectTreeNode& node,
                             const EditorLayoutState& layout, float& cursor, int depth);
    void draw_inspector(const DockRect& rect, const EditorUiModel& model,
                        const std::vector<FileEntry>& files, const EditorLayoutState& layout);
    void draw_viewport(const DockRect& rect, const EditorUiModel& model, const EditorLayoutState& layout);
    void draw_assets(const DockRect& rect, const std::vector<FileEntry>& files,
                     const EditorLayoutState& layout);
    void rebuild_asset_index(const std::vector<FileEntry>& files);
    void draw_asset_context_menu(const DockRect& rect, const EditorLayoutState& layout);
    void draw_console(const DockRect& rect, const render::Renderer& renderer,
                      const std::vector<std::string>& consoleEntries, const EditorLayoutState& layout);
    void draw_generic_panel(const DockRect& rect, std::string_view id,
                            std::string_view title, const EditorLayoutState& layout);
    void draw_button(ui::Rect rect, std::string id, std::string_view label,
                     const EditorLayoutState& layout, bool primary = false);

    ui::ThemeColor color(std::string_view hex, ui::ThemeColor fallback = {}) const;
    static ui::Rect rect(DockRect value) noexcept;
    static DockRect content_rect(DockRect value) noexcept;
    static std::string path_key(std::string_view prefix, const DockNodePath& path);
    static std::string lower(std::string value);
};

} // namespace shinkou::editor

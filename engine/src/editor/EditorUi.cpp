#include "shinkou/editor/EditorUi.h"

#include "shinkou/editor/EditorLayer.h"
#include "shinkou/editor/FileSystem.h"
#include "shinkou/render/Renderer.h"
#include "shinkou/ui/Media.h"
#include "shinkou/ui/Performance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <functional>
#include <iomanip>
#include <sstream>

namespace shinkou::editor {
namespace {

constexpr float kToolbarHeight = 42.0f;
constexpr float kPortableMenuHeight = 30.0f;
constexpr float kStatusHeight = 24.0f;
constexpr float kPanelHeaderHeight = 25.0f;
constexpr float kRowHeight = 24.0f;

DockRect inset(DockRect value, float amount) noexcept {
    value.x += amount;
    value.y += amount;
    value.width = std::max(0.0f, value.width - amount * 2.0f);
    value.height = std::max(0.0f, value.height - amount * 2.0f);
    return value;
}

std::string command_key(std::string_view command, std::string_view target = {}) {
    std::string result = "command:";
    result.append(command);
    if (!target.empty()) { result.push_back(':'); result.append(target); }
    return result;
}

void mix_key(std::uint64_t& hash, std::uint64_t value) noexcept {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
}

void mix_key(std::uint64_t& hash, std::string_view value) noexcept {
    mix_key(hash, std::hash<std::string_view>{}(value));
}

std::string lower_text(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

void erase_last_utf8_code_point(std::string& value) {
    if (value.empty()) return;
    std::size_t index = value.size() - 1;
    while (index > 0 && (static_cast<unsigned char>(value[index]) & 0xC0u) == 0x80u) --index;
    value.erase(index);
}

bool is_direct_child(const std::filesystem::path& path, const std::filesystem::path& directory) {
    return path.parent_path().lexically_normal() == directory.lexically_normal();
}

AssetIconKind asset_icon_kind(const FileEntry& file) {
    if (file.directory) return AssetIconKind::Folder;
    const auto extension = lower_text(file.relativePath.extension().string());
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" || extension == ".tga") return AssetIconKind::Image;
    if (extension == ".fbx" || extension == ".obj" || extension == ".gltf" || extension == ".glb" || extension == ".mesh") return AssetIconKind::Mesh;
    if (extension == ".scene" || extension == ".world" || extension == ".json") return AssetIconKind::Scene;
    if (extension == ".mat" || extension == ".material") return AssetIconKind::Material;
    if (extension == ".wav" || extension == ".mp3" || extension == ".ogg" || extension == ".flac") return AssetIconKind::Audio;
    if (extension == ".mp4" || extension == ".mov" || extension == ".webm") return AssetIconKind::Video;
    if (extension == ".cs" || extension == ".cpp" || extension == ".h" || extension == ".hpp" || extension == ".lua") return AssetIconKind::Script;
    if (extension == ".ttf" || extension == ".otf" || extension == ".ttc") return AssetIconKind::Font;
    if (extension == ".zip" || extension == ".7z" || extension == ".tar" || extension == ".gz") return AssetIconKind::Archive;
    return AssetIconKind::File;
}

std::string_view asset_type_name(AssetIconKind kind) {
    switch (kind) {
    case AssetIconKind::Folder: return "Folder";
    case AssetIconKind::Image: return "Texture";
    case AssetIconKind::Mesh: return "Mesh";
    case AssetIconKind::Scene: return "Scene";
    case AssetIconKind::Material: return "Material";
    case AssetIconKind::Audio: return "Audio";
    case AssetIconKind::Video: return "Video";
    case AssetIconKind::Script: return "Script";
    case AssetIconKind::Font: return "Font";
    case AssetIconKind::Archive: return "Archive";
    case AssetIconKind::File: return "File";
    }
    return "File";
}

std::string_view asset_icon_code(AssetIconKind kind) {
    switch (kind) {
    case AssetIconKind::Folder: return "DIR";
    case AssetIconKind::Image: return "IMG";
    case AssetIconKind::Mesh: return "3D";
    case AssetIconKind::Scene: return "SCN";
    case AssetIconKind::Material: return "MAT";
    case AssetIconKind::Audio: return "AUD";
    case AssetIconKind::Video: return "VID";
    case AssetIconKind::Script: return "{}";
    case AssetIconKind::Font: return "Aa";
    case AssetIconKind::Archive: return "ZIP";
    case AssetIconKind::File: return "FILE";
    }
    return "FILE";
}

std::string_view asset_icon_color_hex(AssetIconKind kind, std::string_view theme) {
    const bool light = theme == "light";
    switch (kind) {
    case AssetIconKind::Folder: return light ? "#E1B24F" : "#D99A32";
    case AssetIconKind::Image: return "#3FA6A0";
    case AssetIconKind::Mesh: return "#8A6FD1";
    case AssetIconKind::Scene: return "#4A86C5";
    case AssetIconKind::Material: return "#C27A3A";
    case AssetIconKind::Audio: return "#B65EB8";
    case AssetIconKind::Video: return "#C85B63";
    case AssetIconKind::Script: return "#4E9A65";
    case AssetIconKind::Font: return "#4B87C2";
    case AssetIconKind::Archive: return "#B4834A";
    case AssetIconKind::File: return light ? "#8D96A3" : "#586170";
    }
    return light ? "#8D96A3" : "#586170";
}

} // namespace

EditorUi::EditorUi() = default;

bool EditorUi::initialize(EditorUiCallbacks callbacks) {
    if (initialized_) return true;
    callbacks_ = std::move(callbacks);
    // SVGs are parsed once at editor startup. The browser never touches the
    // filesystem from its paint path; a marker fallback keeps a damaged
    // installation usable and makes the failure explicit in the UI.
    iconLibrary_.initialize();
    ui::LayoutStyle rootStyle;
    rootStyle.layout = ui::Layout::Overlay;
    rootStyle.hitTestVisible = false;
    rootStyle.size = {ui::AutoSize, ui::AutoSize};
    // UiRuntime already owns the root widget. Creating another child here
    // left the actual root at 0x0, so every pointer hit test failed before it
    // reached the editor regions.
    runtime_.set_style(runtime_.root(), rootStyle);
    initialized_ = true;
    return true;
}

void EditorUi::shutdown() noexcept {
    regions_.clear();
    regionRects_.clear();
    commandActions_.clear();
    tabActions_.clear();
    tabCloseActions_.clear();
    splitterActions_.clear();
    floatingHeaderActions_.clear();
    floatingHeaderRects_.clear();
    assetActions_.clear();
    assetContextActions_.clear();
    activeRegions_.clear();
    commandActions_.clear();
    if (auto* root = runtime_.widget(runtime_.root())) root->children.clear();
    assetItems_.clear();
    assetIndexValid_ = false;
    iconLibrary_.clear();
    workspace_ = nullptr;
    draggedPanelId_.clear();
    openMenuId_.clear();
    assetDirectory_.clear();
    collapsedAssetDirectories_.clear();
    selectedAsset_.clear();
    assetFilter_.clear();
    assetContextPath_.clear();
    assetContextBlank_ = false;
    assetContextOpen_ = false;
    assetDeletePath_.clear();
    assetEditTarget_.clear();
    assetEditText_.clear();
    assetEditActive_ = false;
    lastAssetClickPath_.clear();
    tabDragActive_ = false;
    floatingDragActive_ = false;
    layoutPrepared_ = false;
    layoutWidth_ = 0.0f;
    layoutHeight_ = 0.0f;
    layoutGeneration_ = 0;
    paintCacheValid_ = false;
    repaintRect_ = {};
    repaintRectValid_ = false;
    repaintFull_ = true;
    initialized_ = false;
}

void EditorUi::set_display_size(float physicalWidth, float physicalHeight, float dpiScale) noexcept {
    const float nextWidth = std::max(1.0f, physicalWidth);
    const float nextHeight = std::max(1.0f, physicalHeight);
    const float nextDpiScale = std::clamp(std::isfinite(dpiScale) ? dpiScale : 1.0f, 0.25f, 8.0f);
    const bool changed = physicalWidth_ != nextWidth || physicalHeight_ != nextHeight || dpiScale_ != nextDpiScale;
    physicalWidth_ = nextWidth;
    physicalHeight_ = nextHeight;
    dpiScale_ = nextDpiScale;
    if (auto* root = runtime_.widget(runtime_.root())) {
        root->rect = {0.0f, 0.0f, physicalWidth_ / dpiScale_, physicalHeight_ / dpiScale_};
    }
    if (changed) {
        ui::ui_performance().workload(ui::UiWorkload::Resize);
        layoutPrepared_ = false;
        paintCacheValid_ = false;
        mark_full_repaint();
    }
}

void EditorUi::set_asset_directory(std::filesystem::path directory) {
    directory = directory.lexically_normal();
    if (directory == ".") directory.clear();
    if (assetDirectory_ == directory) return;
    assetDirectory_ = std::move(directory);
    assetScrollOffset_ = 0.0f;
    selectedAsset_.clear();
    assetContextPath_.clear();
    assetContextBlank_ = false;
    assetContextOpen_ = false;
    assetDeletePath_.clear();
    assetEditTarget_.clear();
    assetEditText_.clear();
    assetEditActive_ = false;
    assetIndexValid_ = false;
    paintCacheValid_ = false;
    mark_full_repaint();
}

void EditorUi::set_asset_revision(std::uint64_t revision) noexcept {
    if (assetRevision_ == revision) return;
    assetRevision_ = revision;
    assetIndexValid_ = false;
    paintCacheValid_ = false;
    mark_full_repaint();
}

void EditorUi::set_native_main_menu_available(bool available) noexcept {
    if (nativeMainMenuAvailable_ == available) return;
    nativeMainMenuAvailable_ = available;
    openMenuId_.clear();
    layoutPrepared_ = false;
    paintCacheValid_ = false;
    mark_full_repaint();
}

void EditorUi::set_asset_view(EditorAssetView view) noexcept {
    if (assetView_ == view) return;
    assetView_ = view;
    assetScrollOffset_ = 0.0f;
    assetIndexValid_ = false;
    paintCacheValid_ = false;
    mark_full_repaint();
}

ui::WidgetId EditorUi::ensure_region(std::string id, bool focusable) {
    const auto found = regions_.find(id);
    if (found != regions_.end()) {
        if (auto* widget = runtime_.widget(found->second)) widget->style.focusable = focusable;
        return found->second;
    }
    ui::LayoutStyle style;
    style.layout = ui::Layout::Overlay;
    style.hitTestVisible = true;
    style.focusable = focusable;
    const auto widget = runtime_.create_widget(runtime_.root(), style);
    regions_.emplace(id, widget);
    const auto stableId = id;
    runtime_.set_event_handler(widget, [this, stableId](ui::WidgetId target, ui::UiEvent& event) {
        return on_region(stableId, target, event);
    });
    return widget;
}

void EditorUi::begin_regions() {
    activeRegions_.clear();
    toolRects_.clear();
    commandActions_.clear();
    if (auto* root = runtime_.widget(runtime_.root())) root->children.clear();
    tabActions_.clear();
    tabCloseActions_.clear();
    splitterActions_.clear();
    floatingHeaderActions_.clear();
    floatingHeaderRects_.clear();
    assetActions_.clear();
    assetContextActions_.clear();
    for (const auto& entry : regions_) {
        if (auto* widget = runtime_.widget(entry.second)) {
            widget->visible = false;
            widget->enabled = true;
            widget->style.hitTestVisible = true;
        }
    }
}

void EditorUi::prune_regions() {
    // Region widgets are persistent so pointer capture/focus remain stable,
    // but rows and popup actions are data-driven. Reclaim regions which were
    // not emitted by this paint pass; otherwise every visited asset path would
    // remain in UiRuntime's hit-test tree for the rest of the session.
    for (auto it = regions_.begin(); it != regions_.end();) {
        if (activeRegions_.find(it->first) != activeRegions_.end()) {
            ++it;
            continue;
        }
        runtime_.destroy_widget(it->second);
        regionRects_.erase(it->first);
        it = regions_.erase(it);
    }
    if (!activeRegion_.empty() && activeRegions_.find(activeRegion_) == activeRegions_.end()) {
        activeRegion_.clear();
        tabDragActive_ = false;
        floatingDragActive_ = false;
        draggedPanelId_.clear();
    }
    if (!hotRegion_.empty() && activeRegions_.find(hotRegion_) == activeRegions_.end()) hotRegion_.clear();
}

void EditorUi::set_region(std::string_view id, ui::Rect bounds, bool focusable) {
    if (regionClipActive_) {
        const float right = std::min(bounds.right(), regionClip_.right()), bottom = std::min(bounds.bottom(), regionClip_.bottom());
        bounds.x = std::max(bounds.x, regionClip_.x); bounds.y = std::max(bounds.y, regionClip_.y);
        bounds.width = std::max(0.0f, right - bounds.x); bounds.height = std::max(0.0f, bottom - bounds.y);
    }
    const auto key = std::string(id);
    const auto widgetId = ensure_region(key, focusable);
    if (auto* root = runtime_.widget(runtime_.root())) {
        auto& children = root->children;
        children.erase(std::remove(children.begin(), children.end(), widgetId), children.end());
        children.push_back(widgetId);
    }
    if (auto* widget = runtime_.widget(widgetId)) {
        widget->visible = bounds.width > 0.0f && bounds.height > 0.0f;
        widget->style.focusable = focusable;
        widget->rect = bounds;
    }
    regionRects_[key] = bounds;
    activeRegions_.insert(key);
    if (focusable && key == pendingFocus_) { runtime_.focus(widgetId); pendingFocus_.clear(); }
}

void EditorUi::mark_region_repaint(std::string_view id) {
    if (id.empty()) return;
    const auto found = regionRects_.find(std::string(id));
    if (found == regionRects_.end()) {
        mark_full_repaint();
        return;
    }
    const auto bounds = found->second;
    if (!repaintRectValid_) {
        repaintRect_ = bounds;
        repaintRectValid_ = true;
        return;
    }
    const float left = std::min(repaintRect_.x, bounds.x);
    const float top = std::min(repaintRect_.y, bounds.y);
    const float right = std::max(repaintRect_.x + repaintRect_.width, bounds.x + bounds.width);
    const float bottom = std::max(repaintRect_.y + repaintRect_.height, bounds.y + bounds.height);
    repaintRect_ = {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

ui::EventResult EditorUi::on_region(std::string_view id, ui::WidgetId widget, ui::UiEvent& event) {
    if (event.type == ui::UiEventType::FocusGained || event.type == ui::UiEventType::FocusLost) {
        mark_region_repaint(id); paintCacheValid_ = false;
        if (event.type == ui::UiEventType::FocusGained && id.rfind("field:", 0) == 0) {
            const auto f = inspectorFields_.find(std::string(id));
            if (f != inspectorFields_.end() && f->second.editable && !f->second.boolean && editFieldId_ != id) {
                editFieldId_ = id; editText_ = f->second.value; editSelectAll_ = true; editError_.clear();
            }
        }
        return ui::EventResult::Handled;
    }
    const auto point = event.position;
    if (event.type == ui::UiEventType::Scroll) for (const auto& panel : toolRects_) {
        if (panel.second.contains({point.x,point.y})) {
            toolScroll_[panel.first] = std::max(0.0f, toolScroll_[panel.first] - event.delta.y * 64.0f);
            mark_full_repaint(); paintCacheValid_ = false; return ui::EventResult::Handled;
        }
    }
    if (id == "viewport.surface") {
        if (event.type == ui::UiEventType::Scroll) {
            if (callbacks_.navigateViewport) callbacks_.navigateViewport(ViewportNavigation::Zoom, {0,event.delta.y});
            return ui::EventResult::Handled;
        }
        if (event.type == ui::UiEventType::PointerDown && event.button == ui::PointerButton::Middle) {
            viewportDragging_ = true; viewportPointer_ = point;
            runtime_.capture_pointer(widget); return ui::EventResult::Handled;
        }
        if (event.type == ui::UiEventType::PointerMove && viewportDragging_) {
            const math::Vec2 delta{point.x-viewportPointer_.x, point.y-viewportPointer_.y};
            viewportPointer_ = point;
            if (callbacks_.navigateViewport) callbacks_.navigateViewport(shiftDown_ ? ViewportNavigation::Pan : ViewportNavigation::Orbit, delta);
            return ui::EventResult::Handled;
        }
        if ((event.type == ui::UiEventType::PointerUp && event.button == ui::PointerButton::Middle) || event.type == ui::UiEventType::PointerCancel) {
            viewportDragging_ = false; runtime_.release_pointer(widget); return ui::EventResult::Handled;
        }
    }
    const math::Vec2 dockPoint{point.x, point.y};
    const float clientToolbarHeight = dockArea_.y;
    const float clientStatusHeight = std::max(0.0f, physicalHeight_ / dpiScale_ - dockArea_.y - dockArea_.height);
    if (event.type == ui::UiEventType::PointerMove) {
        if (hotRegion_ != id) {
            mark_region_repaint(hotRegion_);
            mark_region_repaint(id);
            hotRegion_ = std::string(id);
            paintCacheValid_ = false;
        }
        if (activeRegion_ == "assets.scrollbar") {
            const float travel = assetRail_.height - assetThumb_.height;
            if (travel > 0) assetScrollOffset_ = std::clamp((point.y - assetRail_.y - assetDragOffset_) / travel, 0.0f, 1.0f) * assetMaxScroll_;
            mark_full_repaint(); paintCacheValid_ = false;
        }
        if (allowDocking_ && activeRegion_.rfind("split:", 0) == 0 && workspace_) {
            mark_full_repaint();
            const auto split = splitterActions_.find(activeRegion_);
            if (split != splitterActions_.end()) {
                workspace_->drag_splitter(split->second, {point.x, point.y}, dockArea_, dockOptions_);
                layoutPrepared_ = false;
            }
        }
        if (allowDocking_ && workspace_ && !draggedPanelId_.empty() &&
            (activeRegion_.rfind("tab:", 0) == 0 || activeRegion_.rfind("floating-header:", 0) == 0)) {
            const float dx = point.x - pointerDownPosition_.x;
            const float dy = point.y - pointerDownPosition_.y;
            if (activeRegion_.rfind("tab:", 0) == 0 && !tabDragActive_ && std::hypot(dx, dy) >= 5.0f)
                tabDragActive_ = true;

            if (tabDragActive_) {
                mark_full_repaint();
                const bool overDock = dockArea_.contains(dockPoint);
                std::optional<DockTabHit> tabHit;
                std::string targetPanel;
                std::size_t targetIndex = DockWorkspace::npos;
                if (overDock) {
                    tabHit = workspace_->hit_test_tab(dockArea_, dockPoint, dockOptions_);
                    if (tabHit && tabHit->panelId != draggedPanelId_) {
                        targetPanel = tabHit->panelId;
                        targetIndex = tabHit->tabIndex;
                        const auto source = tabActions_.find(activeRegion_);
                        if (source != tabActions_.end() && source->second.path == tabHit->stackPath &&
                            source->second.index < targetIndex && targetIndex > 0) {
                            --targetIndex;
                        }
                    } else {
                        const auto snapshot = workspace_->layout(dockArea_, dockOptions_);
                        for (auto it = snapshot.panels.rbegin(); it != snapshot.panels.rend(); ++it) {
                            if (it->renderable && it->rect.contains(dockPoint) && it->panelId != draggedPanelId_) {
                                targetPanel = it->panelId;
                                break;
                            }
                        }
                    }
                    if (!targetPanel.empty() && workspace_->move_tab(draggedPanelId_, targetPanel, targetIndex)) {
                        floatingDragActive_ = false;
                        layoutPrepared_ = false;
                        paintCacheValid_ = false;
                    }
                } else if (!floatingDragActive_) {
                    const float width = physicalWidth_ / dpiScale_;
                    const float height = physicalHeight_ / dpiScale_;
                    const float windowWidth = std::min(360.0f, std::max(220.0f, width - 24.0f));
                    const float windowHeight = std::min(240.0f, std::max(140.0f, height - clientToolbarHeight - clientStatusHeight - 24.0f));
                    const DockRect floatingBounds{
                        std::clamp(point.x - windowWidth * 0.5f, 0.0f, std::max(0.0f, width - windowWidth)),
                        std::clamp(point.y - 14.0f, clientToolbarHeight, std::max(clientToolbarHeight, height - clientStatusHeight - windowHeight)),
                        windowWidth, windowHeight};
                    if (workspace_->float_panel(draggedPanelId_, floatingBounds)) {
                        floatingDragBounds_ = floatingBounds;
                        floatingDragOffset_ = {windowWidth * 0.5f, 14.0f};
                        floatingDragActive_ = true;
                        layoutPrepared_ = false;
                        paintCacheValid_ = false;
                    }
                }
                if (floatingDragActive_ && !overDock) {
                    floatingDragBounds_.x = point.x - floatingDragOffset_.x;
                    floatingDragBounds_.y = point.y - floatingDragOffset_.y;
                    const float width = physicalWidth_ / dpiScale_;
                    const float height = physicalHeight_ / dpiScale_;
                    floatingDragBounds_.x = std::clamp(floatingDragBounds_.x, 0.0f,
                                                       std::max(0.0f, width - floatingDragBounds_.width));
                    floatingDragBounds_.y = std::clamp(floatingDragBounds_.y, clientToolbarHeight,
                                                       std::max(clientToolbarHeight, height - clientStatusHeight - floatingDragBounds_.height));
                    workspace_->move_floating(draggedPanelId_, floatingDragBounds_);
                    layoutPrepared_ = false;
                    paintCacheValid_ = false;
                }
            }
        }
        if (allowDocking_ && workspace_ && floatingDragActive_ && activeRegion_.rfind("floating-header:", 0) == 0 && !tabDragActive_) {
            mark_full_repaint();
            floatingDragBounds_.x = point.x - floatingDragOffset_.x;
            floatingDragBounds_.y = point.y - floatingDragOffset_.y;
            const float width = physicalWidth_ / dpiScale_;
            const float height = physicalHeight_ / dpiScale_;
            floatingDragBounds_.x = std::clamp(floatingDragBounds_.x, 0.0f,
                                               std::max(0.0f, width - floatingDragBounds_.width));
            floatingDragBounds_.y = std::clamp(floatingDragBounds_.y, clientToolbarHeight,
                                               std::max(clientToolbarHeight, height - clientStatusHeight - floatingDragBounds_.height));
            workspace_->move_floating(draggedPanelId_, floatingDragBounds_);
            layoutPrepared_ = false;
            paintCacheValid_ = false;
        }
        return ui::EventResult::Handled;
    }
    if (event.type == ui::UiEventType::Scroll && (id.rfind("field:", 0) == 0 || id == "inspector.background" || id.rfind("component-add:", 0) == 0)) {
        inspectorScroll_ = std::max(0.0f, inspectorScroll_ - event.delta.y * 72.0f);
        mark_full_repaint(); paintCacheValid_ = false; return ui::EventResult::Handled;
    }
    if (event.type == ui::UiEventType::Scroll && (id.rfind("object:", 0) == 0 || id == "hierarchy.background" || id.rfind("object-toggle:", 0) == 0)) {
        hierarchyScroll_ = std::clamp(hierarchyScroll_ - event.delta.y * 72.0f, 0.0f, std::max(0.0f, hierarchyContentHeight_ - hierarchyPageHeight_));
        mark_full_repaint(); paintCacheValid_ = false; return ui::EventResult::Handled;
    }
    if (event.type == ui::UiEventType::PointerDown && event.button == ui::PointerButton::Right &&
        (id.rfind("asset:", 0) == 0 || id == "assets.background" || id.rfind("asset-toggle:", 0) == 0 || id == "assets.scrollbar")) {
        const auto action = assetActions_.find(id.rfind("asset-toggle:", 0) == 0 ? "asset:" + std::string(id.substr(13)) : std::string(id));
        mark_full_repaint();
        if (id == "assets.background" || id == "assets.scrollbar") {
            assetContextPath_ = assetDirectory_.generic_string();
            assetContextDirectory_ = true;
            assetContextBlank_ = true;
            assetContextPosition_ = point;
            assetContextOpen_ = true;
            assetDeletePath_.clear();
            assetEditTarget_.clear();
            assetEditText_.clear();
            assetEditActive_ = false;
            paintCacheValid_ = false;
            return ui::EventResult::Handled;
        }
        if (action != assetActions_.end()) {
            selectedAsset_ = action->second.path;
            if (callbacks_.selectAsset) callbacks_.selectAsset(selectedAsset_);
            assetContextPath_ = action->second.path;
            assetContextDirectory_ = action->second.action == EditorAssetAction::Navigate;
            assetContextBlank_ = false;
            assetContextOpen_ = true;
            assetContextPosition_ = point;
            assetDeletePath_.clear();
            assetEditTarget_.clear();
            assetEditText_.clear();
            assetEditActive_ = false;
            paintCacheValid_ = false;
            return ui::EventResult::Handled;
        }
    }
    if (event.type == ui::UiEventType::Scroll &&
        (id.rfind("asset:", 0) == 0 || id == "assets.background" || id.rfind("asset-toggle:", 0) == 0 || id == "assets.scrollbar")) {
        // Wheel units are backend-dependent. Treat one unit as three rows so
        // a native wheel and a high-resolution precision wheel feel similar.
        const float wheelRows = event.delta.y != 0.0f ? event.delta.y : 1.0f;
        const float itemExtent = assetView_ == EditorAssetView::LargeIcons ? 92.0f :
            assetView_ == EditorAssetView::Tree ? 26.0f : 28.0f;
        assetScrollOffset_ = std::max(0.0f, assetScrollOffset_ - wheelRows * itemExtent * 3.0f);
        mark_full_repaint();
        paintCacheValid_ = false;
        return ui::EventResult::Handled;
    }
    if (event.type == ui::UiEventType::PointerDown && event.button == ui::PointerButton::Left) {
        mark_region_repaint(activeRegion_);
        mark_region_repaint(id);
        activeRegion_ = std::string(id);
        pointerDownPosition_ = point;
        if (id == "assets.scrollbar") {
            assetDragOffset_ = assetThumb_.contains(point) ? point.y - assetThumb_.y : assetThumb_.height * 0.5f;
            const float travel = assetRail_.height - assetThumb_.height;
            if (travel > 0) assetScrollOffset_ = std::clamp((point.y - assetRail_.y - assetDragOffset_) / travel, 0.0f, 1.0f) * assetMaxScroll_;
            mark_full_repaint(); paintCacheValid_ = false;
        }
        if (assetContextOpen_ && id.rfind("asset-context:", 0) != 0 &&
            id != "asset-delete-confirm" && id != "asset-delete-cancel") {
            assetContextPath_.clear();
            assetContextBlank_ = false;
            assetContextOpen_ = false;
            assetContextActions_.clear();
            paintCacheValid_ = false;
        }
        if (!openMenuId_.empty() && id.rfind("menu:", 0) != 0 && id.rfind("menu-item:", 0) != 0) {
            openMenuId_.clear();
            paintCacheValid_ = false;
        }
        draggedPanelId_.clear();
        tabDragActive_ = false;
        floatingDragActive_ = false;
        if (id.rfind("tab:", 0) == 0 && allowDocking_) {
            const auto action = tabActions_.find(std::string(id));
            if (action != tabActions_.end()) draggedPanelId_ = action->second.panelId;
        } else if (id.rfind("floating-header:", 0) == 0 && allowDocking_) {
            const auto action = floatingHeaderActions_.find(std::string(id));
            const auto bounds = floatingHeaderRects_.find(std::string(id));
            if (action != floatingHeaderActions_.end() && bounds != floatingHeaderRects_.end()) {
                draggedPanelId_ = action->second;
                floatingDragBounds_ = bounds->second;
                floatingDragOffset_ = {point.x - bounds->second.x, point.y - bounds->second.y};
                floatingDragActive_ = true;
            }
        }
        runtime_.capture_pointer(widget);
        return ui::EventResult::Handled;
    }
    if (event.type == ui::UiEventType::PointerUp && event.button == ui::PointerButton::Left) {
        const auto active = activeRegion_;
        mark_region_repaint(active);
        mark_region_repaint(id);
        activeRegion_.clear();
        const bool wasDrag = tabDragActive_ || floatingDragActive_;
        draggedPanelId_.clear();
        tabDragActive_ = false;
        floatingDragActive_ = false;
        runtime_.release_pointer(widget);
        const auto hit = regionRects_.find(std::string(id));
        if (!wasDrag && !active.empty() && active == id && hit != regionRects_.end() && hit->second.contains(point)) activate_region(id, point);
        return ui::EventResult::Handled;
    }
    if (event.type == ui::UiEventType::PointerCancel) {
        mark_region_repaint(activeRegion_);
        activeRegion_.clear();
        draggedPanelId_.clear();
        tabDragActive_ = false;
        floatingDragActive_ = false;
        runtime_.release_pointer(widget);
        return ui::EventResult::Handled;
    }
    if (event.type == ui::UiEventType::KeyDown && id.rfind("asset:", 0) == 0) {
        const auto key = lower(event.control);
        if (key.find("enter") != std::string::npos || key.find("return") != std::string::npos ||
            key.find("space") != std::string::npos) {
            activate_region(id, point);
            return ui::EventResult::Handled;
        }
        if (key.find("delete") != std::string::npos) {
            const auto action = assetActions_.find(std::string(id));
            if (action != assetActions_.end()) {
                assetDeletePath_ = action->second.path;
                paintCacheValid_ = false;
            }
            return ui::EventResult::Handled;
        }
    }
    if (event.type == ui::UiEventType::KeyDown && lower(event.control).find("escape") != std::string::npos &&
        assetContextOpen_) {
        assetContextPath_.clear();
        assetContextBlank_ = false;
        assetContextOpen_ = false;
        assetContextActions_.clear();
        paintCacheValid_ = false;
        return ui::EventResult::Handled;
    }
    if (event.type == ui::UiEventType::TextInput && id == "hierarchy.filter") {
        if (!event.text.empty()) {
            filterText_.append(event.text);
            mark_full_repaint();
            if (callbacks_.setObjectFilter) callbacks_.setObjectFilter(filterText_);
        }
        return ui::EventResult::Handled;
    }
    if (event.type == ui::UiEventType::KeyDown && id == "hierarchy.filter") {
        const auto key = lower(event.control);
        if (key.find("backspace") != std::string::npos && !filterText_.empty()) {
            erase_last_utf8_code_point(filterText_);
            mark_full_repaint();
            if (callbacks_.setObjectFilter) callbacks_.setObjectFilter(filterText_);
        }
        return ui::EventResult::Handled;
    }
    if (event.type == ui::UiEventType::TextInput && id == "assets.filter") {
        if (!event.text.empty()) {
            assetFilter_.append(event.text);
            assetScrollOffset_ = 0.0f;
            mark_full_repaint();
            paintCacheValid_ = false;
        }
        return ui::EventResult::Handled;
    }
    if (event.type == ui::UiEventType::KeyDown && id == "assets.filter") {
        const auto key = lower(event.control);
        if (key.find("backspace") != std::string::npos && !assetFilter_.empty()) {
            erase_last_utf8_code_point(assetFilter_);
            assetScrollOffset_ = 0.0f;
            mark_full_repaint();
            paintCacheValid_ = false;
        }
        return ui::EventResult::Handled;
    }
    if (event.type == ui::UiEventType::TextInput && id == "asset.rename") {
        if (!event.text.empty()) {
            assetEditText_.append(event.text);
            mark_full_repaint();
            paintCacheValid_ = false;
        }
        return ui::EventResult::Handled;
    }
    if (event.type == ui::UiEventType::KeyDown && id == "asset.rename") {
        const auto key = lower(event.control);
        if (key.find("backspace") != std::string::npos && !assetEditText_.empty()) {
            erase_last_utf8_code_point(assetEditText_);
            mark_full_repaint();
            paintCacheValid_ = false;
        } else if (key.find("escape") != std::string::npos) {
            assetEditTarget_.clear();
            assetEditText_.clear();
            assetEditActive_ = false;
            runtime_.clear_focus();
            paintCacheValid_ = false;
        } else if (key.find("enter") != std::string::npos || key.find("return") != std::string::npos) {
            commit_asset_edit();
        }
        return ui::EventResult::Handled;
    }
    return ui::EventResult::Continue;
}

void EditorUi::begin_asset_edit(EditorAssetAction action, std::string path, std::string initial) {
    assetEditAction_ = action;
    assetEditTarget_ = std::move(path);
    assetEditText_ = std::move(initial);
    assetEditActive_ = true;
    assetSelectAll_ = true;
    assetContextOpen_ = false;
    mark_full_repaint();
    assetContextPath_.clear();
    assetContextBlank_ = false;
    assetContextActions_.clear();
    assetDeletePath_.clear();
    paintCacheValid_ = false;
}

void EditorUi::commit_asset_edit() {
    if (!assetEditActive_ || assetEditText_.empty()) return;
    const auto action = assetEditAction_;
    auto path = std::move(assetEditTarget_);
    auto value = std::move(assetEditText_);
    assetEditTarget_.clear();
    assetEditText_.clear();
    assetEditActive_ = false;
    runtime_.clear_focus();
    paintCacheValid_ = false;
    if (callbacks_.assetAction) callbacks_.assetAction(action, std::move(path), std::move(value));
}

void EditorUi::activate_region(std::string_view id, ui::Vec2 position) {
    // A committed action may update an unrelated panel (selection, inspector,
    // filesystem contents), so its repaint boundary is the whole editor.
    // Pure hover/press transitions stay on the dirty-rectangle path.
    mark_full_repaint();
    if (id.rfind("field:", 0) == 0) {
        const auto f = inspectorFields_.find(std::string(id));
        if (f == inspectorFields_.end() || !f->second.editable) return;
        if (f->second.boolean) {
            if (callbacks_.editField) callbacks_.editField(id.substr(6), f->second.value == "true" ? "false" : "true");
        } else { editFieldId_ = id; editText_ = f->second.value; editSelectAll_ = true; editError_.clear(); }
        paintCacheValid_ = false; return;
    }
    if (id.rfind("component-add:", 0) == 0) {
        if (callbacks_.command) callbacks_.command(EditorCommand::AddComponent, id.substr(14));
        return;
    }
    if (id.rfind("object-toggle:", 0) == 0) {
        const auto oid = static_cast<ObjectId>(std::stoull(std::string(id.substr(14))));
        if (!collapsedObjects_.erase(oid)) collapsedObjects_.insert(oid);
        paintCacheValid_ = false; return;
    }
    if (id == "assets.scrollbar" || id == "inspector.background" || id == "hierarchy.background") return;
    if (id.rfind("menu-item:", 0) == 0) {
        const auto action = commandActions_.find(std::string(id));
        if (action != commandActions_.end() && action->second.command != EditorCommand::None && callbacks_.command)
            callbacks_.command(action->second.command, action->second.target);
        openMenuId_.clear();
        paintCacheValid_ = false;
        return;
    }
    if (id.rfind("menu:", 0) == 0) {
        const std::string menuId(id.substr(5));
        if (openMenuId_ == menuId) openMenuId_.clear();
        else openMenuId_ = menuId;
        paintCacheValid_ = false;
        return;
    }
    if (id.rfind("tab-close:", 0) == 0) {
        const auto action = tabCloseActions_.find(std::string(id));
        if (action != tabCloseActions_.end() && callbacks_.closePanel) callbacks_.closePanel(action->second);
        paintCacheValid_ = false;
        return;
    }
    if (id.rfind("command:", 0) == 0) {
        const auto action = commandActions_.find(std::string(id));
        if (action != commandActions_.end() && callbacks_.command)
            callbacks_.command(action->second.command, action->second.target);
        return;
    }
    if (id.rfind("asset-context:", 0) == 0) {
        const auto action = assetContextActions_.find(std::string(id));
        if (action == assetContextActions_.end()) return;
        const auto item = action->second;
        assetContextPath_.clear();
        assetContextBlank_ = false;
        assetContextOpen_ = false;
        assetContextActions_.clear();
        switch (item.action) {
        case EditorAssetAction::Open:
            selectedAsset_ = item.path;
            if (callbacks_.selectAsset) callbacks_.selectAsset(item.path);
            if (callbacks_.assetAction) callbacks_.assetAction(EditorAssetAction::Open, item.path, {});
            break;
        case EditorAssetAction::Navigate:
            assetDirectory_ = item.path;
            selectedAsset_.clear();
            lastAssetClickPath_.clear();
            if (callbacks_.assetAction) callbacks_.assetAction(item.action, item.path, {});
            break;
        case EditorAssetAction::Refresh:
            if (callbacks_.assetAction) callbacks_.assetAction(item.action, {}, {});
            break;
        case EditorAssetAction::Rename:
            begin_asset_edit(item.action, item.path, std::filesystem::path(item.path).filename().string());
            break;
        case EditorAssetAction::NewFolder:
            begin_asset_edit(item.action, item.path, "New Folder");
            break;
        case EditorAssetAction::Delete:
            assetDeletePath_ = item.path;
            break;
        }
        paintCacheValid_ = false;
        return;
    }
    if (id.rfind("asset-toggle:", 0) == 0) {
        const std::string path(id.substr(13));
        const auto found = collapsedAssetDirectories_.find(path);
        if (found == collapsedAssetDirectories_.end()) collapsedAssetDirectories_.insert(path);
        else collapsedAssetDirectories_.erase(found);
        paintCacheValid_ = false;
        return;
    }
    if (id == "asset-delete-confirm") {
        if (callbacks_.assetAction && !assetDeletePath_.empty())
            callbacks_.assetAction(EditorAssetAction::Delete, assetDeletePath_, {});
        assetDeletePath_.clear();
        paintCacheValid_ = false;
        return;
    }
    if (id == "asset-delete-cancel") {
        assetDeletePath_.clear();
        paintCacheValid_ = false;
        return;
    }
    if (id == "asset-view:list" || id == "asset-view:small" || id == "asset-view:large" || id == "asset-view:tree") {
        assetView_ = id == "asset-view:tree" ? EditorAssetView::Tree :
            id == "asset-view:large" ? EditorAssetView::LargeIcons : EditorAssetView::SmallList;
        assetScrollOffset_ = 0.0f;
        revealAsset_ = true;
        assetIndexValid_ = false;
        paintCacheValid_ = false;
        return;
    }
    if (id == "assets.background") return;
    if (id == "asset-parent") {
        if (!assetDirectory_.empty()) {
            assetDirectory_ = assetDirectory_.parent_path().lexically_normal();
            if (assetDirectory_ == ".") assetDirectory_.clear();
            selectedAsset_.clear();
            lastAssetClickPath_.clear();
            if (callbacks_.assetAction) callbacks_.assetAction(EditorAssetAction::Navigate,
                                                               assetDirectory_.generic_string(), {});
            paintCacheValid_ = false;
        }
        return;
    }
    if (id == "asset-refresh") {
        if (callbacks_.assetAction) callbacks_.assetAction(EditorAssetAction::Refresh, {}, {});
        paintCacheValid_ = false;
        return;
    }
    if (id.rfind("tab:", 0) == 0) {
        const auto action = tabActions_.find(std::string(id));
        if (action != tabActions_.end() && workspace_) {
            workspace_->activate_tab(action->second.path, action->second.index);
            layoutPrepared_ = false;
        }
        return;
    }
    if (id.rfind("split:", 0) == 0) {
        return;
    }
    if (id.rfind("object:", 0) == 0 && callbacks_.selectObject) {
        selectedAsset_.clear(); editFieldId_.clear(); inspectorScroll_ = 0;
        try { callbacks_.selectObject(static_cast<ObjectId>(std::stoull(std::string(id.substr(7))))); }
        catch (...) { }
        return;
    }
    if (id.rfind("asset:", 0) == 0) {
        const auto action = assetActions_.find(std::string(id));
        if (action == assetActions_.end()) return;
        selectedAsset_ = action->second.path;
        const auto now = std::chrono::steady_clock::now();
        const bool doubleClick = action->second.path == lastAssetClickPath_ &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAssetClickTime_).count() <= 350 &&
            std::hypot(position.x - pointerDownPosition_.x, position.y - pointerDownPosition_.y) <= 8.0f;
        lastAssetClickPath_ = action->second.path;
        lastAssetClickTime_ = now;
        if (doubleClick && action->second.action == EditorAssetAction::Navigate) {
            assetDirectory_ = action->second.path;
            selectedAsset_.clear();
            lastAssetClickPath_.clear();
            if (callbacks_.assetAction) callbacks_.assetAction(EditorAssetAction::Navigate,
                                                               action->second.path, {});
        } else if (doubleClick && callbacks_.assetAction) {
            callbacks_.assetAction(EditorAssetAction::Open, action->second.path, {});
        } else if (callbacks_.selectAsset) {
            callbacks_.selectAsset(action->second.path);
        }
        return;
    }
    if (id == "mode:next" && callbacks_.setViewMode) {
        const std::array<std::string,7> modes{"Perspective","Front","Back","Left","Right","Top","Bottom"};
        const auto it = std::find(modes.begin(),modes.end(),viewMode_);
        viewMode_ = modes[(it == modes.end() ? 0 : static_cast<std::size_t>(it-modes.begin())+1)%modes.size()];
        callbacks_.setViewMode(viewMode_); paintCacheValid_ = false; return;
    }
    if (id.rfind("mode:", 0) == 0 && callbacks_.setViewMode) {
        viewMode_ = std::string(id.substr(5));
        callbacks_.setViewMode(id.substr(5));
        return;
    }
    (void)position;
}

void EditorUi::prepare_layout(const EditorLayoutState& layout, DockWorkspace& workspace) {
    ui::UiTimer timer(ui::UiStage::Layout);
    if (!initialized_) return;
    const float width = physicalWidth_ / dpiScale_;
    const float height = physicalHeight_ / dpiScale_;
    const bool allowDocking = layout.allowDocking;
    if (workspace_ == &workspace && layoutPrepared_ && layoutWidth_ == width && layoutHeight_ == height &&
        allowDocking_ == allowDocking && layoutShowToolbar_ == layout.showToolbar &&
        layoutShowStatusBar_ == layout.showStatusBar) return;
    workspace_ = &workspace;
    allowDocking_ = allowDocking;
    if (auto* root = runtime_.widget(runtime_.root())) root->rect = {0.0f, 0.0f, width, height};
    const float menuHeight = nativeMainMenuAvailable_ ? 0.0f : kPortableMenuHeight;
    const float toolbarHeight = layout.showToolbar ? kToolbarHeight : 0.0f;
    const float statusHeight = layout.showStatusBar ? kStatusHeight : 0.0f;
    dockArea_ = {0.0f, menuHeight + toolbarHeight, width,
                 std::max(0.0f, height - menuHeight - toolbarHeight - statusHeight)};
    dockOptions_.splitterThickness = 5.0f;
    dockOptions_.splitterHitSlop = 5.0f;
    dockOptions_.tabBarHeight = 25.0f;
    dockOptions_.tabWidth = 132.0f;
    dockOptions_.minTabWidth = 72.0f;
    // Keep at least two asset rows reachable below wrapped controls at high DPI.
    workspace.set_minimum_size("assets", {180.0f, 200.0f});
    workspace.set_minimum_size("viewport", {260.0f, 130.0f});
    const auto result = workspace.layout(dockArea_, dockOptions_);
    viewportRect_ = {};
    for (const auto& panel : result.panels) {
        if (panel.panelId != "viewport" || !panel.renderable) continue;
        viewportRect_ = panel.rect;
        if (panel.tabIndex == DockWorkspace::npos) viewportRect_ = content_rect(viewportRect_);
        break;
    }
    layoutPrepared_ = true;
    layoutWidth_ = width;
    layoutHeight_ = height;
    layoutShowToolbar_ = layout.showToolbar;
    layoutShowStatusBar_ = layout.showStatusBar;
    ++layoutGeneration_;
}

void EditorUi::process_input(const input::InputSystem& input) {
    ui::UiTimer timer(ui::UiStage::Input);
    if (!initialized_) return;
    runtime_.begin_frame();
    for (const auto& source : input.events()) {
        if (source.type == input::InputEventType::MouseMove) {
            mark_region_repaint(hotRegion_);
            hotRegion_.clear();
            paintCacheValid_ = false;
            break;
        }
    }
    const auto logical_position = [this](math::Vec2 value) {
        return ui::Vec2{value.x / dpiScale_, value.y / dpiScale_};
    };
    for (const auto& source : input.events()) {
        if (source.type == input::InputEventType::MouseMove) {
            const auto workload = activeRegion_=="assets.scrollbar" ? ui::UiWorkload::Scroll :
                activeRegion_.rfind("split:",0)==0 || tabDragActive_ || floatingDragActive_ ? ui::UiWorkload::Resize :
                viewportDragging_ ? ui::UiWorkload::Input : ui::UiWorkload::Hover;
            ui::ui_performance().workload(workload);
        }
        else if (source.type == input::InputEventType::MouseWheel) ui::ui_performance().workload(ui::UiWorkload::Scroll);
        else if (source.type == input::InputEventType::TextInput) {
            const auto it = regions_.find("assets.filter");
            ui::ui_performance().workload(it != regions_.end() && runtime_.focused() == it->second ? ui::UiWorkload::Filter : ui::UiWorkload::Input);
        } else ui::ui_performance().workload(ui::UiWorkload::Input);
        ui::UiEvent event;
        if (source.type == input::InputEventType::FocusLost) {
            controlDown_ = shiftDown_ = viewportDragging_ = false;
            activeRegion_.clear(); draggedPanelId_.clear(); tabDragActive_ = floatingDragActive_ = false;
            runtime_.release_pointer(); mark_full_repaint(); paintCacheValid_ = false;
            continue;
        }
        event.position = logical_position(source.type == input::InputEventType::KeyDown || source.type == input::InputEventType::KeyUp || source.type == input::InputEventType::TextInput
            ? input.mouse().position : source.position);
        event.delta = {source.delta.x / dpiScale_, source.delta.y / dpiScale_};
        event.control = source.control;
        event.text = source.text;
        event.repeat = source.repeat;
        event.button = source.control.find("left") != std::string::npos ? ui::PointerButton::Left :
            source.control.find("middle") != std::string::npos ? ui::PointerButton::Middle :
            source.control.find("right") != std::string::npos ? ui::PointerButton::Right : ui::PointerButton::None;
        switch (source.type) {
        case input::InputEventType::MouseMove: event.type = ui::UiEventType::PointerMove; break;
        case input::InputEventType::MouseButtonDown: event.type = ui::UiEventType::PointerDown; break;
        case input::InputEventType::MouseButtonUp: event.type = ui::UiEventType::PointerUp; break;
        case input::InputEventType::MouseWheel: event.type = ui::UiEventType::Scroll; event.delta.y = source.value != 0.0f ? source.value : source.delta.y; break;
        case input::InputEventType::KeyDown: event.type = ui::UiEventType::KeyDown; break;
        case input::InputEventType::KeyUp: event.type = ui::UiEventType::KeyUp; break;
        case input::InputEventType::TextInput: event.type = ui::UiEventType::TextInput; break;
        default: continue;
        }
        if (!handle_key(event) && !handle_text(event)) runtime_.dispatch(event);
    }
}

void EditorUi::build(const EditorUiModel& model, const std::vector<FileEntry>& files,
                    const render::Renderer& renderer, const ui::MediaPanel& mediaPanel,
                    const EditorLayoutState& layout, const std::vector<std::string>& consoleEntries,
                    std::string_view status, DockWorkspace& workspace, float) {
    if (!initialized_) initialize(callbacks_);
    if (layout.showProfiler || layout.showRenderGraph) {
        const auto now = std::chrono::steady_clock::now();
        if (now - diagnosticsRefresh_ >= std::chrono::milliseconds(250)) {
            diagnosticsRefresh_ = now; mark_full_repaint(); paintCacheValid_ = false;
        }
    }
    if (lastStatusText_ != status) { lastStatusText_ = status; mark_full_repaint(); }
    activeTheme_ = layout.theme;
    if (inspectorObject_ != model.selected_object()) { inspectorObject_ = model.selected_object(); editFieldId_.clear(); inspectorScroll_ = 0; }
    if (model.revision() != lastModelRevision_) { mark_full_repaint(); lastModelRevision_ = model.revision(); }
    prepare_layout(layout, workspace);
    std::uint64_t paintKey = 0xcbf29ce484222325ull;
    mix_key(paintKey, model.revision());
    mix_key(paintKey, static_cast<std::uint64_t>(model.selected_object()));
    mix_key(paintKey, model.playing() ? 1u : 0u);
    mix_key(paintKey, model.paused() ? 1u : 0u);
    mix_key(paintKey, model.last_command() == EditorCommand::None ? 0u : static_cast<std::uint64_t>(model.last_command()));
    mix_key(paintKey, layoutGeneration_);
    mix_key(paintKey, layout.theme);
    mix_key(paintKey, static_cast<std::uint64_t>(layout.selectedObject));
    mix_key(paintKey, static_cast<std::uint64_t>(layout.showToolbar));
    mix_key(paintKey, static_cast<std::uint64_t>(layout.showStatusBar));
    mix_key(paintKey, static_cast<std::uint64_t>(nativeMainMenuAvailable_));
    mix_key(paintKey, openMenuId_);
    mix_key(paintKey, static_cast<std::uint64_t>(layout.showHierarchy));
    mix_key(paintKey, static_cast<std::uint64_t>(layout.showInspector));
    mix_key(paintKey, static_cast<std::uint64_t>(layout.showViewport));
    mix_key(paintKey, static_cast<std::uint64_t>(layout.showAssets));
    mix_key(paintKey, static_cast<std::uint64_t>(layout.showConsole));
    mix_key(paintKey, static_cast<std::uint64_t>(layout.showProfiler));
    mix_key(paintKey, static_cast<std::uint64_t>(layout.showRenderGraph));
    mix_key(paintKey, static_cast<std::uint64_t>(layout.showSettings));
    mix_key(paintKey, static_cast<std::uint64_t>(layout.showMedia));
    mix_key(paintKey, std::hash<std::string_view>{}(hotRegion_));
    mix_key(paintKey, std::hash<std::string_view>{}(activeRegion_));
    mix_key(paintKey, std::hash<std::string_view>{}(draggedPanelId_));
    mix_key(paintKey, static_cast<std::uint64_t>(tabDragActive_));
    mix_key(paintKey, static_cast<std::uint64_t>(floatingDragActive_));
    mix_key(paintKey, viewMode_);
    mix_key(paintKey, std::hash<std::string_view>{}(filterText_));
    mix_key(paintKey, assetDirectory_.generic_string());
    mix_key(paintKey, assetRevision_);
    mix_key(paintKey, static_cast<std::uint64_t>(assetView_));
    mix_key(paintKey, assetFilter_);
    mix_key(paintKey, selectedAsset_);
    mix_key(paintKey, static_cast<std::uint64_t>(collapsedAssetDirectories_.size()));
    for (const auto& path : collapsedAssetDirectories_) mix_key(paintKey, path);
    mix_key(paintKey, assetContextPath_);
    mix_key(paintKey, static_cast<std::uint64_t>(assetContextBlank_));
    mix_key(paintKey, static_cast<std::uint64_t>(std::round(assetScrollOffset_)));
    mix_key(paintKey, assetDeletePath_);
    mix_key(paintKey, assetEditTarget_);
    mix_key(paintKey, assetEditText_);
    mix_key(paintKey, static_cast<std::uint64_t>(assetEditActive_));
    mix_key(paintKey, std::hash<std::string_view>{}(status));
    mix_key(paintKey, std::hash<std::string_view>{}(renderer.last_error()));
    mix_key(paintKey, reinterpret_cast<std::uintptr_t>(files.data()));
    mix_key(paintKey, static_cast<std::uint64_t>(files.size()));
    mix_key(paintKey, reinterpret_cast<std::uintptr_t>(consoleEntries.data()));
    mix_key(paintKey, static_cast<std::uint64_t>(consoleEntries.size()));
    if (paintCacheValid_ && paintKey == lastPaintKey_) return;
    rebuild_asset_index(files);
    ui::UiTimer paintTimer(ui::UiStage::Paint);
    begin_regions();
    renderList_.clear();
    renderList_.set_dpi_scale(dpiScale_);
    if (layout.showToolbar) draw_toolbar(model, renderer, layout);
    draw_dock(model, files, renderer, mediaPanel, layout, consoleEntries, status);
    // Paint the portable menu last so an open popup is a true editor overlay
    // and its hit-test widgets are above dock panels in the retained tree.
    if (!nativeMainMenuAvailable_) draw_portable_menu(model, layout);
    const float width = physicalWidth_ / dpiScale_;
    const float height = physicalHeight_ / dpiScale_;
    const auto statusRect = ui::Rect{0.0f, height - kStatusHeight, width, kStatusHeight};
    if (layout.showStatusBar) {
        renderList_.rect(statusRect, color(layout.theme == "light" ? "#F2F3F5" : "#1C1E22"));
        renderList_.border(statusRect, color(layout.theme == "light" ? "#D7DADF" : "#30343A"), 1.0f);
        renderList_.text({12.0f, statusRect.y + 3.0f, std::max(0.0f, width - 232.0f), 18.0f}, status.empty() ? "Ready" : status,
                          color(layout.theme == "light" ? "#5D6470" : "#AAB1BC"), 12.0f,
                          {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
        std::ostringstream stats;
        stats << "UI " << renderList_.size() << "  |  DPI " << std::fixed << std::setprecision(2) << dpiScale_;
        renderList_.text({width - 210.0f, statusRect.y + 3.0f, 198.0f, 18.0f}, stats.str(),
                          color(layout.theme == "light" ? "#5D6470" : "#AAB1BC"), 12.0f,
                          {}, ui::TextAlign::End, ui::TextOverflow::Ellipsis);
    }
    if (repaintFull_ || !repaintRectValid_) {
        renderList_.set_dirty_rect({0.0f, 0.0f, width, height}, true);
    } else {
        const float left = std::max(0.0f, repaintRect_.x - 2.0f);
        const float top = std::max(0.0f, repaintRect_.y - 2.0f);
        const float right = std::min(width, repaintRect_.x + repaintRect_.width + 2.0f);
        const float bottom = std::min(height, repaintRect_.y + repaintRect_.height + 2.0f);
        renderList_.set_dirty_rect({left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)});
    }
    repaintRect_ = {};
    repaintRectValid_ = false;
    repaintFull_ = false;
    prune_regions();
    if (!pendingFocus_.empty()) {
        const auto found = regions_.find(pendingFocus_);
        if (found != regions_.end()) runtime_.focus(found->second);
        pendingFocus_.clear();
    }
    lastPaintKey_ = paintKey;
    paintCacheValid_ = true;
    (void)mediaPanel;
}

void EditorUi::draw_portable_menu(const EditorUiModel& model, const EditorLayoutState& layout) {
    const float width = physicalWidth_ / dpiScale_;
    const float height = physicalHeight_ / dpiScale_;
    const auto background = color(layout.theme == "light" ? "#FFFFFF" : "#202328");
    const auto border = color(layout.theme == "light" ? "#D7DADF" : "#30343A");
    const auto text = color(layout.theme == "light" ? "#24282E" : "#E0E4EA");
    const auto muted = color(layout.theme == "light" ? "#7A828D" : "#8E97A4");
    const auto hover = color(layout.theme == "light" ? "#E8F1FC" : "#303B4A");
    const auto active = color(layout.theme == "light" ? "#D3E3F8" : "#35557D");

    renderList_.rect({0.0f, 0.0f, width, kPortableMenuHeight}, background);
    renderList_.border({0.0f, kPortableMenuHeight - 1.0f, width, 1.0f}, border);

    float x = 0.0f;
    ui::Rect openHeader{};
    for (const auto& menu : model.menus()) {
        const float labelWidth = std::clamp(24.0f + static_cast<float>(menu.label.size()) * 8.0f,
                                            64.0f, 156.0f);
        const ui::Rect header{x, 0.0f, std::min(labelWidth, std::max(0.0f, width - x)), kPortableMenuHeight};
        const auto id = std::string("menu:") + menu.id;
        set_region(id, header);
        if (openMenuId_ == menu.id) openHeader = header;
        if (hotRegion_ == id || openMenuId_ == menu.id)
            renderList_.rect(header, activeRegion_ == id ? active : hover, 4.0f);
        renderList_.text({header.x + 12.0f, header.y + 5.0f,
                          std::max(0.0f, header.width - 20.0f), 18.0f}, menu.label, text, 13.0f,
                          {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
        x += labelWidth;
        if (x >= width) break;
    }

    if (openMenuId_.empty() || openHeader.width <= 0.0f) return;
    const auto menuIt = std::find_if(model.menus().begin(), model.menus().end(), [this](const auto& menu) {
        return menu.id == openMenuId_;
    });
    if (menuIt == model.menus().end()) return;

    float popupWidth = openHeader.width;
    float popupHeight = 8.0f;
    for (const auto& item : menuIt->items) {
        if (item.separator) popupHeight += 9.0f;
        else {
            popupWidth = std::max(popupWidth, 36.0f + static_cast<float>(item.label.size()) * 7.0f +
                                   (item.shortcut.empty() ? 0.0f : 86.0f));
            popupHeight += 28.0f;
        }
    }
    popupWidth = std::clamp(popupWidth, 160.0f, std::min(360.0f, std::max(160.0f, width)));
    popupHeight = std::min(popupHeight, std::max(0.0f, height - kPortableMenuHeight));
    const ui::Rect popup{
        std::clamp(openHeader.x, 0.0f, std::max(0.0f, width - popupWidth)),
        kPortableMenuHeight, popupWidth, popupHeight};
    renderList_.rect({popup.x + 3.0f, popup.y + 3.0f, popup.width, popup.height},
                     color(layout.theme == "light" ? "#00000018" : "#00000055"), 5.0f);
    renderList_.rect(popup, color(layout.theme == "light" ? "#FFFFFF" : "#292C31"), 4.0f);
    renderList_.border(popup, border, 1.0f, 4.0f);

    float y = popup.y + 4.0f;
    for (const auto& item : menuIt->items) {
        if (item.separator) {
            renderList_.border({popup.x + 8.0f, y + 4.0f, popup.width - 16.0f, 1.0f}, border);
            y += 9.0f;
            continue;
        }
        const auto id = std::string("menu-item:") + menuIt->id + ":" + item.id;
        const ui::Rect row{popup.x + 4.0f, y, std::max(0.0f, popup.width - 8.0f), 28.0f};
        set_region(id, row);
        if (auto* widget = runtime_.widget(ensure_region(id))) widget->enabled = item.enabled;
        if (item.enabled && (hotRegion_ == id || activeRegion_ == id))
            renderList_.rect(row, activeRegion_ == id ? active : hover, 3.0f);
        const auto itemColor = item.enabled ? text : muted;
        renderList_.text({row.x + 10.0f, row.y + 5.0f,
                          std::max(0.0f, row.width - (item.shortcut.empty() ? 18.0f : 104.0f)), 18.0f},
                          item.label, itemColor, 12.0f, {}, ui::TextAlign::Start,
                          ui::TextOverflow::Ellipsis);
        if (!item.shortcut.empty())
            renderList_.text({row.right() - 94.0f, row.y + 5.0f, 84.0f, 18.0f}, item.shortcut,
                              muted, 11.0f, {}, ui::TextAlign::End, ui::TextOverflow::Ellipsis);
        commandActions_[id] = {item.command, item.target};
        y += 28.0f;
        if (y >= popup.bottom()) break;
    }
}

void EditorUi::draw_toolbar(const EditorUiModel& model, const render::Renderer& renderer,
                            const EditorLayoutState& layout) {
    const float width = physicalWidth_ / dpiScale_;
    const float menuHeight = nativeMainMenuAvailable_ ? 0.0f : kPortableMenuHeight;
    const auto bar = ui::Rect{0.0f, menuHeight, width, kToolbarHeight};
    const auto panel = color(layout.theme == "light" ? "#F0F1F3" : "#25272B");
    renderList_.rect(bar, panel);
    renderList_.border({0.0f, bar.y + bar.height - 1.0f, bar.width, 1.0f}, color(layout.theme == "light" ? "#D7DADF" : "#30343A"));
    float x = 12.0f;
    const auto button = [&](std::string_view id, std::string_view label, EditorCommand command, bool primary = false) {
        const auto key = command_key(id);
        commandActions_[key] = {command, {}};
        draw_button({x, bar.y + 8.0f, label == "Save" ? 64.0f : 68.0f, 26.0f}, key, label, layout, primary);
        x += label == "Save" ? 72.0f : 76.0f;
    };
    button("play", model.playing() ? "Stop" : "Play", EditorCommand::Play, true);
    button("pause", model.paused() ? "Resume" : "Pause", EditorCommand::Pause);
    button("step", "Step", EditorCommand::Step);
    x += 8.0f;
    button("save", "Save", EditorCommand::SaveScene);
    button("reset", "Reset", EditorCommand::ResetLayout);
    renderList_.text({x + 12.0f, bar.y + 12.0f, 300.0f, 18.0f}, renderer.last_error().empty() ? "Scene Editor" : "Renderer diagnostic",
                      renderer.last_error().empty() ? color(layout.theme == "light" ? "#69717D" : "#9BA3AF") : color("#F29B8F"), 12.0f);
}

void EditorUi::draw_dock(const EditorUiModel& model, const std::vector<FileEntry>& files,
                         const render::Renderer& renderer, const ui::MediaPanel& mediaPanel,
                         const EditorLayoutState& layout, const std::vector<std::string>& consoleEntries,
                         std::string_view status) {
    if (!workspace_) return;
    const auto result = workspace_->layout(dockArea_, dockOptions_);
    for (const auto& panel : result.panels) if (panel.renderable && panel.tabIndex != DockWorkspace::npos)
        renderList_.rect({panel.rect.x,panel.rect.y-25,panel.rect.width,25},color(layout.theme == "light" ? "#F0F1F3" : "#25272B"));
    draw_tabs(result, layout);
    draw_splitters(result, layout);
    for (const auto& panel : result.panels) {
        if (!panel.renderable) continue;
        draw_panel_frame(panel, layout);
        const auto content = panel.tabIndex == DockWorkspace::npos ? content_rect(panel.rect) : panel.rect;
        if (content.width <= 0.0f || content.height <= 0.0f) continue;
        regionClip_ = rect(content); regionClipActive_ = true;
        renderList_.begin_clip(rect(content));
        if (panel.panelId == "hierarchy") draw_hierarchy(content, model, layout);
        else if (panel.panelId == "inspector") draw_inspector(content, model, files, layout);
        else if (panel.panelId == "viewport") draw_viewport(content, model, layout);
        else if (panel.panelId == "assets") draw_assets(content, files, layout);
        else if (panel.panelId == "console") draw_console(content, renderer, consoleEntries, layout);
        else if (panel.panelId == "game") draw_tools_panel(content, "game", renderer, layout);
        else if (panel.panelId == "profiler") draw_tools_panel(content, "profiler", renderer, layout);
        else if (panel.panelId == "render-graph") draw_tools_panel(content, "render-graph", renderer, layout);
        else if (panel.panelId == "settings") draw_tools_panel(content, "settings", renderer, layout);
        else if (panel.panelId == "media") draw_generic_panel(content, "media", mediaPanel.description().title, layout);
        else draw_generic_panel(content, panel.panelId, panel.title, layout);
        renderList_.end_clip();
        regionClipActive_ = false;
    }
    (void)status;
}

void EditorUi::draw_panel_frame(const DockPanelLayout& panel, const EditorLayoutState& layout) {
    const auto bounds = rect(panel.rect);
    const bool viewport = panel.panelId == "viewport";
    const auto surface = color(layout.theme == "light" ? "#FFFFFF" : "#27292E");
    const auto border = color(layout.theme == "high-contrast" ? "#FFFFFF" : layout.theme == "light" ? "#D5D8DE" : "#363A42");
    if (panel.floating) {
        const auto shadow = ui::Rect{bounds.x - 4.0f, bounds.y - 4.0f, bounds.width + 8.0f, bounds.height + 8.0f};
        renderList_.rect(shadow, color(layout.theme == "light" ? "#00000018" : "#00000055"), 6.0f);
    }
    if (!viewport) renderList_.rect(bounds, surface);
    renderList_.border(bounds, border, 1.0f);
    if (panel.tabIndex == DockWorkspace::npos) {
        const auto header = ui::Rect{bounds.x, bounds.y, bounds.width, std::min(kPanelHeaderHeight, bounds.height)};
        if (panel.floating) {
            const auto headerId = std::string("floating-header:") + panel.panelId;
            floatingHeaderActions_[headerId] = panel.panelId;
            floatingHeaderRects_[headerId] = {header.x, header.y, header.width, header.height};
            set_region(headerId, header);
        }
        const bool headerHovered = panel.floating && hotRegion_ == std::string("floating-header:") + panel.panelId;
        const bool headerPressed = panel.floating && activeRegion_ == std::string("floating-header:") + panel.panelId;
        renderList_.rect(header, headerPressed ? color(layout.theme == "light" ? "#D3E3F8" : "#35557D") :
                                headerHovered ? color(layout.theme == "light" ? "#E8F1FC" : "#303B4A") :
                                color(layout.theme == "light" ? "#F3F4F6" : "#2B2E33"));
        renderList_.border({header.x, header.bottom() - 1.0f, header.width, 1.0f}, border);
        renderList_.text({header.x + 10.0f, header.y + 3.0f, std::max(0.0f, header.width - 20.0f), 18.0f}, panel.title,
                          color(layout.theme == "light" ? "#464C55" : "#C8CDD5"), 12.0f, {},
                          ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
    }
}

void EditorUi::draw_tabs(const DockLayoutResult& result, const EditorLayoutState& layout) {
    for (const auto& tab : result.tabs) {
        const auto bounds = rect(tab.rect);
        const auto id = path_key("tab", tab.stackPath) + ":" + std::to_string(tab.tabIndex);
        tabActions_[id] = {tab.stackPath, tab.tabIndex, tab.panelId};
        set_region(id, bounds);
        const auto background = tab.active ? color(layout.theme == "light" ? "#FFFFFF" : "#27292E") :
            color(layout.theme == "light" ? "#E7E9ED" : "#232529");
        renderList_.rect(bounds, background);
        if (tab.active) renderList_.border({bounds.x, bounds.bottom() - 2.0f, bounds.width, 2.0f}, color("#3B82F6"));
        if (tab.panelId == draggedPanelId_ && tabDragActive_)
            renderList_.border({bounds.x + 1.0f, bounds.y + 1.0f, std::max(0.0f, bounds.width - 2.0f), std::max(0.0f, bounds.height - 2.0f)}, color("#78A9E8"), 1.0f, 3.0f);
        const float closeWidth = tab.closeable ? 22.0f : 0.0f;
        renderList_.text({bounds.x + 10.0f, bounds.y + 3.0f,
                          std::max(0.0f, bounds.width - 18.0f - closeWidth), 18.0f}, tab.title,
                          tab.active ? color(layout.theme == "light" ? "#252A31" : "#E4E8EE") : color(layout.theme == "light" ? "#68717D" : "#929AA6"), 12.0f, {},
                          ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
        if (tab.closeable && bounds.width >= 42.0f) {
            const auto closeId = std::string("tab-close:") + tab.panelId;
            const auto close = ui::Rect{bounds.right() - 22.0f, bounds.y + 3.0f, 18.0f, std::max(0.0f, bounds.height - 6.0f)};
            tabCloseActions_[closeId] = tab.panelId;
            set_region(closeId, close);
            const bool hovered = hotRegion_ == closeId || activeRegion_ == closeId;
            if (hovered) renderList_.rect(close, color(layout.theme == "light" ? "#E5E8ED" : "#343942"), 3.0f);
            const auto ink = hovered ? color(layout.theme == "light" ? "#27313C" : "#E6EAF0") :
                color(layout.theme == "light" ? "#7A828D" : "#8A94A1");
            renderList_.line({close.x + 6.0f, close.y + 6.0f}, {close.right() - 6.0f, close.bottom() - 6.0f}, ink, 1.2f);
            renderList_.line({close.right() - 6.0f, close.y + 6.0f}, {close.x + 6.0f, close.bottom() - 6.0f}, ink, 1.2f);
        }
    }
}

void EditorUi::draw_splitters(const DockLayoutResult& result, const EditorLayoutState& layout) {
    for (const auto& splitter : result.splitters) {
        const auto id = path_key("split", splitter.path);
        splitterActions_[id] = splitter.path;
        set_region(id, rect(splitter.hitZone));
        const bool hovered = hotRegion_ == id;
        const bool pressed = activeRegion_ == id;
        renderList_.rect(rect(splitter.bar), pressed ? color("#4C87D7") : hovered ? color("#6E9DDA") :
                          color(layout.theme == "light" ? "#D7DADF" : "#343840"));
    }
}

void EditorUi::draw_hierarchy(const DockRect& value, const EditorUiModel& model, const EditorLayoutState& layout) {
    const auto bounds = inset(value, 8.0f);
    const auto filter = ui::Rect{bounds.x, bounds.y, std::max(0.0f, bounds.width - 52.0f), 26.0f};
    filterText_ = model.object_filter();
    set_region("hierarchy.filter", filter, true);
    renderList_.rect(filter, color(layout.theme == "light" ? "#F1F2F4" : "#1E2024"), 5.0f);
    renderList_.border(filter, color(layout.theme == "light" ? "#D3D7DD" : "#393D45"), 1.0f, 5.0f);
    renderList_.text({filter.x + 9.0f, filter.y + 4.0f, filter.width - 18.0f, 18.0f}, filterText_.empty() ? "Filter" : filterText_,
                      filterText_.empty() ? color(layout.theme == "light" ? "#8A929D" : "#737C89") : color(layout.theme == "light" ? "#353A42" : "#E2E6EC"), 12.0f,
                      {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
    const auto add = command_key("create-empty");
    commandActions_[add] = {EditorCommand::CreateEmpty, {}};
    draw_button({bounds.x + bounds.width - 44.0f, bounds.y, 44.0f, 26.0f}, add, "+", layout);
    draw_input(filter, "hierarchy.filter", filterText_.empty() ? "Filter objects" : filterText_, layout);
    DockRect list{bounds.x, bounds.y+36, bounds.width, std::max(0.0f,bounds.height-36)};
    set_region("hierarchy.background", rect(list), true);
    hierarchyPageHeight_ = list.height;
    visibleObjects_.clear();
    const auto oldClip = regionClip_; regionClip_ = rect(list);
    renderList_.begin_clip(rect(list));
    float cursor = list.y - hierarchyScroll_;
    for (const auto& node : model.object_roots()) draw_hierarchy_node(list, node, layout, cursor, 0);
    hierarchyContentHeight_ = cursor - list.y + hierarchyScroll_;
    hierarchyScroll_ = std::min(hierarchyScroll_, std::max(0.0f, hierarchyContentHeight_-list.height));
    renderList_.end_clip(); regionClip_ = oldClip;
}

void EditorUi::draw_hierarchy_node(const DockRect& value, const EditorObjectTreeNode& node,
                                   const EditorLayoutState& layout, float& cursor, int depth) {
    visibleObjects_.push_back(node.id);
    if (cursor + kRowHeight > value.y && cursor < value.y + value.height) {
    const auto row = ui::Rect{value.x + 6.0f, cursor, std::max(0.0f, value.width - 12.0f), kRowHeight};
    const auto id = std::string("object:") + std::to_string(node.id);
    set_region(id, row, true);
    if (hotRegion_ == id || activeRegion_ == id) renderList_.rect(row, color("#303B4A"), 3);
    if (runtime_.focused() == regions_.at(id)) renderList_.border(row, color("#78A9E8"));
    if (!node.children.empty()) {
        const ui::Rect toggle{row.x + static_cast<float>(depth)*14, row.y, 20, row.height};
        set_region("object-toggle:" + std::to_string(node.id), toggle);
        renderList_.text(toggle, collapsedObjects_.count(node.id) ? ">" : "v", color("#98A1AD"), 11);
    }
    if (node.selected) renderList_.rect(row, color(layout.theme == "light" ? "#DCEBFF" : "#243B5A"), 4.0f);
    const std::string label = std::string(node.active ? "" : "· ") + node.name;
    const float textX = row.x + 22.0f + static_cast<float>(depth) * 14.0f;
    renderList_.text({textX, row.y + 3.0f,
                      std::max(0.0f, row.x + row.width - textX - 8.0f), 18.0f}, label,
                      node.selected ? color(layout.theme == "light" ? "#1A5DBB" : "#D7E6FF") : color(layout.theme == "light" ? "#40464F" : "#C9CED7"), 12.0f,
                      {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
    }
    cursor += kRowHeight;
    if (collapsedObjects_.count(node.id) && filterText_.empty()) return;
    for (const auto& child : node.children) draw_hierarchy_node(value, child, layout, cursor, depth + 1);
}

void EditorUi::draw_inspector(const DockRect& value, const EditorUiModel& model,
                              const std::vector<FileEntry>& files, const EditorLayoutState& layout) {
    const auto bounds = inset(value, 10.0f);
    if (!selectedAsset_.empty()) {
        const auto found = std::find_if(files.begin(), files.end(), [this](const FileEntry& entry) {
            return entry.relativePath.generic_string() == selectedAsset_;
        });
        const auto text = color(layout.theme == "light" ? "#30353C" : "#E1E5EB");
        const auto muted = color(layout.theme == "light" ? "#707884" : "#9AA3AF");
        const auto border = color(layout.theme == "light" ? "#D7DADF" : "#363A42");
        if (found == files.end()) {
            renderList_.text({bounds.x, bounds.y, bounds.width, 20.0f}, "Asset", muted, 11.0f);
            renderList_.text({bounds.x, bounds.y + 26.0f, bounds.width, 20.0f},
                             "Resource is no longer available", text, 12.0f,
                             {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
            return;
        }

        const auto kind = asset_icon_kind(*found);
        const auto accent = color(asset_icon_color_hex(kind, layout.theme));
        const auto icon = ui::Rect{bounds.x, bounds.y + 24.0f, 42.0f, 32.0f};
        const auto iconInk = color(layout.theme == "light" ? "#28313A" : "#EDF1F7");
        const auto iconPaper = color(layout.theme == "light" ? "#DCE3EC" : "#D9E0EA");
        if (!iconLibrary_.paint(renderList_, icon, kind, accent, iconInk, iconPaper))
            renderList_.text(icon, asset_icon_code(kind), text, 9.0f, {}, ui::TextAlign::Center);
        renderList_.text({bounds.x + 54.0f, bounds.y + 22.0f,
                          std::max(0.0f, bounds.width - 54.0f), 22.0f}, found->name, text, 13.0f,
                         {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
        renderList_.text({bounds.x + 54.0f, bounds.y + 45.0f,
                          std::max(0.0f, bounds.width - 54.0f), 18.0f}, asset_type_name(kind), accent, 11.0f,
                         {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
        renderList_.border({bounds.x, bounds.y + 72.0f, bounds.width, 1.0f}, border);
        renderList_.text({bounds.x, bounds.y + 84.0f, bounds.width, 18.0f}, "Location", muted, 10.0f);
        renderList_.text({bounds.x, bounds.y + 103.0f, bounds.width, 18.0f}, found->relativePath.generic_string(), text, 11.0f,
                         {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
        const float previewTop = bounds.y + 132.0f;
        const auto preview = ui::Rect{bounds.x, previewTop, bounds.width,
                                      std::max(0.0f, std::min(176.0f, bounds.y + bounds.height - previewTop))};
        if (preview.height > 0.0f) {
            renderList_.rect(preview, color(layout.theme == "light" ? "#F5F6F8" : "#202226"), 4.0f);
            renderList_.border(preview, border, 1.0f, 4.0f);
            const auto title = kind == AssetIconKind::Image ? "Texture Preview" :
                kind == AssetIconKind::Mesh ? "Mesh Preview" :
                kind == AssetIconKind::Scene ? "Scene Document" :
                kind == AssetIconKind::Material ? "Material Properties" :
                kind == AssetIconKind::Audio ? "Audio Preview" :
                kind == AssetIconKind::Video ? "Video Preview" :
                kind == AssetIconKind::Script ? "Text Resource" :
                kind == AssetIconKind::Font ? "Font Preview" :
                kind == AssetIconKind::Archive ? "Archive Contents" :
                kind == AssetIconKind::Folder ? "Folder Contents" : "File Details";
            renderList_.text({preview.x + 12.0f, preview.y + 10.0f,
                              std::max(0.0f, preview.width - 24.0f), 20.0f}, title, text, 12.0f,
                             {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
            const auto previewIcon = ui::Rect{preview.x + preview.width * 0.5f - 24.0f,
                                              preview.y + 42.0f, 48.0f, 36.0f};
            if (!iconLibrary_.paint(renderList_, previewIcon, kind, accent, iconInk, iconPaper))
                renderList_.text(previewIcon, asset_icon_code(kind), text, 9.0f, {}, ui::TextAlign::Center);
            const auto hint = kind == AssetIconKind::Folder ? "Double-click to open this folder" :
                kind == AssetIconKind::Image ? "No texture preview provider" :
                kind == AssetIconKind::Mesh ? "No mesh preview provider" :
                kind == AssetIconKind::Scene ? "Scene document actions" :
                kind == AssetIconKind::Material ? "Material editor actions" :
                kind == AssetIconKind::Audio || kind == AssetIconKind::Video ? "No media decoder registered" :
                kind == AssetIconKind::Script ? "Text editor actions" :
                kind == AssetIconKind::Font ? "Font preview actions" :
                kind == AssetIconKind::Archive ? "Archive browser actions" : "Resource actions";
            renderList_.text({preview.x + 10.0f, preview.y + 90.0f,
                              std::max(0.0f, preview.width - 20.0f), 32.0f}, hint, muted, 10.0f,
                             {}, ui::TextAlign::Center, ui::TextOverflow::Ellipsis);
        }
        return;
    }
    if (model.selected_object() == 0) {
        renderList_.text({bounds.x, bounds.y, bounds.width, 20.0f}, "Select an object to inspect it.", color(layout.theme == "light" ? "#7B838E" : "#929AA6"), 12.0f);
        return;
    }
    set_region("inspector.background", rect(bounds), true);
    inspectorFields_.clear();
    const float totalHeight = static_cast<float>(model.inspector_fields().size()) * 52.0f + 40 + static_cast<float>(model.component_types().size())*30;
    inspectorScroll_ = std::clamp(inspectorScroll_, 0.0f, std::max(0.0f,totalHeight-bounds.height));
    float y = bounds.y - inspectorScroll_;
    for (const auto& field : model.inspector_fields()) {
        const auto id = "field:" + field.id;
        inspectorFields_[id] = field;
        if (y+52 > bounds.y && y < bounds.y+bounds.height) {
            renderList_.text({bounds.x,y,bounds.width,18}, field.label, color(layout.theme == "light" ? "#59616E" : "#AAB2BE"), 11);
            const ui::Rect input{bounds.x,y+20,bounds.width,26};
            if (field.boolean && field.editable) draw_button(input,id,field.value == "true" ? "On" : "Off",layout,field.value == "true");
            else if (field.editable) draw_input(input,id,field.value,layout);
            else renderList_.text(input,field.value,color("#98A1AD"),12);
        }
        y += 52;
    }
    if (!editError_.empty()) renderList_.text({bounds.x,bounds.y+bounds.height-22,bounds.width,20},editError_,color("#F29B8F"),11);
    renderList_.text({bounds.x,y,bounds.width,20}, "Add component", color("#98A1AD"),12); y += 28;
    for (const auto& type : model.component_types()) {
        if (y+26 > bounds.y && y < bounds.y+bounds.height) draw_button({bounds.x,y,bounds.width,26},"component-add:"+type,type,layout);
        y += 30;
    }
}

void EditorUi::draw_viewport(const DockRect& value, const EditorUiModel&, const EditorLayoutState& layout) {
    const auto bounds = value;
    set_region("viewport.surface", rect(value), true);
    const auto gridColor = color(layout.theme == "light" ? "#FFFFFF22" : "#FFFFFF14");
    const float grid = 32.0f;
    const float right = bounds.x + bounds.width;
    const float bottom = bounds.y + bounds.height;
    for (float x = bounds.x; x < right; x += grid) renderList_.line({x, bounds.y}, {x, bottom}, gridColor, 1.0f);
    for (float y = bounds.y; y < bottom; y += grid) renderList_.line({bounds.x, y}, {right, y}, gridColor, 1.0f);
    renderList_.line({bounds.x + bounds.width * 0.5f, bounds.y}, {bounds.x + bounds.width * 0.5f, bottom}, color("#4C87D744"), 1.0f);
    renderList_.line({bounds.x, bounds.y + bounds.height * 0.5f}, {right, bounds.y + bounds.height * 0.5f}, color("#4C87D744"), 1.0f);
    const float modeStart = std::max(bounds.x+8,right-142);
    draw_button({modeStart,bounds.y+8,134,24},"mode:next",viewMode_+" >",layout);
    const float titleWidth = std::max(0.0f, modeStart - bounds.x - 24.0f);
    renderList_.text({bounds.x + 12.0f, bounds.y + 12.0f, titleWidth, 18.0f}, "Scene Viewport",
                     color(layout.theme == "light" ? "#DDE2E8" : "#A6AFBC"), 12.0f,
                     {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
    renderList_.text({bounds.x + 12.0f, bottom - 26.0f, std::max(0.0f, bounds.width - 24.0f), 18.0f},
                     "MMB orbit · Shift+MMB pan · Wheel zoom · F frame", color("#9AA3AF"), 11.0f,
                     {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
}

void EditorUi::rebuild_asset_index(const std::vector<FileEntry>& files) {
    ui::UiTimer timer(ui::UiStage::Index);
    std::uint64_t key = 0xcbf29ce484222325ull;
    mix_key(key, assetRevision_);
    mix_key(key, assetDirectory_.generic_string());
    mix_key(key, assetFilter_);
    mix_key(key, static_cast<std::uint64_t>(assetView_));
    mix_key(key, static_cast<std::uint64_t>(files.size()));
    mix_key(key, reinterpret_cast<std::uintptr_t>(files.data()));
    for (const auto& path : collapsedAssetDirectories_) mix_key(key, path);
    if (assetIndexValid_ && assetIndexKey_ == key) return;

    assetItems_.clear();
    const std::string searchFilter = lower_text(assetFilter_);
    const auto root = assetDirectory_.lexically_normal();
    const auto hidden_by_collapsed_ancestor = [this, &root](const std::filesystem::path& path) {
        auto ancestor = path.parent_path().lexically_normal();
        while (!ancestor.empty() && ancestor != root && ancestor != ".") {
            if (collapsedAssetDirectories_.find(ancestor.generic_string()) != collapsedAssetDirectories_.end()) return true;
            const auto parent = ancestor.parent_path().lexically_normal();
            if (parent == ancestor) break;
            ancestor = parent;
        }
        return false;
    };
    std::unordered_set<std::string> matchingPaths;
    if (!searchFilter.empty() && assetView_ == EditorAssetView::Tree) {
        for (const auto& file : files) {
            if (lower_text(file.name + " " + file.relativePath.generic_string()).find(searchFilter) == std::string::npos) continue;
            auto ancestor = file.relativePath;
            while (!ancestor.empty() && ancestor != root && ancestor != ".") {
                matchingPaths.insert(ancestor.generic_string()); ancestor = ancestor.parent_path();
            }
        }
    }
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto& file = files[index];
        if ((assetView_ == EditorAssetView::SmallList || assetView_ == EditorAssetView::LargeIcons) &&
            !is_direct_child(file.relativePath, assetDirectory_)) continue;
        if (assetView_ == EditorAssetView::Tree && !assetDirectory_.empty()) {
            const auto relative = file.relativePath.lexically_relative(assetDirectory_);
            if (relative.empty() || relative == ".." || relative.generic_string().rfind("../", 0) == 0) continue;
        }
        if (assetView_ == EditorAssetView::Tree && searchFilter.empty() && hidden_by_collapsed_ancestor(file.relativePath)) continue;
        const auto name = lower_text(file.name + " " + file.relativePath.generic_string());
        if (!searchFilter.empty() && name.find(searchFilter) == std::string::npos && !matchingPaths.count(file.relativePath.generic_string())) continue;

        AssetDisplayItem item;
        item.sortPath = file.relativePath;
        item.sourceIndex = index;
        item.path = file.relativePath.generic_string();
        item.directory = file.directory;
        item.icon = asset_icon_kind(file);
        const auto relative = assetDirectory_.empty() ? file.relativePath : file.relativePath.lexically_relative(assetDirectory_);
        item.displayName = relative.filename().string().empty() ? file.name : relative.filename().string();
        if (assetView_ == EditorAssetView::Tree) {
            for (auto it = relative.begin(); it != relative.end(); ++it) ++item.depth;
            if (item.depth > 0) --item.depth;
        }
        assetItems_.push_back(std::move(item));
    }
    std::sort(assetItems_.begin(), assetItems_.end(), [this](const AssetDisplayItem& left, const AssetDisplayItem& right) {
        if (assetView_ == EditorAssetView::Tree) return left.sortPath.compare(right.sortPath) < 0;
        if (left.directory != right.directory) return left.directory > right.directory;
        return left.displayName < right.displayName;
    });
    assetIndexKey_ = key;
    assetIndexValid_ = true;
}

void EditorUi::draw_assets(const DockRect& value, const std::vector<FileEntry>& files, const EditorLayoutState& layout) {
    const auto bounds = inset(value, 10.0f);
    const auto border = color(layout.theme == "light" ? "#D3D7DD" : "#393D45");
    const auto surface = color(layout.theme == "light" ? "#F5F6F8" : "#202226");
    const auto text = color(layout.theme == "light" ? "#3F464F" : "#CBD1DA");
    const auto muted = color(layout.theme == "light" ? "#7A828D" : "#8F98A5");
    const auto hover = color(layout.theme == "light" ? "#E8F1FC" : "#303B4A");
    const auto selected = color(layout.theme == "light" ? "#DCEBFF" : "#243B5A");
    const bool compact = bounds.width < 620.0f;
    const float toolbarHeight = compact ? 64.0f : 34.0f;
    const float controlHeight = 26.0f;

    const auto toolbar = ui::Rect{bounds.x, bounds.y, bounds.width, toolbarHeight};
    renderList_.border({toolbar.x, toolbar.bottom() - 1.0f, toolbar.width, 1.0f}, border);
    const float filterWidth = compact ? bounds.width : std::clamp(bounds.width * 0.32f, 120.0f, 190.0f);
    const auto searchInput = ui::Rect{bounds.x, bounds.y + 3.0f, std::max(0.0f, filterWidth), controlHeight};
    set_region("assets.filter", searchInput, true);
    renderList_.rect(searchInput, surface, 4.0f);
    renderList_.border(searchInput, border, 1.0f, 4.0f);
    renderList_.text({searchInput.x + 8.0f, searchInput.y + 4.0f, std::max(0.0f, searchInput.width - 16.0f), 18.0f},
                     assetFilter_.empty() ? "Search assets" : assetFilter_,
                     assetFilter_.empty() ? muted : text, 11.0f, {}, ui::TextAlign::Start,
                     ui::TextOverflow::Ellipsis);

    draw_input(searchInput, "assets.filter", assetFilter_.empty() ? "Search assets" : assetFilter_, layout);
    const std::string directoryLabel = assetDirectory_.empty() ? "Project" : assetDirectory_.generic_string();
    const float buttonWidth = 56.0f;
    const float refreshWidth = 70.0f;
    const float controlsWidth = buttonWidth * 3.0f + refreshWidth + 16.0f;
    if (!compact) {
        const auto breadcrumb = ui::Rect{searchInput.right() + 8.0f, bounds.y + 3.0f,
                                         std::max(0.0f, bounds.width - searchInput.width - controlsWidth - 16.0f), controlHeight};
        renderList_.text({breadcrumb.x, breadcrumb.y + 4.0f, breadcrumb.width, 18.0f}, directoryLabel,
                         muted, 11.0f, {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
    }
    const float controlsY = compact ? bounds.y + 29.0f : bounds.y + 3.0f;
    draw_button({bounds.x + bounds.width - controlsWidth, controlsY, refreshWidth, controlHeight},
                "asset-refresh", "Refresh", layout);
    draw_button({bounds.x + bounds.width - buttonWidth * 3.0f - 8.0f, controlsY, buttonWidth, controlHeight},
                "asset-view:small", "Small", layout, assetView_ == EditorAssetView::SmallList);
    draw_button({bounds.x + bounds.width - buttonWidth * 2.0f - 4.0f, controlsY, buttonWidth, controlHeight},
                "asset-view:large", "Large", layout, assetView_ == EditorAssetView::LargeIcons);
    draw_button({bounds.x + bounds.width - buttonWidth, controlsY, buttonWidth, controlHeight},
                "asset-view:tree", "Tree", layout, assetView_ == EditorAssetView::Tree);
    float contentTop = toolbar.bottom() + 5.0f;
    if (assetEditActive_) {
        const auto edit = ui::Rect{bounds.x, contentTop, std::max(0.0f, bounds.width - 86.0f), 28.0f};
        set_region("asset.rename", edit, true);
        renderList_.rect(edit, surface, 4.0f);
        renderList_.border(edit, color("#4A86C5"), 1.0f, 4.0f);
        renderList_.text({edit.x + 8.0f, edit.y + 5.0f, std::max(0.0f, edit.width - 16.0f), 18.0f},
                         assetEditText_, text, 12.0f, {}, ui::TextAlign::Start,
                         ui::TextOverflow::Ellipsis);
        draw_input(edit, "asset.rename", assetEditText_, layout);
        if (runtime_.focused() != ensure_region("asset.rename", true)) runtime_.focus(ensure_region("asset.rename", true));
        const auto hint = assetEditAction_ == EditorAssetAction::NewFolder ? "Create folder" : "Rename";
        renderList_.text({edit.right() + 8.0f, edit.y + 5.0f, 72.0f, 18.0f}, hint, muted, 11.0f,
                         {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
        contentTop += 34.0f;
    }

    visibleAssetCount_ = assetItems_.size();

    const float deleteFooterHeight = assetDeletePath_.empty() ? 0.0f : 38.0f;
    const float navigationFooterHeight = assetDirectory_.empty() ? 0.0f : 30.0f;
    const float listBottom = bounds.y + bounds.height - deleteFooterHeight - navigationFooterHeight;
    const float listHeight = std::max(0.0f, listBottom - contentTop);
    const bool largeIcons = assetView_ == EditorAssetView::LargeIcons;
    const bool tree = assetView_ == EditorAssetView::Tree;
    const float rowHeight = largeIcons ? 92.0f : tree ? 26.0f : 28.0f;
    const float itemExtent = largeIcons ? 92.0f : rowHeight;
    const std::size_t columns = largeIcons ? std::max<std::size_t>(1,
        static_cast<std::size_t>(std::floor(std::max(1.0f, bounds.width - 8.0f) / 112.0f))) : 1;
    const std::size_t itemCount = largeIcons ?
        (assetItems_.size() + columns - 1u) / columns : assetItems_.size();
    assetColumns_ = columns; assetItemExtent_ = itemExtent;
    assetListRect_ = {bounds.x, contentTop, bounds.width, listHeight};
    if (revealAsset_) {
        const auto it = std::find_if(assetItems_.begin(), assetItems_.end(), [&](const auto& item) { return item.path == selectedAsset_; });
        if (it != assetItems_.end()) {
            const float y = static_cast<float>(static_cast<std::size_t>(it-assetItems_.begin())/columns)*itemExtent;
            if (y < assetScrollOffset_) assetScrollOffset_ = y;
            if (y+itemExtent > assetScrollOffset_+listHeight) assetScrollOffset_ = y+itemExtent-listHeight;
        }
        revealAsset_ = false;
    }
    const auto visibleRange = ui::visible_range(itemCount, itemExtent, assetScrollOffset_, listHeight, 2);
    const float maxScroll = std::max(0.0f, visibleRange.contentExtent - listHeight);
    assetMaxScroll_ = maxScroll;
    assetScrollOffset_ = std::clamp(assetScrollOffset_, 0.0f, maxScroll);
    const auto repaintRange = ui::visible_range(itemCount, itemExtent, assetScrollOffset_, listHeight, 2);
    set_region("assets.background", {bounds.x, contentTop, bounds.width,
                                      std::max(0.0f, listBottom - contentTop)}, true);
    const auto panelClip = regionClip_; regionClip_ = assetListRect_;
    renderList_.begin_clip(assetListRect_);
    const auto iconInk = color(layout.theme == "light" ? "#28313A" : "#EDF1F7");
    const auto iconPaper = color(layout.theme == "light" ? "#DCE3EC" : "#D9E0EA");
    for (std::size_t rowIndex = repaintRange.first; rowIndex < repaintRange.last; ++rowIndex) {
        const float cursor = contentTop - assetScrollOffset_ + static_cast<float>(rowIndex) * itemExtent;
        if (largeIcons) {
            for (std::size_t column = 0; column < columns; ++column) {
                const std::size_t itemIndex = rowIndex * columns + column;
                if (itemIndex >= assetItems_.size()) break;
                const auto& item = assetItems_[itemIndex];
                const auto& file = files[item.sourceIndex];
                const float cellWidth = std::max(80.0f, (bounds.width - 8.0f - static_cast<float>(columns - 1) * 8.0f) / static_cast<float>(columns));
                const auto cell = ui::Rect{bounds.x + 4.0f + static_cast<float>(column) * (cellWidth + 8.0f), cursor,
                                           cellWidth, itemExtent - 4.0f};
                const auto id = std::string("asset:") + item.path;
                set_region(id, cell, true);
                assetActions_[id] = {file.directory ? EditorAssetAction::Navigate : EditorAssetAction::Open, item.path, {}};
                if (selectedAsset_ == item.path) renderList_.rect(cell, selected, 5.0f);
                else if (hotRegion_ == id || activeRegion_ == id) renderList_.rect(cell, hotRegion_ == id ? hover : color("#35557D"), 5.0f);
                if (runtime_.focused() == ensure_region(id, true)) {
                    renderList_.border({cell.x + 1.0f, cell.y + 1.0f,
                                        std::max(0.0f, cell.width - 2.0f), std::max(0.0f, cell.height - 2.0f)},
                                       color(layout.theme == "light" ? "#4A86C5" : "#78A9E8"), 1.0f, 5.0f);
                }
                const auto iconBounds = ui::Rect{cell.x + cell.width * 0.5f - 25.0f, cell.y + 7.0f, 50.0f, 48.0f};
                if (!iconLibrary_.paint(renderList_, iconBounds, item.icon,
                                        color(asset_icon_color_hex(item.icon, layout.theme)), iconInk, iconPaper))
                    renderList_.text(iconBounds, asset_icon_code(item.icon), text, 9.0f, {}, ui::TextAlign::Center);
                renderList_.text({cell.x + 6.0f, cell.y + 58.0f, std::max(0.0f, cell.width - 12.0f), 18.0f},
                                 item.displayName, text, 11.0f, {}, ui::TextAlign::Center, ui::TextOverflow::Ellipsis);
            }
            continue;
        }

        if (rowIndex >= assetItems_.size()) continue;
        const auto& item = assetItems_[rowIndex];
        const auto& file = files[item.sourceIndex];
        const auto row = ui::Rect{bounds.x, cursor, bounds.width, rowHeight};
        const auto id = std::string("asset:") + item.path;
        set_region(id, row, true);
        assetActions_[id] = {file.directory ? EditorAssetAction::Navigate : EditorAssetAction::Open, item.path, {}};
        if (selectedAsset_ == item.path) {
            renderList_.rect(row, selected, 3.0f);
            renderList_.rect({row.x, row.y, 2.0f, row.height}, color("#4A86C5"));
        } else if (hotRegion_ == id || activeRegion_ == id) {
            renderList_.rect(row, hotRegion_ == id ? hover : color("#35557D"), 3.0f);
        }
        if (runtime_.focused() == ensure_region(id, true)) {
            renderList_.border({row.x + 1.0f, row.y + 1.0f,
                                std::max(0.0f, row.width - 2.0f), std::max(0.0f, row.height - 2.0f)},
                               color(layout.theme == "light" ? "#4A86C5" : "#78A9E8"), 1.0f, 3.0f);
        }
        const float indent = tree ? static_cast<float>(item.depth) * 18.0f : 0.0f;
        const float left = row.x + 6.0f + indent;
        if (tree) {
            const auto guide = color(layout.theme == "light" ? "#D1D7DF" : "#343B46");
            for (std::size_t level = 0; level < item.depth; ++level) {
                const float gx = row.x + 14.0f + static_cast<float>(level) * 18.0f;
                renderList_.line({gx, row.y}, {gx, row.bottom()}, guide, 1.0f);
            }
            if (item.depth > 0) renderList_.line({left - 10.0f, row.y + row.height * 0.5f}, {left + 2.0f, row.y + row.height * 0.5f}, guide, 1.0f);
            if (file.directory) {
                const auto toggleId = std::string("asset-toggle:") + item.path;
                const auto toggle = ui::Rect{left - 4.0f, row.y, 22.0f, row.height};
                set_region(toggleId, toggle);
                const bool collapsed = collapsedAssetDirectories_.find(item.path) != collapsedAssetDirectories_.end();
                const auto chevron = color(layout.theme == "light" ? "#657180" : "#B7C1CF");
                const float cx = toggle.x + 11.0f;
                const float cy = toggle.y + toggle.height * 0.5f;
                if (collapsed) {
                    renderList_.line({cx - 2.0f, cy - 4.0f}, {cx + 3.0f, cy}, chevron, 1.4f);
                    renderList_.line({cx + 3.0f, cy}, {cx - 2.0f, cy + 4.0f}, chevron, 1.4f);
                } else {
                    renderList_.line({cx - 4.0f, cy - 2.0f}, {cx, cy + 2.0f}, chevron, 1.4f);
                    renderList_.line({cx, cy + 2.0f}, {cx + 4.0f, cy - 2.0f}, chevron, 1.4f);
                }
            }
        }
        const auto icon = ui::Rect{left + (tree ? 20.0f : 0.0f), row.y + 3.0f, tree ? 24.0f : 26.0f, row.height - 6.0f};
        if (!iconLibrary_.paint(renderList_, icon, item.icon,
                                color(asset_icon_color_hex(item.icon, layout.theme)), iconInk, iconPaper))
            renderList_.text(icon, asset_icon_code(item.icon), text, 8.0f, {}, ui::TextAlign::Center);
        renderList_.text({icon.right() + 8.0f, row.y + 3.0f,
                          std::max(0.0f, row.right() - icon.right() - 12.0f), row.height - 6.0f},
                         item.displayName, text, 11.0f, {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
    }
    if (assetItems_.empty()) {
        const auto searchFilter = lower_text(assetFilter_);
        renderList_.text({bounds.x + 8.0f, contentTop + 8.0f, std::max(0.0f, bounds.width - 16.0f), 20.0f},
                         searchFilter.empty() ? "This folder is empty" : "No matching assets", muted, 11.0f,
                         {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
    }
    renderList_.end_clip(); regionClip_ = panelClip;
    if (!assetDirectory_.empty()) {
        const auto parent = ui::Rect{bounds.x, bounds.y + bounds.height - navigationFooterHeight, 66.0f, 24.0f};
        draw_button(parent, "asset-parent", "Up", layout);
    }
    if (maxScroll > 0.0f && listHeight > 8.0f) {
        const float railWidth = 12.0f;
        const auto rail = ui::Rect{bounds.x + bounds.width - railWidth - 2.0f, contentTop, railWidth, listHeight};
        const float thumbHeight = std::max(18.0f, listHeight * listHeight / visibleRange.contentExtent);
        const float thumbTravel = std::max(0.0f, listHeight - thumbHeight);
        const auto thumb = ui::Rect{rail.x,
                                    rail.y + (maxScroll > 0.0f ? assetScrollOffset_ / maxScroll * thumbTravel : 0.0f),
                                    rail.width, thumbHeight};
        assetRail_ = rail; assetThumb_ = thumb;
        set_region("assets.scrollbar", rail, true);
        renderList_.rect(rail, color(layout.theme == "light" ? "#D9DDE3" : "#30343B"), 2.0f);
        renderList_.rect(thumb, color(layout.theme == "light" ? "#8B96A5" : "#687383"), 2.0f);
    }
    if (assetContextOpen_) draw_asset_context_menu(value, layout);
    if (!assetDeletePath_.empty()) {
        const auto confirm = ui::Rect{bounds.x, bounds.y + bounds.height - 34.0f, bounds.width, 30.0f};
        renderList_.rect(confirm, color(layout.theme == "light" ? "#FFF4E5" : "#3A2D21"), 4.0f);
        renderList_.text({confirm.x + 8.0f, confirm.y + 5.0f, std::max(0.0f, confirm.width - 152.0f), 18.0f},
                         std::string("Delete ") + std::filesystem::path(assetDeletePath_).filename().string() + "?",
                         color(layout.theme == "light" ? "#805A24" : "#E6B66D"), 11.0f, {},
                         ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
        draw_button({confirm.right() - 136.0f, confirm.y + 2.0f, 62.0f, 26.0f},
                    "asset-delete-cancel", "Cancel", layout);
        draw_button({confirm.right() - 68.0f, confirm.y + 2.0f, 62.0f, 26.0f},
                    "asset-delete-confirm", "Delete", layout, true);
    }
}

void EditorUi::draw_asset_context_menu(const DockRect& value, const EditorLayoutState& layout) {
    const auto bounds = inset(value, 10.0f);
    const auto border = color(layout.theme == "light" ? "#D3D7DD" : "#393D45");
    const auto text = color(layout.theme == "light" ? "#3F464F" : "#CBD1DA");
    const auto muted = color(layout.theme == "light" ? "#7A828D" : "#8F98A5");
    const auto hover = color(layout.theme == "light" ? "#E8F1FC" : "#303B4A");
    assetContextActions_.clear();
    struct ContextItem { std::string label; EditorAssetAction action; std::string path; };
    std::vector<ContextItem> items;
    if (assetContextBlank_) {
        items.push_back({"New Folder", EditorAssetAction::NewFolder, assetDirectory_.generic_string()});
    } else {
        if (assetContextDirectory_) items.push_back({"Open", EditorAssetAction::Navigate, assetContextPath_});
        else items.push_back({"Open", EditorAssetAction::Open, assetContextPath_});
        if (!assetContextPath_.empty()) {
            items.push_back({"Rename...", EditorAssetAction::Rename, assetContextPath_});
            items.push_back({"Delete", EditorAssetAction::Delete, assetContextPath_});
        }
        if (assetContextDirectory_) items.push_back({"New Folder", EditorAssetAction::NewFolder, assetContextPath_});
    }
    items.push_back({"Refresh", EditorAssetAction::Refresh, {}});
    const float width = 176.0f;
    const float height = 8.0f + static_cast<float>(items.size()) * 28.0f;
    const auto popup = ui::Rect{
        std::clamp(assetContextPosition_.x, bounds.x, std::max(bounds.x, bounds.x + bounds.width - width)),
        std::clamp(assetContextPosition_.y, bounds.y, std::max(bounds.y, bounds.y + bounds.height - height)), width, height};
    renderList_.rect({popup.x + 3.0f, popup.y + 3.0f, popup.width, popup.height},
                     color(layout.theme == "light" ? "#00000018" : "#00000055"), 5.0f);
    renderList_.rect(popup, color(layout.theme == "light" ? "#FFFFFF" : "#292C31"), 4.0f);
    renderList_.border(popup, border, 1.0f, 4.0f);
    float y = popup.y + 4.0f;
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        const auto id = std::string("asset-context:") + std::to_string(index);
        const auto row = ui::Rect{popup.x + 4.0f, y, popup.width - 8.0f, 28.0f};
        set_region(id, row, true);
        assetContextActions_[id] = {item.action, item.path, {}};
        if (hotRegion_ == id || activeRegion_ == id) renderList_.rect(row, activeRegion_ == id ? color("#35557D") : hover, 3.0f);
        renderList_.text({row.x + 10.0f, row.y + 5.0f, row.width - 20.0f, 18.0f}, item.label,
                         item.action == EditorAssetAction::Delete ? color("#E58B82") : text, 11.0f,
                         {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
        y += 28.0f;
    }
    (void)muted;
}

void EditorUi::draw_console(const DockRect& value, const render::Renderer& renderer,
                            const std::vector<std::string>& consoleEntries, const EditorLayoutState& layout) {
    const auto bounds = inset(value, 10.0f);
    const auto clear = command_key("console-clear"); commandActions_[clear] = {EditorCommand::ClearConsole,{}};
    draw_button({bounds.x+bounds.width-68,bounds.y,68,24},clear,"Clear",layout);
    float cursor = bounds.y + 30;
    const auto text = color(layout.theme == "light" ? "#5E6671" : "#9CA5B2");
    const auto rows = static_cast<std::size_t>(std::max(0.0f,bounds.height-30)/18);
    const auto first = consoleEntries.size() > rows ? consoleEntries.size()-rows : 0;
    for (std::size_t index = first; index < consoleEntries.size(); ++index) {
        const auto& entry = consoleEntries[index];
        if (cursor + 18.0f > bounds.y + bounds.height) break;
        renderList_.text({bounds.x, cursor, bounds.width, 18.0f}, entry, text, 11.0f,
                         {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
        cursor += 18.0f;
    }
    if (!renderer.last_error().empty() && cursor + 18.0f <= bounds.y + bounds.height)
        renderList_.text({bounds.x, cursor, bounds.width, 18.0f}, renderer.last_error(), color("#F29B8F"), 11.0f,
                         {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
}

void EditorUi::draw_generic_panel(const DockRect& value, std::string_view id,
                                  std::string_view title, const EditorLayoutState& layout) {
    const auto bounds = inset(value, 12.0f);
    renderList_.text({bounds.x, bounds.y, bounds.width, 20.0f}, title, color(layout.theme == "light" ? "#3F464F" : "#D2D7DE"), 13.0f);
    renderList_.text({bounds.x, bounds.y + 28.0f, bounds.width, 18.0f}, id=="media" ? "No media preview provider is registered." : "No retained panel content is registered.", color(layout.theme == "light" ? "#78808A" : "#8D97A4"), 11.0f);
}

void EditorUi::draw_button(ui::Rect bounds, std::string id, std::string_view label,
                           const EditorLayoutState& layout, bool primary) {
    set_region(id, bounds, true);
    const bool hovered = hotRegion_ == id;
    const bool pressed = activeRegion_ == id;
    const bool focused = runtime_.focused() == ensure_region(id, true);
    const auto fill = primary ? color(pressed ? "#245FAE" : hovered ? "#3278D0" : "#2E6FBE") :
        color(layout.theme == "light" ? (pressed ? "#D2D5DA" : hovered ? "#E4E6EA" : "#F1F2F4") :
              (pressed ? "#3A3E46" : hovered ? "#33373F" : "#2C2F35"));
    const bool highContrast = layout.theme == "high-contrast";
    const auto foreground = highContrast && pressed ? color("#000000") : primary ? color("#FFFFFF") : color(layout.theme == "light" ? "#454B54" : "#D7DCE4");
    renderList_.rect(bounds, highContrast && pressed ? color("#FFFFFF") : highContrast && hovered ? color("#0066CC") : fill, 5.0f);
    renderList_.border(bounds, color(highContrast ? "#FFFFFF" : layout.theme == "light" ? "#D0D4DA" : "#41464F"), 1.0f, 5.0f);
    if (focused) {
        const auto focusColor = primary ? color("#8DBBFF") : color(layout.theme == "light" ? "#2E6FBE" : "#78A9E8");
        renderList_.border({bounds.x - 1.0f, bounds.y - 1.0f, bounds.width + 2.0f, bounds.height + 2.0f}, focusColor, 1.0f, 6.0f);
    }
    renderList_.text({bounds.x + 6.0f, bounds.y + 3.0f, std::max(0.0f, bounds.width - 12.0f), 18.0f}, label, foreground, 12.0f,
                      {}, ui::TextAlign::Center, ui::TextOverflow::Ellipsis);
}

ui::ThemeColor EditorUi::color(std::string_view hex, ui::ThemeColor fallback) const {
    if ((hex.size() != 7 && hex.size() != 9) || hex.front() != '#') return fallback;
    unsigned result = 0;
    for (std::size_t i = 1; i < hex.size(); ++i) {
        const char c = hex[i];
        const unsigned digit = c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0') :
            c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10) :
            c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10) : 16u;
        if (digit > 15u) return fallback;
        result = (result << 4u) | digit;
    }
    const float alpha = hex.size() == 9 ? static_cast<float>(result & 0xffu) / 255.0f : 1.0f;
    if (hex.size() == 9) result >>= 8u;
    const ui::ThemeColor parsed{static_cast<float>((result >> 16u) & 0xffu) / 255.0f,
                                static_cast<float>((result >> 8u) & 0xffu) / 255.0f,
                                static_cast<float>(result & 0xffu) / 255.0f, alpha};
    if (activeTheme_ != "high-contrast") return parsed;
    // Apply the high-contrast policy at the final color boundary so every
    // retained panel, hover state and icon follows one consistent palette.
    const float luminance = 0.2126f * parsed.r + 0.7152f * parsed.g + 0.0722f * parsed.b;
    if (alpha < 0.35f) return {1.0f, 1.0f, 1.0f, alpha};
    if (parsed.b > parsed.r * 1.25f && parsed.b > parsed.g * 1.12f && luminance < 0.65f)
        return {0.0f, 0.38f, 0.75f, alpha};
    if (luminance < 0.38f) return {0.0f, 0.0f, 0.0f, alpha};
    return {1.0f, 1.0f, 1.0f, alpha};
}

ui::Rect EditorUi::rect(DockRect value) noexcept { return {value.x, value.y, value.width, value.height}; }

DockRect EditorUi::content_rect(DockRect value) noexcept {
    value.y += std::min(kPanelHeaderHeight, value.height);
    value.height = std::max(0.0f, value.height - kPanelHeaderHeight);
    return value;
}

std::string EditorUi::path_key(std::string_view prefix, const DockNodePath& path) {
    std::string result(prefix);
    result.push_back(':');
    if (path.floating) { result += "floating/" + std::to_string(path.floatingIndex); }
    for (const auto index : path.childIndices) result += "/" + std::to_string(index);
    return result;
}

std::string EditorUi::lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

} // namespace shinkou::editor

#include "shinkou/editor/EditorUi.h"
#include "shinkou/editor/EditorLayer.h"
#include <algorithm>
#include <cmath>

namespace shinkou::editor {
namespace {
std::string key_name(std::string value) {
    const auto colon = value.rfind(':');
    if (colon != std::string::npos) value.erase(0, colon + 1);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    value.erase(std::remove(value.begin(), value.end(), ' '), value.end());
    return value;
}
void backspace(std::string& text) {
    if (text.empty()) return;
    auto i = text.size() - 1;
    while (i && (static_cast<unsigned char>(text[i]) & 0xc0) == 0x80) --i;
    text.erase(i);
}
}

void EditorUi::select_asset_index(std::size_t index) {
    if (index >= assetItems_.size()) return;
    selectedAsset_ = assetItems_[index].path;
    if (callbacks_.selectAsset) callbacks_.selectAsset(selectedAsset_);
    revealAsset_ = true; assetKeyboardFocus_ = true;
    pendingFocus_ = "asset:" + selectedAsset_;
    mark_full_repaint(); paintCacheValid_ = false;
}

bool EditorUi::handle_text(ui::UiEvent& event) {
    if (event.type != ui::UiEventType::TextInput) return false;
    if (controlDown_) return true;
    std::string focused;
    for (const auto& pair : regions_) if (pair.second == runtime_.focused()) { focused = pair.first; break; }
    std::string* text = nullptr;
    bool* all = &editSelectAll_;
    if (!editFieldId_.empty() && focused == editFieldId_) text = &editText_;
    else if (focused == "assets.filter") text = &assetFilter_;
    else if (focused == "hierarchy.filter") text = &filterText_;
    else if (focused == "asset.rename") { text = &assetEditText_; all = &assetSelectAll_; }
    if (!text) return false;
    if (*all) { text->clear(); *all = false; }
    if (text->size() + event.text.size() <= 4096) text->append(event.text);
    if (focused == "hierarchy.filter" && callbacks_.setObjectFilter) callbacks_.setObjectFilter(*text);
    if (focused == "assets.filter") assetScrollOffset_ = 0;
    editError_.clear(); mark_full_repaint(); paintCacheValid_ = false;
    return true;
}

bool EditorUi::handle_key(ui::UiEvent& event) {
    if (event.type != ui::UiEventType::KeyDown && event.type != ui::UiEventType::KeyUp) return false;
    const auto key = key_name(event.control);
    const bool down = event.type == ui::UiEventType::KeyDown;
    if (key.find("ctrl") != std::string::npos || key.find("control") != std::string::npos) { controlDown_ = down; return true; }
    if (key.find("shift") != std::string::npos) { shiftDown_ = down; return true; }
    if (!down) return false;
    std::string focused;
    for (const auto& pair : regions_) if (pair.second == runtime_.focused()) { focused = pair.first; break; }
    const bool enter = key == "enter" || key == "return";
    const auto repaint = [&] { mark_full_repaint(); paintCacheValid_ = false; };
    const auto command = [&](EditorCommand cmd, std::string_view target = {}) { if (callbacks_.command) callbacks_.command(cmd, target); repaint(); };
    if (key == "escape") {
        assetDeletePath_.clear(); assetContextOpen_ = false; assetContextActions_.clear();
        assetEditActive_ = false; assetEditText_.clear(); openMenuId_.clear();
        editFieldId_.clear(); editText_.clear(); editError_.clear();
        viewportDragging_ = false;
        activeRegion_.clear(); runtime_.release_pointer(); runtime_.clear_focus(); repaint(); return true;
    }
    // Deletion confirmation consumes keyboard events until cancelled/confirmed.
    if (!assetDeletePath_.empty()) {
        if (enter) activate_region("asset-delete-confirm", {});
        return true;
    }
    std::string* text = nullptr;
    bool* all = &editSelectAll_;
    if (!editFieldId_.empty() && focused == editFieldId_) text = &editText_;
    else if (focused == "assets.filter") text = &assetFilter_;
    else if (focused == "hierarchy.filter") text = &filterText_;
    else if (focused == "asset.rename") { text = &assetEditText_; all = &assetSelectAll_; }
    if (text) {
        if (controlDown_ && key == "a") { *all = true; repaint(); return true; }
        if (key == "backspace" || key == "delete") {
            if (*all) { text->clear(); *all = false; }
            else if (key == "backspace") backspace(*text);
            if (focused == "hierarchy.filter" && callbacks_.setObjectFilter) callbacks_.setObjectFilter(*text);
            if (focused == "assets.filter") assetScrollOffset_ = 0;
            editError_.clear(); repaint(); return true;
        }
        if (enter) {
            if (focused == "asset.rename") commit_asset_edit();
            else if (!editFieldId_.empty()) {
                if (callbacks_.editField && callbacks_.editField(editFieldId_.substr(6), editText_)) {
                    editFieldId_.clear(); runtime_.clear_focus(); editError_.clear();
                } else editError_ = "Invalid value. Check type and range.";
            } else if (focused == "assets.filter" && !assetItems_.empty()) select_asset_index(0);
            repaint(); return true;
        }
        if (key != "tab") return true;
    }
    if (key == "tab") {
        std::vector<std::pair<std::string, ui::Rect>> order;
        for (const auto& pair : regionRects_) {
            const auto found = regions_.find(pair.first);
            const auto* w = found == regions_.end() ? nullptr : runtime_.widget(found->second);
            if (w && w->visible && w->enabled && w->style.focusable) order.push_back(pair);
        }
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
            if (std::abs(a.second.y - b.second.y) > 2) return a.second.y < b.second.y;
            if (a.second.x != b.second.x) return a.second.x < b.second.x;
            return a.first < b.first;
        });
        if (!order.empty()) {
            auto it = std::find_if(order.begin(), order.end(), [&](const auto& a) { return a.first == focused; });
            auto index = it == order.end() ? (shiftDown_ ? 0u : order.size()-1) : static_cast<std::size_t>(it - order.begin());
            index = (index + (shiftDown_ ? order.size()-1 : 1)) % order.size();
            runtime_.focus(regions_.at(order[index].first));
        }
        return true;
    }
    if (controlDown_) {
        if (key == "s") command(shiftDown_ ? EditorCommand::SaveSceneAs : EditorCommand::SaveScene);
        else if (key == "z") command(EditorCommand::Undo);
        else if (key == "y") command(EditorCommand::Redo);
        else if (key == "n") command(shiftDown_ ? EditorCommand::CreateEmpty : EditorCommand::NewScene);
        else if (key == "o") command(EditorCommand::OpenScene);
        else return false;
        return true;
    }
    const bool assets = focused.rfind("asset:", 0) == 0 || focused == "assets.background" || focused == "assets.scrollbar" || focused.rfind("asset-toggle:", 0) == 0;
    if (assets) {
        if (key == "f5") { if (callbacks_.assetAction) callbacks_.assetAction(EditorAssetAction::Refresh, {}, {}); return true; }
        if (key == "backspace") { activate_region("asset-parent", {}); return true; }
        if (assetItems_.empty()) return true;
        auto it = std::find_if(assetItems_.begin(), assetItems_.end(), [&](const auto& item) { return item.path == selectedAsset_; });
        std::size_t index = it == assetItems_.end() ? 0 : static_cast<std::size_t>(it - assetItems_.begin());
        const auto& item = assetItems_[index];
        if (enter) {
            if (callbacks_.assetAction) callbacks_.assetAction(item.directory ? EditorAssetAction::Navigate : EditorAssetAction::Open, item.path, {});
            repaint(); return true;
        }
        if (key == "f2") { begin_asset_edit(EditorAssetAction::Rename, item.path, item.displayName); repaint(); return true; }
        if (key == "delete") { assetDeletePath_ = item.path; repaint(); return true; }
        const auto page = std::max<std::size_t>(1, static_cast<std::size_t>(assetListRect_.height / assetItemExtent_)) * assetColumns_;
        if (key == "home") index = 0;
        else if (key == "end") index = assetItems_.size() - 1;
        else if (key == "up" || key == "pageup") { const auto n = key == "up" ? assetColumns_ : page; index = index > n ? index - n : 0; }
        else if (key == "down" || key == "pagedown") index = std::min(assetItems_.size()-1, index + (key == "down" ? assetColumns_ : page));
        else if (key == "left" && assetView_ == EditorAssetView::Tree) {
            if (item.directory && !collapsedAssetDirectories_.count(item.path)) { collapsedAssetDirectories_.insert(item.path); repaint(); return true; }
            const auto parent = std::filesystem::path(item.path).parent_path().generic_string();
            const auto p = std::find_if(assetItems_.begin(), assetItems_.end(), [&](const auto& a) { return a.path == parent; });
            if (p != assetItems_.end()) index = static_cast<std::size_t>(p - assetItems_.begin());
        } else if (key == "right" && assetView_ == EditorAssetView::Tree) {
            if (item.directory && collapsedAssetDirectories_.erase(item.path)) { repaint(); return true; }
            if (item.directory) index = std::min(index + 1, assetItems_.size()-1);
        } else if (key == "left") index = index ? index-1 : 0;
        else if (key == "right") index = std::min(index+1, assetItems_.size()-1);
        else return false;
        select_asset_index(index); return true;
    }
    if (focused.rfind("object:", 0) == 0 || focused == "hierarchy.background") {
        if (key == "delete") { command(EditorCommand::DeleteObject); return true; }
        if (key == "f") { command(EditorCommand::FrameSelection); return true; }
        if (!visibleObjects_.empty() && (key == "up" || key == "down" || key == "home" || key == "end")) {
            const auto it = std::find(visibleObjects_.begin(), visibleObjects_.end(), inspectorObject_);
            std::size_t index = it == visibleObjects_.end() ? 0 : static_cast<std::size_t>(it - visibleObjects_.begin());
            if (key == "home") index = 0;
            else if (key == "end") index = visibleObjects_.size()-1;
            else if (key == "up") index = index ? index-1 : 0;
            else index = std::min(index+1, visibleObjects_.size()-1);
            selectedAsset_.clear(); if (callbacks_.selectObject) callbacks_.selectObject(visibleObjects_[index]);
            pendingFocus_ = "object:" + std::to_string(visibleObjects_[index]);
            hierarchyScroll_ = std::clamp(static_cast<float>(index)*24.0f - hierarchyPageHeight_*0.5f, 0.0f, std::max(0.0f, hierarchyContentHeight_-hierarchyPageHeight_));
            repaint(); return true;
        }
    }
    if ((enter || key == "space") && !focused.empty()) { activate_region(focused, {}); return true; }
    if (key == "f") { command(EditorCommand::FrameSelection); return true; }
    return false;
}

void EditorUi::draw_input(ui::Rect bounds, std::string id, std::string_view text, const EditorLayoutState& layout) {
    set_region(id, bounds, true);
    const bool focused = runtime_.focused() == regions_.at(id);
    const auto foreground = color(layout.theme == "light" ? "#30353C" : "#E1E5EB");
    renderList_.rect(bounds, color(layout.theme == "light" ? "#F1F2F4" : "#1E2024"), 4);
    renderList_.border(bounds, color(focused ? "#78A9E8" : hotRegion_ == id ? "#697A90" : layout.theme == "high-contrast" ? "#FFFFFF" : "#41464F"), focused ? 2.0f : 1.0f, 4);
    const bool editing = id == editFieldId_;
    const std::string label = editing ? editText_ : std::string(text);
    if (focused && (id == "asset.rename" ? assetSelectAll_ : editSelectAll_))
        renderList_.rect({bounds.x+5, bounds.y+3, std::max(0.0f, bounds.width-10), bounds.height-6}, color("#35557D"), 2);
    renderList_.text({bounds.x+7, bounds.y+3, std::max(0.0f,bounds.width-14), bounds.height-6}, label + (focused ? "|" : ""), foreground, 12, {}, ui::TextAlign::Start, ui::TextOverflow::Ellipsis);
}
} // namespace shinkou::editor

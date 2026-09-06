#include "shinkou/editor/DockLayout.h"

#include <cmath>
#include <iostream>
#include <string>
#include <algorithm>

namespace {
using namespace shinkou::editor;

DockPanel panel(const char* id, float minWidth = 0.0f) {
    return {id, std::string("Title ") + id, true, true, {minWidth, 20.0f}};
}

bool has_panel(const DockNode& node, std::string_view id) {
    if (node.kind == DockNode::Kind::Leaf) return node.panelId == id;
    if (node.kind == DockNode::Kind::TabStack) for (const auto& tab : node.tabs) if (tab.id == id) return true;
    for (const auto& child : node.children) if (has_panel(child, id)) return true;
    return false;
}

const DockPanelLayout* find_panel_layout(const DockLayoutResult& result, std::string_view id) {
    const auto it = std::find_if(result.panels.begin(), result.panels.end(), [id](const auto& panel) {
        return panel.panelId == id;
    });
    return it == result.panels.end() ? nullptr : &*it;
}

bool near(float left, float right, float epsilon = 0.001f) {
    return std::fabs(left - right) <= epsilon;
}

} // namespace

int main() {
    DockWorkspace layout;
    if (!layout.add_tab("", panel("scene", 100.0f))) return 1;
    if (!layout.add_tab("scene", panel("game", 120.0f))) return 2;
    if (layout.root().kind != DockNode::Kind::TabStack || layout.root().activeTab != 1 || layout.root().tabs.size() != 2) return 3;
    if (!layout.activate_tab("scene") || layout.root().activeTab != 0) return 4;

    if (!layout.set_panel_visibility("game", false) || layout.root().tabs[1].visible) return 5;
    const auto hiddenTabLayout = layout.layout({0.0f, 0.0f, 600.0f, 400.0f});
    if (hiddenTabLayout.panels.size() != 1 || !hiddenTabLayout.panels.front().renderable ||
        hiddenTabLayout.panels.front().panelId != "scene") return 5;
    if (!layout.set_minimum_size("scene", {240.0f, 30.0f})) return 6;
    const auto tabMinimum = layout.minimum_size();
    if (tabMinimum.x != 240.0f || tabMinimum.y != 30.0f) return 7;

    if (!layout.split("game", DockSplitOrientation::Horizontal, 0.5f, panel("inspector", 200.0f))) return 8;
    if (layout.root().kind != DockNode::Kind::Split || layout.root().children.size() != 2) return 9;
    if (layout.minimum_size().x < 440.0f) return 10;
    const auto hit = layout.hit_test_split_bar({0, 0, 1000, 600}, {500, 100});
    if (!hit || hit->node != &layout.root() || hit->orientation != DockSplitOrientation::Horizontal) return 11;
    if (layout.hit_test_split_bar({0, 0, 1000, 600}, {10, 10})) return 12;

    if (!layout.move_tab("game", "scene", 0)) return 13;
    if (!layout.remove_tab("inspector") || !has_panel(layout.root(), "scene") || has_panel(layout.root(), "inspector")) return 14;

    if (!layout.add_floating(DockNode::floating(DockNode::leaf(panel("console")), {20, 30, 300, 200}))) return 15;
    if (!layout.set_minimum_size("console", {80, 60}) || layout.floating().size() != 1) return 16;
    if (!layout.float_panel("scene", {40, 50, 320, 220}) || layout.floating().size() != 2) return 16;
    if (!layout.move_floating("scene", {80, 70, 360, 240}) || layout.floating().back().bounds.x != 80.0f) return 16;

    const std::string json = layout.to_json();
    if (json.find("\"version\":1") == std::string::npos || json.find("\"floating\"") == std::string::npos) return 17;
    DockWorkspace restored;
    const auto decoded = restored.from_json(json);
    if (!decoded || restored.to_json() != json || restored.floating().size() != 2) return 18;
    if (!has_panel(restored.root(), "game") || !has_panel(restored.floating().front(), "console") ||
        !has_panel(restored.floating().back(), "scene")) return 19;

    const std::string beforeBad = restored.to_json();
    if (restored.from_json("{\"version\":1,\"root\":{\"type\":\"split\"}}")) return 20;
    if (restored.to_json() != beforeBad) return 21;
    if (restored.from_json("{\"version\":999,\"root\":{}}")) return 22;
    if (restored.from_json("not json")) return 23;
    const auto unknownFields = restored.from_json("{\"version\":1,\"root\":{\"type\":\"leaf\",\"panelId\":\"a\",\"unknown\":true},\"unknown\":42}");
    if (!unknownFields) return 24;
    if (!has_panel(restored.root(), "a")) return 25;

    DockLayoutOptions options;
    options.splitterThickness = 10.0f;
    options.splitterHitSlop = 5.0f;
    options.tabBarHeight = 30.0f;
    const DockNodePath tabPath{false, DockWorkspace::npos, {1}};
    DockWorkspace geometry(DockNode::split(
        DockNode::leaf(panel("outliner", 180.0f)),
        DockNode::tab_stack({panel("scene", 320.0f), panel("console", 120.0f)}, 1),
        DockSplitOrientation::Horizontal, 0.25f));

    const DockRect workspaceRect{10.0f, 20.0f, 1000.0f, 600.0f};
    const auto computed = geometry.layout(workspaceRect, options);
    if (computed.panels.size() != 3 || computed.tabs.size() != 2 || computed.splitters.size() != 1) return 26;
    const auto* outliner = find_panel_layout(computed, "outliner");
    const auto* scene = find_panel_layout(computed, "scene");
    const auto* console = find_panel_layout(computed, "console");
    if (!outliner || !scene || !console || scene->active || !console->active || !console->renderable) return 27;
    const auto& splitter = computed.splitters.front();
    if (splitter.path != DockNodePath{} || splitter.parentRect.x != workspaceRect.x || splitter.bar.height != workspaceRect.height) return 28;
    if (!near(outliner->rect.width, (workspaceRect.width - options.splitterThickness) * 0.25f)) return 29;
    if (!near(scene->rect.y, workspaceRect.y + options.tabBarHeight) || !near(scene->rect.height, workspaceRect.height - options.tabBarHeight)) return 30;
    if (!geometry.hit_test_splitter(workspaceRect, {splitter.bar.x - 4.0f, workspaceRect.y + 40.0f}, options)) return 31;
    const auto splitHit = geometry.hit_test_splitter(workspaceRect, {splitter.bar.x, workspaceRect.y + 40.0f}, options);
    if (!splitHit || splitHit->path != splitter.path || splitHit->node != &geometry.root()) return 32;
    if (!geometry.drag_splitter(splitter.path, 99.0f, workspaceRect, options)) return 33;
    if (!near(geometry.root().ratio, 0.35f)) return 34;
    if (!geometry.drag_splitter(splitter.path, {workspaceRect.x + 500.0f, workspaceRect.y + 40.0f}, workspaceRect, options)) return 35;
    if (!near(geometry.root().ratio, 500.0f / (workspaceRect.width - options.splitterThickness))) return 36;

    const auto currentGeometry = geometry.layout(workspaceRect, options);
    const auto* currentScene = find_panel_layout(currentGeometry, "scene");
    if (!currentScene) return 37;
    const auto tabHit = geometry.hit_test_tab(workspaceRect, {currentScene->rect.x + 20.0f, workspaceRect.y + 10.0f}, options);
    if (!tabHit || tabHit->panelId != "scene" || tabHit->stackPath != tabPath) return 37;
    if (!geometry.activate_tab(tabHit->stackPath, 0) || geometry.root().children[1].activeTab != 0) return 38;
    if (geometry.activate_tab(tabHit->stackPath, 20) || geometry.set_splitter_ratio(splitter.path, 2.0f)) return 39;
    if (!geometry.set_splitter_ratio(splitter.path, 0.0f) || geometry.root().ratio < 0.05f) return 40;

    if (!geometry.set_panel_visibility("scene", false) || !geometry.set_panel_visibility("console", false)) return 40;
    const auto hiddenSplitLayout = geometry.layout(workspaceRect, options);
    if (hiddenSplitLayout.splitters.size() != 0 || hiddenSplitLayout.panels.size() != 1 ||
        hiddenSplitLayout.panels.front().panelId != "outliner") return 40;
    if (!geometry.set_panel_visibility("scene", true) || !geometry.set_panel_visibility("console", true)) return 40;

    const DockRect narrowRect{0.0f, 0.0f, 200.0f, 100.0f};
    const auto narrow = geometry.layout(narrowRect, options);
    if (!narrow.minimumSizeExceeded || narrow.splitters.empty()) return 41;
    const auto* narrowOutliner = find_panel_layout(narrow, "outliner");
    const auto* narrowScene = find_panel_layout(narrow, "scene");
    if (!narrowOutliner || !narrowScene || narrowOutliner->rect.width < 0.0f || narrowScene->rect.width < 0.0f) return 42;
    geometry.rebalance({200.0f, 100.0f}, options.splitterThickness, options);
    if (geometry.root().ratio < 0.05f || geometry.root().ratio > 0.95f) return 44;

    DockWorkspace floatingGeometry(DockNode::leaf(panel("main")));
    if (!floatingGeometry.add_floating(DockNode::floating(
            DockNode::split(DockNode::leaf(panel("float-left")), DockNode::leaf(panel("float-right")),
                            DockSplitOrientation::Horizontal, 0.5f),
            {100.0f, 80.0f, 400.0f, 200.0f}))) return 45;
    const auto floatingResult = floatingGeometry.layout({0.0f, 0.0f, 800.0f, 600.0f}, options);
    if (floatingResult.splitters.size() != 1 || floatingResult.splitters.front().path.floatingIndex != 0) return 46;
    if (!floatingGeometry.hit_test_splitter({0.0f, 0.0f, 800.0f, 600.0f}, {300.0f, 120.0f}, options)) return 47;

    std::cout << "DockWorkspace tree, derived geometry, splitter drag, tab activation, constraints, hit testing, and JSON passed\n";
    return 0;
}

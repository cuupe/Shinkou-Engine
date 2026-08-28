#include "shinkou/editor/DockLayout.h"

#include <cmath>
#include <iostream>
#include <string>

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

} // namespace

int main() {
    DockWorkspace layout;
    if (!layout.add_tab("", panel("scene", 100.0f))) return 1;
    if (!layout.add_tab("scene", panel("game", 120.0f))) return 2;
    if (layout.root().kind != DockNode::Kind::TabStack || layout.root().activeTab != 1 || layout.root().tabs.size() != 2) return 3;
    if (!layout.activate_tab("scene") || layout.root().activeTab != 0) return 4;

    if (!layout.set_panel_visibility("game", false) || layout.root().tabs[1].visible) return 5;
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

    const std::string json = layout.to_json();
    if (json.find("\"version\":1") == std::string::npos || json.find("\"floating\"") == std::string::npos) return 17;
    DockWorkspace restored;
    const auto decoded = restored.from_json(json);
    if (!decoded || restored.to_json() != json || restored.floating().size() != 1) return 18;
    if (!has_panel(restored.root(), "scene") || !has_panel(restored.floating().front(), "console")) return 19;

    const std::string beforeBad = restored.to_json();
    if (restored.from_json("{\"version\":1,\"root\":{\"type\":\"split\"}}")) return 20;
    if (restored.to_json() != beforeBad) return 21;
    if (restored.from_json("{\"version\":999,\"root\":{}}")) return 22;
    if (restored.from_json("not json")) return 23;
    const auto unknownFields = restored.from_json("{\"version\":1,\"root\":{\"type\":\"leaf\",\"panelId\":\"a\",\"unknown\":true},\"unknown\":42}");
    if (!unknownFields) return 24;
    if (!has_panel(restored.root(), "a")) return 25;

    std::cout << "DockWorkspace tree, layout constraints, hit testing, and JSON passed\n";
    return 0;
}

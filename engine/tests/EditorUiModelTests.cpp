#include "shinkou/World.h"
#include "shinkou/editor/EditorUiModel.h"
#include <iostream>

namespace {
bool has_selected(const shinkou::editor::EditorObjectTreeNode& node, shinkou::ObjectId id) {
    if (node.id == id && node.selected) return true;
    for (const auto& child : node.children) if (has_selected(child, id)) return true;
    return false;
}
}

int main() {
    shinkou::World world;
    auto& player = world.create_object("Player");
    const auto& camera = player.create_child("Camera");

    shinkou::editor::EditorUiModel model;
    model.sync(world);
    if (model.menus().size() < 6 || model.pages().size() < 8 || model.object_roots().size() != 1) return 1;

    model.select_object(camera.id());
    model.sync(world);
    if (!has_selected(model.object_roots().front(), camera.id())) return 2;

    model.set_object_filter("camera");
    model.sync(world);
    if (model.object_roots().size() != 1 || model.object_roots().front().children.empty()) return 3;

    model.execute(shinkou::editor::EditorCommand::Play);
    if (!model.playing() || model.paused()) return 4;
    std::cout << "EditorUiModel object tree and command model passed\n";
    return 0;
}

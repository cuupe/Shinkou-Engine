#include "shinkou/World.h"
#include "shinkou/components/CameraComponent.h"
#include "shinkou/editor/EditorLayer.h"
#include <limits>
#include <unordered_set>

namespace shinkou::editor {
namespace {
std::string unique_copy_name(const World &world, const GameObject *parent, std::string name) {
    std::unordered_set<std::string> names;
    if (parent) {
        for (const auto &child : parent->children())
            if (child && !child->destroy_requested())
                names.emplace(child->name());
    } else
        world.each_object([&](const GameObject &object) { names.emplace(object.name()); });
    name += " Copy";
    const auto base = name;
    for (unsigned suffix = 2; names.count(name); ++suffix)
        name = base + " (" + std::to_string(suffix) + ")";
    return name;
}
void root_pose(EditorDocument &document, const math::Transform &pose) {
    for (auto &component : document.objects.front().components)
        if (component.type == "Transform")
            for (auto &property : component.properties) {
                if (property.name == "position")
                    property.value = property_text(pose.position);
                else if (property.name == "rotation")
                    property.value = property_text(pose.rotation);
                else if (property.name == "scale")
                    property.value = property_text(pose.scale);
            }
}
} // namespace
bool EditorLayer::copy_object(World &world) {
    auto *source = world.find_object(layout_.selectedObject);
    if (!source)
        return false;
    EditorDocument document;
    std::string error;
    if (!EditorDocument::capture_subtree(*source, document, error)) {
        lastStatus_ = error;
        return false;
    }
    math::Transform pose;
    if (!math::Decompose(source->get_component<components::TransformComponent>()->world_matrix(),
                         pose)) {
        lastStatus_ = "Cannot copy an invalid world transform";
        return false;
    }
    root_pose(document, pose);
    objectClipboard_ = std::move(document);
    lastStatus_ = "Object copied";
    return true;
}
bool EditorLayer::instantiate_object(World &world, bool paste) {
    EditorDocument document;
    std::string error;
    GameObject *parent = nullptr;
    if (paste) {
        if (!objectClipboard_)
            return false;
        document = *objectClipboard_;
    } else {
        auto *source = world.find_object(layout_.selectedObject);
        if (!source)
            return false;
        parent = source->parent();
        if (!EditorDocument::capture_subtree(*source, document, error)) {
            lastStatus_ = error;
            return false;
        }
    }
    if (document.objects.empty())
        return false;
    document.objects.front().name = unique_copy_name(world, parent, document.objects.front().name);
    if (!checkpoint(world))
        return false;
    ObjectId created{};
    if (!document.instantiate(world, parent ? parent->id() : 0, created, error)) {
        undo_.pop_back();
        undoHistory_.pop_back();
        lastStatus_ = error;
        return false;
    }
    layout_.selectedObject = created;
    uiModel_.select_object(created);
    world.refresh_transforms();
    document_changed();
    lastStatus_ = paste ? "Object pasted" : "Object duplicated";
    return true;
}
bool EditorLayer::create_scene_camera(World &world) {
    if (!checkpoint(world))
        return false;
    std::int64_t priority = 0;
    world.each_game_object([&](const GameObject &object) {
        if (const auto *camera = object.get_component<components::CameraComponent>())
            priority = std::max(priority, camera->priority);
    });
    auto &object = world.create_object("Camera");
    auto *camera = object.add_component<components::CameraComponent>();
    camera->priority =
        priority < std::numeric_limits<std::int64_t>::max() ? priority + 1 : priority;
    camera->fieldOfView = preferences_.camera.fieldOfView;
    camera->nearPlane = preferences_.camera.nearPlane;
    camera->farPlane = preferences_.camera.farPlane;
    ensure_viewport_camera(world);
    object.get_component<components::TransformComponent>()->set_local_transform(
        viewportController_.pose());
    layout_.selectedObject = object.id();
    uiModel_.select_object(object.id());
    world.refresh_transforms();
    document_changed();
    lastStatus_ = "Created scene camera";
    return true;
}
} // namespace shinkou::editor

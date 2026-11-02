#include "shinkou/World.h"
#include "shinkou/editor/EditorLayer.h"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace shinkou::editor {
namespace {
template <class Fn> void each_document_object(World &world, Fn &&fn) {
    const auto visit = [&](const auto &self, GameObject &object) -> void {
        if (object.destroy_requested())
            return;
        fn(object);
        for (const auto &child : object.children())
            if (child)
                self(self, *child);
    };
    world.each_object([&](GameObject &object) { visit(visit, object); });
}
struct Binding {
    std::function<PropertyValue()> get;
    std::function<bool(const PropertyValue &)> set;
};
Binding binding(World &world, ObjectId owner, std::string_view id,
                const std::shared_ptr<const std::vector<EditorInspectorChoice>> &audioChoices) {
    auto *object = world.find_object(owner);
    if (!object)
        return {};
    if (id == "name")
        return {[object] { return PropertyValue{std::string(object->name())}; },
                [object](const PropertyValue &v) {
                    const auto *name = std::get_if<std::string>(&v);
                    if (!name || name->empty())
                        return false;
                    object->set_name(*name);
                    return true;
                }};
    if (id == "active")
        return {[object] { return PropertyValue{object->active_self()}; },
                [object](const PropertyValue &v) {
                    const auto *active = std::get_if<bool>(&v);
                    if (!active)
                        return false;
                    object->set_active(*active);
                    return true;
                }};
    const bool enabled = id.substr(0, 8) == "enabled:";
    const auto colon = id.find(':');
    if (colon == std::string_view::npos)
        return {};
    const auto number = enabled ? id.substr(8) : id.substr(0, colon);
    ComponentId cid{};
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), cid);
    if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size())
        return {};
    auto *component = world.find_component(cid);
    if (!component || component->try_game_object() != object)
        return {};
    if (enabled)
        return {[component] { return PropertyValue{component->enabled()}; },
                [component](const PropertyValue &v) {
                    const auto *active = std::get_if<bool>(&v);
                    if (!active)
                        return false;
                    component->set_enabled(*active);
                    return true;
                }};
    if (id.substr(colon + 1) == "__local")
        if (auto *transform = dynamic_cast<components::TransformComponent *>(component))
            return {[transform] { return PropertyValue{transform->local}; },
                    [transform](const PropertyValue &value) {
                        const auto *pose = std::get_if<math::Transform>(&value);
                        return pose && transform->set_local_transform(*pose);
                    }};
    for (auto &property : component->properties()) {
        if (property.name != id.substr(colon + 1) || !property.editable())
            continue;
        Binding result{property.editorGet ? property.editorGet : property.get,
                       property.editorSet ? property.editorSet : property.set};
        if (auto *audio = dynamic_cast<components::AudioSourceComponent *>(component);
            audio && property.name == "clipPath") {
            result.set = [setter = result.set, audio, audioChoices](const PropertyValue &v) {
                const auto *path = std::get_if<std::string>(&v);
                if (!path || !setter(v))
                    return false;
                audio->assetId = 0;
                const auto normalized =
                    std::filesystem::u8path(*path).lexically_normal().generic_u8string();
                if (audioChoices)
                    for (const auto &choice : *audioChoices)
                        if (choice.value == normalized) {
                            audio->assetId = choice.assetId;
                            break;
                        }
                return true;
            };
        }
        return result;
    }
    return {};
}
bool finite(const PropertyValue &value) {
    return std::visit(
        [](const auto &v) -> bool {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, double>)
                return std::isfinite(v);
            else if constexpr (std::is_same_v<T, math::Vec2>)
                return math::IsFinite(v.x) && math::IsFinite(v.y);
            else if constexpr (std::is_same_v<T, math::Vec3>)
                return math::IsFinite(v.x) && math::IsFinite(v.y) && math::IsFinite(v.z);
            else if constexpr (std::is_same_v<T, math::Quat>)
                return math::IsFinite(v.x) && math::IsFinite(v.y) && math::IsFinite(v.z) &&
                       math::IsFinite(v.w);
            else if constexpr (std::is_same_v<T, math::Transform>) {
                const auto m = math::TransformMatrix(v);
                for (float n : m.m)
                    if (!math::IsFinite(n))
                        return false;
                return true;
            } else
                return !std::is_same_v<T, std::monostate>;
        },
        value);
}
bool equal_value(const PropertyValue &a, const PropertyValue &b) {
    if (a.index() != b.index())
        return false;
    return std::visit(
        [&](const auto &value) -> bool {
            using T = std::decay_t<decltype(value)>;
            const auto &other = std::get<T>(b);
            if constexpr (std::is_same_v<T, math::Vec2>)
                return value.x == other.x && value.y == other.y;
            else if constexpr (std::is_same_v<T, math::Vec3>)
                return value.x == other.x && value.y == other.y && value.z == other.z;
            else if constexpr (std::is_same_v<T, math::Quat>)
                return value.x == other.x && value.y == other.y && value.z == other.z &&
                       value.w == other.w;
            else if constexpr (std::is_same_v<T, math::Transform>)
                return equal_value(value.position, other.position) &&
                       equal_value(value.rotation, other.rotation) &&
                       equal_value(value.scale, other.scale);
            else
                return value == other;
        },
        a);
}
} // namespace
void EditorLayer::record_scene_checkpoint(EditorDocument document) {
    undo_.push_back({std::move(document), scenePath_, sceneDirty_, sceneNamed_});
    undoHistory_.push_back(EditHistoryKind::Scene);
    if (undo_.size() > 64) {
        undo_.erase(undo_.begin());
        const auto mark =
            std::find(undoHistory_.begin(), undoHistory_.end(), EditHistoryKind::Scene);
        if (mark != undoHistory_.end())
            undoHistory_.erase(mark);
    }
}
bool EditorLayer::begin_inspector_edit(std::string_view id) {
    if (inspectorEdit_ && inspectorEdit_->object == layout_.selectedObject &&
        inspectorEdit_->field == id)
        return true;
    finish_inspector_edit();
    if (!activeWorld_)
        return false;
    const auto target = binding(*activeWorld_, layout_.selectedObject, id, audioInspectorChoices_);
    if (!target.get || !target.set)
        return false;
    InspectorEdit edit;
    edit.object = layout_.selectedObject;
    edit.field = id;
    edit.original = target.get();
    edit.wasDirty = sceneDirty_;
    // The preorder address survives structural scene undo, which rebuilds IDs.
    // Component type + property name similarly avoids retaining stale pointers.
    std::size_t index = 0;
    each_document_object(*activeWorld_, [&](const GameObject &object) {
        if (object.id() == edit.object)
            edit.change.objectIndex = index;
        ++index;
    });
    edit.change.object = edit.object;
    edit.change.property = id;
    edit.change.before = edit.original;
    if (const auto colon = id.find(':'); colon != std::string_view::npos) {
        ComponentId cid{};
        const auto number = id.substr(0, 8) == "enabled:" ? id.substr(8) : id.substr(0, colon);
        std::from_chars(number.data(), number.data() + number.size(), cid);
        if (auto *component = activeWorld_->find_component(cid))
            edit.change.componentType = component->registered_type_name();
        edit.change.property =
            id.substr(0, 8) == "enabled:" ? "__enabled" : std::string(id.substr(colon + 1));
    }
    inspectorEdit_ = std::move(edit);
    return true;
}
bool EditorLayer::edit_inspector_value(std::string_view id, const PropertyValue &value) {
    if (!activeWorld_ || !finite(value))
        return false;
    if (inspectorEdit_ &&
        (inspectorEdit_->object != layout_.selectedObject || inspectorEdit_->field != id)) {
        finish_inspector_edit();
        return false;
    }
    const auto target = binding(*activeWorld_, layout_.selectedObject, id, audioInspectorChoices_);
    if (!target.get || !target.set || target.get().index() != value.index())
        return false;
    const bool single = !inspectorEdit_;
    if (single && !begin_inspector_edit(id))
        return false;
    if (!target.set(value)) {
        if (single)
            finish_inspector_edit();
        lastStatus_ = "Invalid property value or range";
        return false;
    }
    inspectorEdit_->change.after = target.get();
    inspectorEdit_->changed = !equal_value(inspectorEdit_->original, inspectorEdit_->change.after);
    document_changed(true);
    activeWorld_->refresh_transforms();
    if (single)
        finish_inspector_edit();
    lastStatus_ = "Property updated";
    return true;
}
void EditorLayer::finish_inspector_edit(bool cancel) {
    if (!inspectorEdit_)
        return;
    auto edit = std::move(*inspectorEdit_);
    inspectorEdit_.reset();
    if (!edit.changed) {
        sceneDirty_ = edit.wasDirty;
        return;
    }
    if (cancel && activeWorld_) {
        const auto target = binding(*activeWorld_, edit.object, edit.field, audioInspectorChoices_);
        if (target.set && target.set(edit.original)) {
            activeWorld_->refresh_transforms();
            sceneDirty_ = edit.wasDirty;
            uiModel_.invalidate_inspector();
            return;
        }
    }
    propertyUndo_.push_back(std::move(edit.change));
    undoHistory_.push_back(EditHistoryKind::Property);
    if (propertyUndo_.size() > 64) {
        propertyUndo_.erase(propertyUndo_.begin());
        const auto first =
            std::find(undoHistory_.begin(), undoHistory_.end(), EditHistoryKind::Property);
        if (first != undoHistory_.end())
            undoHistory_.erase(first);
    }
    document_changed();
}
bool EditorLayer::replay_property_edit(bool undo) {
    auto &source = undo ? propertyUndo_ : propertyRedo_;
    auto &destination = undo ? propertyRedo_ : propertyUndo_;
    if (!activeWorld_ || source.empty())
        return false;
    const auto &change = source.back();
    auto *owner = activeWorld_->find_object(change.object);
    if (!owner || owner->destroy_requested()) {
        lastStatus_ = "Property owner is unavailable";
        return false;
    }
    auto id = change.property;
    if (!change.componentType.empty()) {
        Component *found = nullptr;
        owner->each_component([&](Component &component) {
            if (component.registered_type_name() == change.componentType)
                found = &component;
        });
        if (!found) {
            lastStatus_ = "Property component is unavailable";
            return false;
        }
        id = change.property == "__enabled" ? "enabled:" + std::to_string(found->id())
                                            : std::to_string(found->id()) + ":" + change.property;
    }
    const auto target = binding(*activeWorld_, owner->id(), id, audioInspectorChoices_);
    if (!target.set || !target.set(undo ? change.before : change.after)) {
        lastStatus_ = "Property history could not be applied";
        return false;
    }
    layout_.selectedObject = owner->id();
    destination.push_back(std::move(source.back()));
    source.pop_back();
    activeWorld_->refresh_transforms();
    document_changed(true);
    return true;
}
void EditorLayer::rebind_property_history(World &world) {
    // Only an explicit scene restore may remap object identities. External
    // destruction must never redirect an old edit to a different object.
    std::vector<ObjectId> objects;
    each_document_object(world, [&](const GameObject &object) { objects.push_back(object.id()); });
    for (auto *history : {&propertyUndo_, &propertyRedo_})
        for (auto &change : *history)
            change.object = change.objectIndex < objects.size() ? objects[change.objectIndex] : 0;
}
} // namespace shinkou::editor

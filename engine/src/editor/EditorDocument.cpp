#include "shinkou/editor/EditorDocument.h"
#include "shinkou/reflection/Serialization.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_set>

namespace shinkou::editor {
std::string property_text(const PropertyValue& value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9);
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {}
        else if constexpr (std::is_same_v<T, bool>) out << (v ? "true" : "false");
        else if constexpr (std::is_same_v<T, math::Vec2>) out << v.x << ' ' << v.y;
        else if constexpr (std::is_same_v<T, math::Vec3>) out << v.x << ' ' << v.y << ' ' << v.z;
        else if constexpr (std::is_same_v<T, math::Quat>) out << v.x << ' ' << v.y << ' ' << v.z << ' ' << v.w;
        else if constexpr (std::is_same_v<T, math::Transform>) out << property_text(v.position) << ' ' << property_text(v.rotation) << ' ' << property_text(v.scale);
        else out << v;
    }, value);
    return out.str();
}

bool parse_property_text(PropertyType type, std::string_view text, PropertyValue& value) {
    if (type == PropertyType::String) { value = std::string(text); return true; }
    if (type == PropertyType::Bool) {
        if (text == "true" || text == "1") value = true;
        else if (text == "false" || text == "0") value = false;
        else return false;
        return true;
    }
    if (type == PropertyType::Integer || type == PropertyType::UnsignedInteger) {
        if (text.empty()) return false;
        if (type == PropertyType::Integer) {
            std::int64_t number{};
            const auto r = std::from_chars(text.data(), text.data() + text.size(), number);
            if (r.ec != std::errc{} || r.ptr != text.data() + text.size()) return false;
            value = number;
        } else {
            std::uint64_t number{};
            const auto r = std::from_chars(text.data(), text.data() + text.size(), number);
            if (r.ec != std::errc{} || r.ptr != text.data() + text.size()) return false;
            value = number;
        }
        return true;
    }
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    double numbers[10]{};
    const int count = type == PropertyType::Number ? 1 : type == PropertyType::Vec2 ? 2 :
        type == PropertyType::Vec3 ? 3 : type == PropertyType::Quaternion ? 4 : type == PropertyType::Transform ? 10 : 0;
    if (!count) return false;
    for (int i = 0; i < count; ++i)
        if (!(in >> numbers[i]) || !std::isfinite(numbers[i]) || std::abs(numbers[i]) > std::numeric_limits<float>::max()) return false;
    in >> std::ws;
    if (!in.eof()) return false;
    const auto n = [&](int i) { return static_cast<float>(numbers[i]); };
    switch (type) {
    case PropertyType::Number: value = numbers[0]; break;
    case PropertyType::Vec2: value = math::Vec2{n(0), n(1)}; break;
    case PropertyType::Vec3: value = math::Vec3{n(0), n(1), n(2)}; break;
    case PropertyType::Quaternion:
        if (n(0)*n(0)+n(1)*n(1)+n(2)*n(2)+n(3)*n(3) < 1e-12f) return false;
        value = math::Quat{n(0), n(1), n(2), n(3)}; break;
    case PropertyType::Transform: {
        math::Transform t;
        t.position = {n(0), n(1), n(2)}; t.rotation = {n(3), n(4), n(5), n(6)}; t.scale = {n(7), n(8), n(9)};
        value = t; break;
    }
    default: return false;
    }
    return true;
}

namespace {
void register_document(reflection::TypeRegistry& r) {
    using namespace reflection;
    r.register_type(TypeBuilder<DocumentProperty>("editor.DocumentProperty").field("name", &DocumentProperty::name).field("type", &DocumentProperty::type).field("value", &DocumentProperty::value).take());
    r.register_type(make_vector_descriptor<DocumentProperty>("editor.Properties"));
    r.register_type(make_vector_descriptor<std::string>("editor.Strings"));
    r.register_type(TypeBuilder<DocumentComponent>("editor.DocumentComponent").field("type", &DocumentComponent::type).field("enabled", &DocumentComponent::enabled).field("properties", &DocumentComponent::properties).field("tags", &DocumentComponent::tags).take());
    r.register_type(make_vector_descriptor<DocumentComponent>("editor.Components"));
    r.register_type(TypeBuilder<DocumentObject>("editor.DocumentObject").field("name", &DocumentObject::name).field("parent", &DocumentObject::parent).field("active", &DocumentObject::active).field("components", &DocumentObject::components).take());
    r.register_type(make_vector_descriptor<DocumentObject>("editor.Objects"));
    r.register_type(TypeBuilder<EditorDocument>("editor.Document").field("version", &EditorDocument::version).field("selected", &EditorDocument::selected).field("objects", &EditorDocument::objects).take());
}
}

bool EditorDocument::capture(World& world, ObjectId selection, EditorDocument& document, std::string& error) {
    EditorDocument next;
    bool valid = true;
    std::function<void(GameObject&, std::int32_t)> visit = [&](GameObject& object, std::int32_t parent) {
        if (!valid || object.destroy_requested()) return;
        // Resource handles and arbitrary ECS stores need their own asset codec.
        // Refuse lossy serialization instead of silently dropping them.
        if (object.has_ecs_entity()) { error = "Scene contains an ECS-owned object without a document codec"; valid = false; return; }
        const auto index = static_cast<std::int32_t>(next.objects.size());
        if (object.id() == selection) next.selected = index;
        DocumentObject entry; entry.name = object.name(); entry.parent = parent; entry.active = object.active_self();
        object.each_component([&](Component& component) {
            DocumentComponent c; c.type = component.registered_type_name(); c.enabled = component.enabled();
            for (auto& p : component.properties()) {
                if (p.get && has_flag(p.flags, PropertyFlags::Serialized) && !has_flag(p.flags, PropertyFlags::RuntimeOnly))
                    c.properties.push_back({p.name, static_cast<std::uint32_t>(p.type), property_text(p.get())});
            }
            if (const auto* tags = dynamic_cast<const components::TagComponent*>(&component)) c.tags = tags->tags();
            entry.components.push_back(std::move(c));
        });
        next.objects.push_back(std::move(entry));
        for (const auto& child : object.children()) if (child) visit(*child, index);
    };
    world.each_object([&](GameObject& object) { visit(object, -1); });
    if (!valid) return false;
    document = std::move(next); return true;
}

bool EditorDocument::restore(World& world, ObjectId& selection, std::string& error) const {
    if (version != 1 || objects.size() > 100000 || selected < -1 || selected >= static_cast<std::int32_t>(objects.size())) { error = "Invalid scene version, selection or object count"; return false; }
    const auto types = world.component_types();
    // Construct and validate a replacement alongside the current scene. Existing
    // scene objects are removed only once every component and setter succeeds.
    std::vector<GameObject*> created;
    std::vector<ObjectId> roots;
    std::vector<ObjectId> oldRoots;
    world.each_object([&](const GameObject& object) { oldRoots.push_back(object.id()); });
    auto rollback = [&] { for (auto id : roots) if (auto* o = world.find_object(id)) o->destroy(); };
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const auto& entry = objects[i];
        if (entry.parent < -1 || entry.parent >= static_cast<std::int32_t>(i)) { error = "Invalid scene parent order"; rollback(); return false; }
        auto& object = entry.parent < 0 ? world.create_object(entry.name) : created[entry.parent]->create_child(entry.name);
        if (entry.parent < 0) roots.push_back(object.id());
        created.push_back(&object);
        std::unordered_set<std::string> seen;
        for (const auto& c : entry.components) {
            if (!seen.insert(c.type).second || std::find(types.begin(), types.end(), c.type) == types.end()) { error = "Unknown or duplicate component: " + c.type; rollback(); return false; }
            Component* component = c.type == "Transform" ? object.get_component<components::TransformComponent>() : object.add_component(c.type);
            if (!component) { error = "Cannot create component: " + c.type; rollback(); return false; }
            for (const auto& p : c.properties) {
                PropertyValue value;
                if (!parse_property_text(static_cast<PropertyType>(p.type), p.value, value) || !component->set_property(p.name, value)) { error = "Invalid property: " + c.type + "." + p.name; rollback(); return false; }
            }
            if (auto* tags = dynamic_cast<components::TagComponent*>(component)) for (const auto& tag : c.tags) tags->add(tag);
            component->set_enabled(c.enabled);
        }
        object.set_active(entry.active);
    }
    for (auto id : oldRoots) if (auto* object = world.find_object(id)) object->destroy();
    selection = selected < 0 ? 0 : created[static_cast<std::size_t>(selected)]->id();
    return true;
}

bool EditorDocument::to_json(std::string& json, std::string& error) const {
    reflection::TypeRegistry r; register_document(r);
    const auto result = reflection::serialize_json(r, reflection::type_id<EditorDocument>(), this, json, {true});
    error = result.message; return static_cast<bool>(result);
}
bool EditorDocument::from_json(std::string_view json, EditorDocument& document, std::string& error) {
    if (json.size() > 16 * 1024 * 1024) { error = "Scene document exceeds 16 MiB"; return false; }
    reflection::TypeRegistry r; register_document(r);
    EditorDocument next;
    const auto result = reflection::deserialize_json(r, reflection::type_id<EditorDocument>(), json, &next);
    error = result.message;
    if (!result) return false;
    if (next.version != 1) { error = "Unsupported scene version"; return false; }
    document = std::move(next); return true;
}
} // namespace shinkou::editor

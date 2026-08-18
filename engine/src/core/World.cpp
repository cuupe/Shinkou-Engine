#include "shinkou/World.h"
#include <algorithm>

namespace shinkou {
GameObject::GameObject(World& world, ObjectId id, std::string name)
    : world_(&world), id_(id), name_(std::move(name)) {
    add_component<components::TransformComponent>();
}

GameObject* GameObjectHandle::get() const noexcept { return valid() ? world->find_object(id) : nullptr; }
bool GameObjectHandle::valid() const noexcept { return world != nullptr && world->find_object(id) != nullptr; }
Component* ComponentHandle::get() const noexcept { return valid() ? world->find_component(id) : nullptr; }
bool ComponentHandle::valid() const noexcept { return world != nullptr && world->find_component(id) != nullptr; }

World::World() {
    register_component_type<components::TransformComponent>("Transform");
    register_component_type<components::TagComponent>("Tag");
    register_component_type<components::LifetimeComponent>("Lifetime");
}

GameObject& Component::game_object() noexcept { return *owner_; }
const GameObject& Component::game_object() const noexcept { return *owner_; }

void Component::set_enabled(bool enabled) noexcept {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!owner_ || !owner_->active_in_hierarchy()) return;
    if (enabled_) on_enable();
    else on_disable();
}

ComponentHandle Component::handle() noexcept { return {owner_ ? &owner_->world() : nullptr, id_}; }

std::vector<PropertyDescriptor> Component::properties() {
    PropertyBuilder builder;
    define_properties(builder);
    return builder.take();
}

bool Component::set_property(std::string_view name, const PropertyValue& value) {
    for (auto& property : properties()) {
        if (property.name == name) return property.editable() && property.set(value);
    }
    return false;
}

void Component::notify_created() noexcept {
    if (state_ != LifecycleState::Constructing) return;
    state_ = LifecycleState::Alive;
    on_create();
}

void Component::notify_destroy_requested() noexcept {
    if (state_ == LifecycleState::DestroyRequested || state_ == LifecycleState::Destroyed) return;
    state_ = LifecycleState::DestroyRequested;
    on_destroy_requested();
}

void Component::notify_destroyed() noexcept {
    if (state_ == LifecycleState::Destroyed) return;
    on_destroy();
    state_ = LifecycleState::Destroyed;
}

void Component::notify_active_state(bool active) noexcept {
    if (state_ == LifecycleState::Destroyed || state_ == LifecycleState::DestroyRequested) return;
    state_ = active ? LifecycleState::Alive : LifecycleState::Inactive;
    if (active) {
        if (enabled_) on_enable();
    } else if (enabled_) {
        on_disable();
    }
}

void components::TransformComponent::update_world(const math::Mat4& parentWorld) noexcept {
    if (dirty_) {
        worldMatrix_ = math::Multiply(parentWorld, math::TransformMatrix(local));
        dirty_ = false;
    }
}

bool components::TagComponent::has(std::string_view tag) const noexcept {
    return std::find(tags_.begin(), tags_.end(), tag) != tags_.end();
}

void components::TagComponent::add(std::string tag) {
    if (!has(tag)) tags_.push_back(std::move(tag));
}

bool components::TagComponent::remove(std::string_view tag) noexcept {
    const auto it = std::find(tags_.begin(), tags_.end(), tag);
    if (it == tags_.end()) return false;
    tags_.erase(it);
    return true;
}

void components::LifetimeComponent::on_update(Seconds dt) {
    if (remaining < 0.0f) return;
    remaining -= std::max(dt, 0.0f);
    if (destroyWhenExpired && remaining <= 0.0f) game_object().destroy();
}

void GameObject::refresh_active_state(bool parentActive) noexcept {
    const bool next = parentActive && activeSelf_ && !destroyRequested_;
    if (next != activeInHierarchy_) {
        activeInHierarchy_ = next;
        for (auto& component : components_) if (component) component->notify_active_state(next);
    }
    for (auto& child : children_) child->refresh_active_state(activeInHierarchy_);
}

void GameObject::set_active(bool active) noexcept {
    if (activeSelf_ == active) return;
    activeSelf_ = active;
    refresh_active_state(parent_ ? parent_->activeInHierarchy_ : true);
}

void GameObject::update_world_transform() noexcept {
    const auto* parentTransform = parent_ ? parent_->get_component<components::TransformComponent>() : nullptr;
    const auto parentWorld = parentTransform ? parentTransform->world_matrix() : math::Mat4::Identity();
    if (auto* transform = get_component<components::TransformComponent>()) transform->update_world(parentWorld);
}

void GameObject::update_recursive(Seconds dt) {
    if (!activeInHierarchy_ || destroyRequested_) return;
    update_world_transform();
    for (auto& component : components_) {
        if (component && component->enabled_ && component->state_ != LifecycleState::DestroyRequested)
            component->on_update(dt);
    }
    std::vector<GameObject*> children;
    children.reserve(children_.size());
    for (auto& child : children_) children.push_back(child.get());
    for (auto* child : children) {
        if (child) child->update_recursive(dt);
    }
}

void GameObject::request_destroy_recursive() noexcept {
    if (destroyRequested_) return;
    destroyRequested_ = true;
    state_ = LifecycleState::DestroyRequested;
    if (lifecycleCallbacks_.destroyRequested) lifecycleCallbacks_.destroyRequested(*this);
    for (auto& component : components_) if (component) component->notify_destroy_requested();
    for (auto& child : children_) child->request_destroy_recursive();
    refresh_active_state(parent_ ? parent_->activeInHierarchy_ : true);
}

void GameObject::destroy() noexcept { world_->destroy_object(*this); }

GameObject& GameObject::create_child(std::string name) { return world_->create_object(std::move(name), this); }

void GameObject::set_parent(GameObject* parent) {
    if (parent == this || parent_ == parent || (parent && parent->world_ != world_)) return;
    for (auto* ancestor = parent; ancestor; ancestor = ancestor->parent_) {
        if (ancestor == this) return;
    }
    world_->reparent_object(*this, parent);
}

void GameObject::dispose() noexcept {
    if (activeInHierarchy_) {
        activeInHierarchy_ = false;
        for (auto& component : components_) if (component) component->notify_active_state(false);
    }
    for (auto& component : components_) {
        if (!component) continue;
        component->on_detach();
        component->notify_destroyed();
        world_->unregister_component(*component);
        component->owner_ = nullptr;
    }
    state_ = LifecycleState::Destroyed;
    if (lifecycleCallbacks_.destroyed) lifecycleCallbacks_.destroyed(*this);
}

void GameObject::initialize() noexcept {
    state_ = activeInHierarchy_ ? LifecycleState::Alive : LifecycleState::Inactive;
    if (lifecycleCallbacks_.created) lifecycleCallbacks_.created(*this);
}

void GameObject::attach_component(Component& component, std::string_view registeredTypeName) {
    component.owner_ = this;
    component.id_ = world_->nextComponentId_++;
    component.registeredName_ = registeredTypeName.empty() ? std::string(component.type_name()) : std::string(registeredTypeName);
    world_->register_component(component, component.registeredName_);
    component.on_attach();
    component.notify_created();
    if (activeInHierarchy_ && component.enabled_) component.notify_active_state(true);
}

void GameObject::remove_component(Component& component) noexcept {
    const auto it = std::find_if(components_.begin(), components_.end(), [&](const auto& item) { return item.get() == &component; });
    if (it == components_.end()) return;
    if (activeInHierarchy_ && component.enabled_) component.notify_active_state(false);
    component.on_detach();
    component.notify_destroyed();
    world_->unregister_component(component);
    component.owner_ = nullptr;
    component.state_ = LifecycleState::Destroyed;
    componentLookup_.erase(std::type_index(typeid(component)));
    components_.erase(it);
}

Component* GameObject::add_component(std::string_view registeredTypeName) {
    const auto it = world_->componentTypes_.find(std::string(registeredTypeName));
    if (it == world_->componentTypes_.end() || !it->second.create) return nullptr;
    auto component = it->second.create();
    if (!component) return nullptr;
    const auto key = std::type_index(typeid(*component));
    if (componentLookup_.find(key) != componentLookup_.end()) return nullptr;
    auto* result = component.get();
    components_.push_back(std::move(component));
    componentLookup_[key] = result;
    attach_component(*result, registeredTypeName);
    return result;
}

Component* GameObject::component_at(std::size_t index) noexcept {
    return index < components_.size() ? components_[index].get() : nullptr;
}
const Component* GameObject::component_at(std::size_t index) const noexcept {
    return index < components_.size() ? components_[index].get() : nullptr;
}
std::size_t GameObject::component_count() const noexcept { return components_.size(); }

std::vector<PropertyDescriptor> GameObject::properties() {
    PropertyBuilder builder;
    PropertyDescriptor name;
    name.name = "name";
    name.displayName = "Name";
    name.type = PropertyType::String;
    name.flags = PropertyFlags::Serialized;
    name.get = [this] { return PropertyValue{name_}; };
    name.set = [this](const PropertyValue& value) {
        const auto* text = std::get_if<std::string>(&value);
        if (!text) return false;
        name_ = *text;
        return true;
    };
    builder.add_read_only("id", PropertyType::UnsignedInteger,
        [this] { return PropertyValue{static_cast<std::uint64_t>(id_)}; });
    builder.add_read_only("activeInHierarchy", PropertyType::Bool,
        [this] { return PropertyValue{activeInHierarchy_}; });
    auto properties = builder.take();
    properties.insert(properties.begin(), std::move(name));
    return properties;
}

bool GameObject::set_property(std::string_view name, const PropertyValue& value) {
    for (auto& property : properties()) {
        if (property.name == name) return property.editable() && property.set(value);
    }
    return false;
}

Entity GameObject::enable_ecs() { return world_->enable_ecs(*this); }
void GameObject::disable_ecs() noexcept { world_->disable_ecs(*this); }

World::~World() { clear_objects(); }

void World::register_object(GameObject& object) noexcept { objectLookup_[object.id_] = &object; }

void World::unregister_object(GameObject& object) noexcept { objectLookup_.erase(object.id_); }

void World::register_component(Component& component, std::string_view) noexcept {
    componentLookup_[component.id_] = &component;
}

void World::unregister_component(Component& component) noexcept {
    componentLookup_.erase(component.id_);
}

void World::release_tree(GameObject& object) noexcept {
    for (auto& child : object.children_) release_tree(*child);
    object.dispose();
    disable_ecs(object);
    unregister_object(object);
}

GameObject& World::create_object(std::string name) { return create_object(std::move(name), nullptr); }

GameObject& World::create_object(std::string name, GameObject* parent) {
    auto object = std::unique_ptr<GameObject>(new GameObject(*this, nextObjectId_++, std::move(name)));
    auto& result = *object;
    result.parent_ = parent;
    if (parent) parent->children_.push_back(std::move(object));
    else gameObjects_.push_back(std::move(object));
    register_object(result);
    result.refresh_active_state(parent ? parent->activeInHierarchy_ : true);
    result.initialize();
    return result;
}

void World::reparent_object(GameObject& object, GameObject* parent) {
    std::unique_ptr<GameObject> ownership;
    if (object.parent_) {
        auto& siblings = object.parent_->children_;
        const auto it = std::find_if(siblings.begin(), siblings.end(), [&](const auto& item) {
            return item.get() == &object;
        });
        if (it == siblings.end()) return;
        ownership = std::move(*it);
        siblings.erase(it);
    } else {
        const auto it = std::find_if(gameObjects_.begin(), gameObjects_.end(), [&](const auto& item) {
            return item.get() == &object;
        });
        if (it == gameObjects_.end()) return;
        ownership = std::move(*it);
        gameObjects_.erase(it);
    }
    object.parent_ = parent;
    if (parent) parent->children_.push_back(std::move(ownership));
    else gameObjects_.push_back(std::move(ownership));
    object.refresh_active_state(parent ? parent->activeInHierarchy_ : true);
}

GameObject* World::find_object(ObjectId id) noexcept {
    const auto it = objectLookup_.find(id);
    return it == objectLookup_.end() ? nullptr : it->second;
}

const GameObject* World::find_object(ObjectId id) const noexcept {
    const auto it = objectLookup_.find(id);
    return it == objectLookup_.end() ? nullptr : it->second;
}

GameObject* World::find_object(Entity entity) noexcept {
    if (!registry_.valid(entity)) return nullptr;
    const auto it = entityLookup_.find(entity);
    return it == entityLookup_.end() ? nullptr : it->second;
}

const GameObject* World::find_object(Entity entity) const noexcept {
    if (!registry_.valid(entity)) return nullptr;
    const auto it = entityLookup_.find(entity);
    return it == entityLookup_.end() ? nullptr : it->second;
}

Component* World::find_component(ComponentId id) noexcept {
    const auto it = componentLookup_.find(id);
    return it == componentLookup_.end() ? nullptr : it->second;
}

const Component* World::find_component(ComponentId id) const noexcept {
    const auto it = componentLookup_.find(id);
    return it == componentLookup_.end() ? nullptr : it->second;
}

std::vector<std::string> World::component_types() const {
    std::vector<std::string> result;
    result.reserve(componentTypes_.size());
    for (const auto& [name, descriptor] : componentTypes_) {
        (void)descriptor;
        result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    return result;
}

Entity World::enable_ecs(GameObject& object) {
    if (object.world_ != this || object.destroyRequested_) return {};
    if (!object.ecsEntity_) {
        object.ecsEntity_ = registry_.create();
        entityLookup_[object.ecsEntity_] = &object;
    }
    return object.ecsEntity_;
}

void World::disable_ecs(GameObject& object) noexcept {
    if (object.world_ != this || !object.ecsEntity_) return;
    registry_.destroy(object.ecsEntity_);
    entityLookup_.erase(object.ecsEntity_);
    object.ecsEntity_ = {};
}

void World::destroy_object(GameObject& object) noexcept {
    if (object.world_ != this) return;
    object.request_destroy_recursive();
    if (!updating_) collect_destroyed();
}

void World::remove_destroyed(GameObject& object) noexcept {
    for (auto it = object.children_.begin(); it != object.children_.end();) {
        auto& child = **it;
        if (child.destroyRequested_) {
            release_tree(child);
            it = object.children_.erase(it);
        } else {
            remove_destroyed(child);
            ++it;
        }
    }
}

void World::clear_objects() noexcept {
    for (auto it = gameObjects_.begin(); it != gameObjects_.end();) {
        auto& object = **it;
        object.request_destroy_recursive();
        release_tree(object);
        it = gameObjects_.erase(it);
    }
    registry_.clear();
    entityLookup_.clear();
}

void World::collect_destroyed() noexcept {
    for (auto it = gameObjects_.begin(); it != gameObjects_.end();) {
        auto& object = **it;
        if (object.destroyRequested_) {
            release_tree(object);
            it = gameObjects_.erase(it);
        } else {
            remove_destroyed(object);
            ++it;
        }
    }
}

void World::add_system(std::function<void(World&, Seconds)> system) {
    if (system) systems_.push_back(std::move(system));
}

void World::update(Seconds dt) {
    updating_ = true;
    for (auto& object : legacyObjects_) {
        if (object) object->update(dt);
    }
    std::vector<GameObject*> objects;
    objects.reserve(gameObjects_.size());
    for (auto& object : gameObjects_) objects.push_back(object.get());
    for (auto* object : objects) if (object) object->update_recursive(dt);
    for (auto& system : systems_) {
        if (system) system(*this, dt);
    }
    updating_ = false;
    collect_destroyed();
}
}

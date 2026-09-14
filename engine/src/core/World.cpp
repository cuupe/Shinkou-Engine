#include "shinkou/World.h"
#include <algorithm>
#include <cmath>

namespace shinkou {
GameObject::GameObject(World& world, ObjectId id, std::string name, ObjectStorage storage)
    : world_(&world), id_(id), name_(std::move(name)), storage_(storage) {
    if (storage_ == ObjectStorage::Ecs) world_->enable_ecs(*this);
    add_component<components::TransformComponent>();
}

GameObject* GameObjectHandle::get() const noexcept { return valid() ? world->find_object(id) : nullptr; }
bool GameObjectHandle::valid() const noexcept { return world != nullptr && world->find_object(id) != nullptr; }
Component* ComponentHandle::get() const noexcept { return valid() ? world->find_component(id) : nullptr; }
bool ComponentHandle::valid() const noexcept { return world != nullptr && world->find_component(id) != nullptr; }

World::World() {
    registry_.set_destroy_observer(this, [](void* context, Entity entity) noexcept {
        static_cast<World*>(context)->on_ecs_entity_destroyed(entity);
    });
    register_component_type<components::TransformComponent>("Transform");
    register_component_type<components::TagComponent>("Tag");
    register_component_type<components::LifetimeComponent>("Lifetime");
}

void EcsSystem::attach(World& world) noexcept {
    world_ = &world;
    state_ = enabled_ ? LifecycleState::Alive : LifecycleState::Inactive;
    on_create();
    if (enabled_) on_enable();
}

void EcsSystem::update(Seconds dt) {
    if (enabled_ && state_ == LifecycleState::Alive) on_update(dt);
}

void EcsSystem::dispose() noexcept {
    if (state_ == LifecycleState::Destroyed) return;
    if (enabled_) {
        enabled_ = false;
        on_disable();
    }
    on_destroy();
    state_ = LifecycleState::Destroyed;
    world_ = nullptr;
}

void EcsSystem::set_enabled(bool enabled) noexcept {
    if (enabled_ == enabled || state_ == LifecycleState::Destroyed) return;
    enabled_ = enabled;
    if (!world_) return;
    state_ = enabled ? LifecycleState::Alive : LifecycleState::Inactive;
    if (enabled_) on_enable();
    else on_disable();
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
        for (auto* component : ecsComponents_) if (component) component->notify_active_state(next);
    }
    for (auto& child : children_) child->refresh_active_state(activeInHierarchy_);
}

void GameObject::set_active(bool active) noexcept {
    if (activeSelf_ == active) return;
    activeSelf_ = active;
    refresh_active_state(parent_ ? parent_->activeInHierarchy_ : true);
}

void GameObject::update_world_transform() noexcept {
    const auto* parentTransform = parent_ ? parent_->transform_ : nullptr;
    const auto parentWorld = parentTransform ? parentTransform->world_matrix() : math::Mat4::Identity();
    if (transform_) transform_->update_world(parentWorld);
}

void GameObject::update_transform_recursive() noexcept {
    if (!activeInHierarchy_ || destroyRequested_) return;
    update_world_transform();
    const auto childCount = children_.size();
    for (std::size_t index = 0; index < childCount && index < children_.size(); ++index)
        if (auto* child = children_[index].get()) child->update_transform_recursive();
}

void GameObject::update_regular_components(Seconds dt) {
    if (!activeInHierarchy_ || destroyRequested_) return;
    // Components may request removal or add another component while running.
    // Index-based traversal remains valid under deferred destruction and does
    // not allocate a snapshot on the frame hot path.
    for (std::size_t index = 0; index < components_.size();) {
        auto* component = components_[index].get();
        if (component && component->enabled_ && component->state_ != LifecycleState::DestroyRequested)
            component->on_update(dt);
        if (index < components_.size() && components_[index].get() == component) ++index;
    }
}

void GameObject::update_ecs_components(Seconds dt) {
    if (!activeInHierarchy_ || destroyRequested_) return;
    // This is the object-compatible ECS path.  High-volume data-only ECS
    // components bypass this loop and should be processed by EcsSystem views.
    // A regular object promoted to ECS can temporarily retain legacy
    // components; update them here so storage changes do not change behavior.
    for (std::size_t index = 0; index < components_.size();) {
        auto* component = components_[index].get();
        if (component && component->enabled_ && component->state_ != LifecycleState::DestroyRequested)
            component->on_update(dt);
        if (index < components_.size() && components_[index].get() == component) ++index;
    }
    for (std::size_t index = 0; index < ecsComponents_.size();) {
        auto* component = ecsComponents_[index];
        if (component && component->enabled_ && component->state_ != LifecycleState::DestroyRequested)
            component->on_update(dt);
        if (index < ecsComponents_.size() && ecsComponents_[index] == component) ++index;
    }
}

void GameObject::request_destroy_recursive() noexcept {
    if (destroyRequested_) return;
    destroyRequested_ = true;
    state_ = LifecycleState::DestroyRequested;
    if (lifecycleCallbacks_.destroyRequested) lifecycleCallbacks_.destroyRequested(*this);
    for (auto& component : components_) if (component) component->notify_destroy_requested();
    for (auto* component : ecsComponents_) if (component) component->notify_destroy_requested();
    for (auto& child : children_) child->request_destroy_recursive();
    refresh_active_state(parent_ ? parent_->activeInHierarchy_ : true);
}

void GameObject::destroy() noexcept { world_->destroy_object(*this); }

GameObject& GameObject::create_child(std::string name, ObjectStorage storage) {
    return world_->create_object(std::move(name), this, storage);
}

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
        for (auto* component : ecsComponents_) if (component) component->notify_active_state(false);
    }
    for (auto& component : components_) {
        if (!component) continue;
        component->on_detach();
        component->notify_destroyed();
        world_->unregister_component(*component);
        component->owner_ = nullptr;
        if (transform_ == component.get()) transform_ = nullptr;
    }
    components_.clear();
    dispose_ecs_components();
    state_ = LifecycleState::Destroyed;
    if (lifecycleCallbacks_.destroyed) lifecycleCallbacks_.destroyed(*this);
}

void GameObject::initialize() noexcept {
    state_ = activeInHierarchy_ ? LifecycleState::Alive : LifecycleState::Inactive;
    if (lifecycleCallbacks_.created) lifecycleCallbacks_.created(*this);
}

void GameObject::attach_component(Component& component, std::string_view registeredTypeName,
                                  ObjectStorage storage,
                                  void (*removeFromEcs)(World&, Entity) noexcept) {
    component.owner_ = this;
    component.id_ = world_->nextComponentId_++;
    component.registeredName_ = registeredTypeName.empty() ? std::string(component.type_name()) : std::string(registeredTypeName);
    component.storage_ = storage;
    component.removeFromEcs_ = removeFromEcs;
    if (typeid(component) == typeid(components::TransformComponent))
        transform_ = static_cast<components::TransformComponent*>(&component);
    world_->register_component(component, component.registeredName_);
    component.on_attach();
    component.notify_created();
    if (activeInHierarchy_ && component.enabled_) component.notify_active_state(true);
}

void GameObject::remove_component(Component& component) noexcept {
    if (component.storage_ == ObjectStorage::Ecs) {
        const auto it = std::find(ecsComponents_.begin(), ecsComponents_.end(), &component);
        if (it == ecsComponents_.end()) return;
        const auto key = std::type_index(typeid(component));
        if (activeInHierarchy_ && component.enabled_) component.notify_active_state(false);
        component.on_detach();
        component.notify_destroyed();
        world_->unregister_component(component);
        const auto removeFromEcs = component.removeFromEcs_;
        const auto entity = ecsEntity_;
        component.owner_ = nullptr;
        componentLookup_.erase(key);
        ecsComponents_.erase(it);
        if (transform_ == &component) transform_ = nullptr;
        if (removeFromEcs && entity) removeFromEcs(*world_, entity);
        return;
    }
    const auto it = std::find_if(components_.begin(), components_.end(), [&](const auto& item) { return item.get() == &component; });
    if (it == components_.end()) return;
    if (activeInHierarchy_ && component.enabled_) component.notify_active_state(false);
    component.on_detach();
    component.notify_destroyed();
    world_->unregister_component(component);
    component.owner_ = nullptr;
    component.state_ = LifecycleState::Destroyed;
    componentLookup_.erase(std::type_index(typeid(component)));
    if (transform_ == &component) transform_ = nullptr;
    components_.erase(it);
}

void GameObject::dispose_ecs_components(bool removeFromRegistry) noexcept {
    while (!ecsComponents_.empty()) {
        Component* component = ecsComponents_.back();
        ecsComponents_.pop_back();
        if (!component) continue;
        if (activeInHierarchy_ && component->enabled_) component->notify_active_state(false);
        component->on_detach();
        component->notify_destroyed();
        world_->unregister_component(*component);
        const auto removeFromEcs = component->removeFromEcs_;
        const auto entity = ecsEntity_;
        componentLookup_.erase(std::type_index(typeid(*component)));
        component->owner_ = nullptr;
        if (transform_ == component) transform_ = nullptr;
        if (removeFromRegistry && removeFromEcs && entity) removeFromEcs(*world_, entity);
    }
}

bool GameObject::set_storage(ObjectStorage storage) {
    return world_ && (storage_ == storage || world_->set_object_storage(*this, storage));
}

Component* GameObject::add_component(std::string_view registeredTypeName) {
    const auto it = world_->componentTypes_.find(std::string(registeredTypeName));
    if (it == world_->componentTypes_.end() || !it->second.create) return nullptr;
    if (it->second.createInObject) return it->second.createInObject(*this);
    auto component = it->second.create();
    if (!component) return nullptr;
    const auto key = std::type_index(typeid(*component));
    if (componentLookup_.find(key) != componentLookup_.end()) return nullptr;
    auto* result = component.get();
    components_.push_back(std::move(component));
    componentLookup_[key] = result;
    attach_component(*result, registeredTypeName, ObjectStorage::Regular);
    return result;
}

Component* GameObject::component_at(std::size_t index) noexcept {
    if (index < components_.size()) return components_[index].get();
    index -= components_.size();
    return index < ecsComponents_.size() ? ecsComponents_[index] : nullptr;
}
const Component* GameObject::component_at(std::size_t index) const noexcept {
    if (index < components_.size()) return components_[index].get();
    index -= components_.size();
    return index < ecsComponents_.size() ? ecsComponents_[index] : nullptr;
}
std::size_t GameObject::component_count() const noexcept {
    return components_.size() + ecsComponents_.size();
}

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

World::~World() {
    clear_ecs_systems();
    clear_objects();
}

void World::register_object(GameObject& object) noexcept {
    objectLookup_[object.id_] = &object;
    object.objectOrderIndex_ = objectOrder_.size();
    objectOrder_.push_back(&object);
    register_storage_object(object);
}

void World::unregister_object(GameObject& object) noexcept {
    objectLookup_.erase(object.id_);
    unregister_storage_object(object, object.storage_);
    const auto index = object.objectOrderIndex_;
    if (index >= objectOrder_.size() || objectOrder_[index] != &object) return;
    GameObject* moved = objectOrder_.back();
    objectOrder_[index] = moved;
    objectOrder_.pop_back();
    if (moved && moved != &object) moved->objectOrderIndex_ = index;
}

void World::register_storage_object(GameObject& object) noexcept {
    auto& order = object.storage_ == ObjectStorage::Ecs ? ecsObjectOrder_ : regularObjectOrder_;
    object.storageOrderIndex_ = order.size();
    order.push_back(&object);
}

void World::unregister_storage_object(GameObject& object, ObjectStorage storage) noexcept {
    auto& order = storage == ObjectStorage::Ecs ? ecsObjectOrder_ : regularObjectOrder_;
    const auto index = object.storageOrderIndex_;
    if (index >= order.size() || order[index] != &object) return;
    GameObject* moved = order.back();
    order[index] = moved;
    order.pop_back();
    if (moved && moved != &object) moved->storageOrderIndex_ = index;
}

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

GameObject& World::create_object(std::string name) {
    return create_object(std::move(name), nullptr, ObjectStorage::Regular);
}

GameObject& World::create_object(std::string name, ObjectStorage storage) {
    return create_object(std::move(name), nullptr, storage);
}

GameObject& World::create_object(std::string name, GameObject* parent, ObjectStorage storage) {
    auto object = std::unique_ptr<GameObject>(new GameObject(*this, nextObjectId_++, std::move(name), storage));
    auto& result = *object;
    result.parent_ = parent;
    if (parent) {
        result.objectOrderIndex_ = parent->children_.size();
        parent->children_.push_back(std::move(object));
    } else {
        result.objectOrderIndex_ = gameObjects_.size();
        gameObjects_.push_back(std::move(object));
    }
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

bool World::set_object_storage(GameObject& object, ObjectStorage storage) {
    if (object.world_ != this) return false;
    if (object.storage_ == storage) return true;
    if (storage == ObjectStorage::Ecs) return static_cast<bool>(enable_ecs(object));
    if (!object.ecsComponents_.empty()) return false;
    disable_ecs(object);
    return true;
}

std::size_t World::regular_object_count() const noexcept {
    return object_count() - ecs_object_count();
}

World::Statistics World::statistics() const noexcept {
    return {object_count(), regular_object_count(), ecs_object_count(), component_count(),
            registry_.alive_count(), registry_.pending_destroy_count(), updateCount_, lastDeltaSeconds_};
}

void World::reserve(std::size_t objectCapacity) {
    objectLookup_.reserve(objectCapacity);
    objectOrder_.reserve(objectCapacity);
    regularObjectOrder_.reserve(objectCapacity);
    ecsObjectOrder_.reserve(objectCapacity);
    componentLookup_.reserve(objectCapacity * 2u);
    gameObjects_.reserve(objectCapacity);
    registry_.reserve(objectCapacity);
}

void World::optimize() {
    const auto objectCapacity = std::max(object_count() + std::size_t{16}, object_count() * 2u);
    reserve(objectCapacity);
    componentLookup_.reserve(std::max(component_count() + std::size_t{16}, component_count() * 2u));
    registry_.reserve(std::max(registry_.alive_count() + std::size_t{16}, registry_.capacity()));
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
    const bool registered = [&] {
        const auto it = objectLookup_.find(object.id_);
        return it != objectLookup_.end() && it->second == &object;
    }();
    const auto oldStorage = object.storage_;
    if (registered && oldStorage != ObjectStorage::Ecs)
        unregister_storage_object(object, oldStorage);
    if (!object.ecsEntity_) {
        object.ecsEntity_ = registry_.create();
        entityLookup_[object.ecsEntity_] = &object;
    }
    object.storage_ = ObjectStorage::Ecs;
    if (registered && oldStorage != ObjectStorage::Ecs) register_storage_object(object);
    return object.ecsEntity_;
}

void World::disable_ecs(GameObject& object) noexcept {
    if (object.world_ != this || !object.ecsEntity_) return;
    const auto entity = object.ecsEntity_;
    const bool registered = [&] {
        const auto it = objectLookup_.find(object.id_);
        return it != objectLookup_.end() && it->second == &object;
    }();
    if (registered && object.storage_ == ObjectStorage::Ecs)
        unregister_storage_object(object, ObjectStorage::Ecs);
    object.dispose_ecs_components();
    entityLookup_.erase(entity);
    object.ecsEntity_ = {};
    object.storage_ = ObjectStorage::Regular;
    registry_.destroy(entity);
    // Objects that remain alive need to re-enter the regular lane.  During
    // destruction release_tree() calls this function and unregisters the
    // object immediately afterwards, so it must not be reinserted there.
    if (registered && !object.destroyRequested_) register_storage_object(object);
}

void World::on_ecs_entity_destroyed(Entity entity) noexcept {
    const auto it = entityLookup_.find(entity);
    if (it == entityLookup_.end() || !it->second) return;
    auto& object = *it->second;
    // Registry::destroy invokes this before EnTT destroys its component pool.
    // Detach callbacks now, then let the outer registry operation reclaim the
    // storage.  Calling Registry::remove here would mutate an active destroy.
    unregister_storage_object(object, ObjectStorage::Ecs);
    object.dispose_ecs_components(false);
    object.ecsEntity_ = {};
    object.storage_ = ObjectStorage::Regular;
    entityLookup_.erase(it);
    object.request_destroy_recursive();
    if (!updating_) collect_destroyed();
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
    objectOrder_.clear();
    regularObjectOrder_.clear();
    ecsObjectOrder_.clear();
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

void World::add_ecs_system(std::unique_ptr<EcsSystem> system) {
    if (!system) return;
    auto* added = system.get();
    ecsSystems_.push_back(std::move(system));
    std::stable_sort(ecsSystems_.begin(), ecsSystems_.end(),
                     [](const auto& left, const auto& right) { return left->order() < right->order(); });
    added->attach(*this);
}

void World::clear_ecs_systems() noexcept {
    for (auto& system : ecsSystems_) if (system) system->dispose();
    ecsSystems_.clear();
}

void World::update_transform_pipeline() noexcept {
    for (std::size_t index = 0; index < gameObjects_.size(); ++index)
        if (gameObjects_[index]) gameObjects_[index]->update_transform_recursive();
}

void World::update_regular_pipeline(Seconds dt) {
    // Flat traversal keeps this pipeline independent from the ECS storage
    // while retaining creation order across mixed object hierarchies.
    const auto objectCount = regularObjectOrder_.size();
    for (std::size_t index = 0; index < objectCount && index < regularObjectOrder_.size();) {
        auto* object = regularObjectOrder_[index];
        if (object) object->update_regular_components(dt);
        if (index < regularObjectOrder_.size() && regularObjectOrder_[index] == object) ++index;
    }
}

void World::update_ecs_pipeline(Seconds dt) {
    // ECS-backed Component objects have the same lifecycle contract, but are
    // dispatched from the ECS lane.  Data-only components are intentionally
    // handled by EcsSystem views below, without GameObject indirection.
    const auto objectCount = ecsObjectOrder_.size();
    for (std::size_t index = 0; index < objectCount && index < ecsObjectOrder_.size();) {
        auto* object = ecsObjectOrder_[index];
        if (object) object->update_ecs_components(dt);
        if (index < ecsObjectOrder_.size() && ecsObjectOrder_[index] == object) ++index;
    }

    const auto ecsSystemCount = ecsSystems_.size();
    for (std::size_t index = 0; index < ecsSystemCount && index < ecsSystems_.size(); ++index)
        if (ecsSystems_[index]) ecsSystems_[index]->update(dt);
}

void World::update(Seconds dt) {
    if (!std::isfinite(dt) || dt < 0.0f) dt = 0.0f;
    updating_ = true;
    for (std::size_t index = 0; index < legacyObjects_.size(); ++index)
        if (legacyObjects_[index]) legacyObjects_[index]->update(dt);
    // Both object kinds see the same transform snapshot and frame boundary,
    // then run through independent hot paths.
    update_transform_pipeline();
    update_regular_pipeline(dt);
    update_ecs_pipeline(dt);
    const auto systemCount = systems_.size();
    for (std::size_t index = 0; index < systemCount && index < systems_.size(); ++index)
        if (systems_[index]) systems_[index](*this, dt);

    // A deferred registry destroy must pass through the GameObject lifetime
    // first, otherwise an ECS-backed Component would be destroyed by EnTT
    // without receiving detach/destroy callbacks.
    registry_.each_pending_destroy([this](Entity entity) {
        const auto it = entityLookup_.find(entity);
        if (it != entityLookup_.end() && it->second) it->second->request_destroy_recursive();
    });
    updating_ = false;
    collect_destroyed();
    registry_.flush_destroyed();
    if (automaticOptimization_) {
        if (objectOrder_.capacity() < objectOrder_.size() + 16u) objectOrder_.reserve(objectOrder_.size() * 2u + 16u);
        if (componentLookup_.load_factor() > 0.75f) componentLookup_.reserve(componentLookup_.size() * 2u + 16u);
    }
    ++updateCount_;
    lastDeltaSeconds_ = dt;
}
}

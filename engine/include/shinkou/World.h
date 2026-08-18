#pragma once

#include "shinkou/GameObject.h"
#include "shinkou/Math.h"
#include "shinkou/ecs/Registry.h"
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shinkou {
class SceneObject {
public:
    virtual ~SceneObject() = default;
    virtual void update(Seconds) {}
};

class World {
public:
    struct ComponentTypeDescriptor {
        std::string name;
        std::function<std::unique_ptr<Component>()> create;
    };

private:
    ecs::Registry registry_;
    std::vector<std::unique_ptr<GameObject>> gameObjects_;
    std::unordered_map<ObjectId, GameObject*> objectLookup_;
    std::unordered_map<Entity, GameObject*, EntityHash> entityLookup_;
    std::unordered_map<ComponentId, Component*> componentLookup_;
    std::unordered_map<std::string, ComponentTypeDescriptor> componentTypes_;
    std::vector<std::unique_ptr<SceneObject>> legacyObjects_;
    std::vector<std::function<void(World&, Seconds)>> systems_;
    ObjectId nextObjectId_{1};
    ComponentId nextComponentId_{1};
    bool updating_{false};

    void remove_destroyed(GameObject& object) noexcept;
    void collect_destroyed() noexcept;
    void register_object(GameObject& object) noexcept;
    void unregister_object(GameObject& object) noexcept;
    void register_component(Component& component, std::string_view registeredTypeName) noexcept;
    void unregister_component(Component& component) noexcept;
    void release_tree(GameObject& object) noexcept;
    Entity enable_ecs(GameObject& object);
    void disable_ecs(GameObject& object) noexcept;
    GameObject& create_object(std::string name, GameObject* parent);
    void reparent_object(GameObject& object, GameObject* parent);

    friend class GameObject;

public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    ecs::Registry& ecs() noexcept { return registry_; }
    const ecs::Registry& ecs() const noexcept { return registry_; }

    GameObject& create_object(std::string name = {});
    GameObject* find_object(ObjectId id) noexcept;
    const GameObject* find_object(ObjectId id) const noexcept;
    GameObject* find_object(Entity entity) noexcept;
    const GameObject* find_object(Entity entity) const noexcept;
    Component* find_component(ComponentId id) noexcept;
    const Component* find_component(ComponentId id) const noexcept;
    std::vector<std::string> component_types() const;

    template<class T>
    bool register_component_type(std::string name) {
        if (name.empty() || componentTypes_.find(name) != componentTypes_.end()) return false;
        const auto key = name;
        componentTypes_.emplace(std::move(name), ComponentTypeDescriptor{key, [] { return std::make_unique<T>(); }});
        return true;
    }
    std::size_t object_count() const noexcept { return objectLookup_.size(); }
    void destroy_object(GameObject& object) noexcept;
    void clear_objects() noexcept;

    template<class Fn>
    void each_object(Fn&& fn) {
        for (auto& object : gameObjects_) {
            if (object && !object->destroy_requested()) fn(*object);
        }
    }

    template<class Fn>
    void each_object(Fn&& fn) const {
        for (const auto& object : gameObjects_) {
            if (object && !object->destroy_requested()) fn(*object);
        }
    }

    template<class TObject, class... Args>
    TObject& add_object(Args&&... args) {
        auto object = std::make_unique<TObject>(std::forward<Args>(args)...);
        auto& result = *object;
        legacyObjects_.push_back(std::move(object));
        return result;
    }
    void add_system(std::function<void(World&, Seconds)> system);
    void clear_systems() { systems_.clear(); }
    void update(Seconds dt);
};

template<class T, class... Args>
T& GameObject::add_ecs_component(Args&&... args) {
    assert(ecsEntity_);
    return world_->ecs().template emplace_or_replace<T>(ecsEntity_, std::forward<Args>(args)...);
}

template<class T>
T* GameObject::get_ecs_component() noexcept {
    return ecsEntity_ ? world_->ecs().template try_get<T>(ecsEntity_) : nullptr;
}

template<class T>
const T* GameObject::get_ecs_component() const noexcept {
    return ecsEntity_ ? world_->ecs().template try_get<T>(ecsEntity_) : nullptr;
}

template<class T>
void GameObject::remove_ecs_component() noexcept {
    if (ecsEntity_) world_->ecs().template remove<T>(ecsEntity_);
}
}

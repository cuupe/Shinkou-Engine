#pragma once

#include "shinkou/GameObject.h"
#include "shinkou/Math.h"
#include "shinkou/ecs/ChunkPipeline.h"
#include "shinkou/ecs/Registry.h"
#include "shinkou/ecs/Transform.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shinkou {
class EcsSystem;

namespace jobs {
class JobSystem;
}

class SceneObject {
  public:
    virtual ~SceneObject() = default;
    virtual void update(Seconds) {
    }
};

// Native ECS systems are ordered once at registration and then dispatched
// without per-frame allocations.  Systems query World::ecs() directly, so
// their hot loops use chunk queries rather than object wrappers.
class EcsSystem {
    World *world_{nullptr};
    LifecycleState state_{LifecycleState::Constructing};
    bool enabled_{true};

    friend class World;
    void attach(World &world) noexcept;
    void update(Seconds dt);
    void dispose() noexcept;

  public:
    EcsSystem() = default;
    virtual ~EcsSystem() = default;
    EcsSystem(const EcsSystem &) = delete;
    EcsSystem &operator=(const EcsSystem &) = delete;

    World &world() noexcept {
        return *world_;
    }
    const World &world() const noexcept {
        return *world_;
    }
    LifecycleState lifecycle_state() const noexcept {
        return state_;
    }
    bool enabled() const noexcept {
        return enabled_;
    }
    void set_enabled(bool enabled) noexcept;
    virtual int order() const noexcept {
        return 0;
    }

  protected:
    virtual void on_create() {
    }
    virtual void on_enable() {
    }
    virtual void on_disable() {
    }
    virtual void on_update(Seconds) {
    }
    virtual void on_destroy() {
    }
};

class World {
  public:
    struct ComponentTypeDescriptor {
        std::string name;
        std::function<std::unique_ptr<Component>()> create;
        std::function<Component *(GameObject &)> createInObject;
    };

    struct Statistics {
        std::size_t objects{0};
        std::size_t regularObjects{0};
        std::size_t ecsObjects{0};
        std::size_t components{0};
        std::size_t ecsEntities{0};
        std::size_t pendingEcsDestructions{0};
        FrameIndex updates{0};
        Seconds lastDeltaSeconds{0.0f};
    };

  private:
    ecs::Registry registry_;
    ecs::ChunkPipeline chunkPipeline_;
    std::vector<std::unique_ptr<GameObject>> gameObjects_;
    std::unordered_map<ObjectId, GameObject *> objectLookup_;
    // Entity id -> authoring object, indexed directly by the 8-byte handle's
    // index.  One array read, no hash, no per-frame map.
    std::vector<GameObject *> entityObjects_;
    std::unordered_map<ComponentId, Component *> componentLookup_;
    std::unordered_map<std::string, ComponentTypeDescriptor> componentTypes_;
    std::vector<std::unique_ptr<SceneObject>> legacyObjects_;
    jobs::JobSystem *jobSystem_{nullptr};
    std::vector<std::function<void(World &, Seconds)>> systems_;
    std::vector<std::unique_ptr<EcsSystem>> ecsSystems_;
    std::vector<GameObject *> objectOrder_;
    // Separate hot-path lanes.  objectOrder_ remains the unified manager
    // view; these vectors ensure each update pipeline only visits its own
    // storage backend.
    std::vector<GameObject *> regularObjectOrder_;
    std::vector<GameObject *> ecsObjectOrder_;
    // Objects whose authoring transform changed, and the per-depth work lists
    // derived from them.  A static scene leaves both empty, so the transform
    // phase costs nothing until something is edited or animated.
    std::vector<ObjectId> pendingTransformUpdates_;
    std::vector<std::vector<GameObject *>> dirtyTransformLevels_;
    ObjectId nextObjectId_{1};
    ComponentId nextComponentId_{1};
    std::size_t ecsObjectCount_{0};
    bool updating_{false};
    bool automaticOptimization_{true};
    FrameIndex updateCount_{0};
    Seconds lastDeltaSeconds_{0.0f};
    std::uint64_t hierarchyRevision_{1};
    std::uint64_t instanceId_{0};
    std::uint64_t componentTypesRevision_{1};

    void remove_destroyed(GameObject &object) noexcept;
    void collect_destroyed() noexcept;
    void register_object(GameObject &object) noexcept;
    void unregister_object(GameObject &object) noexcept;
    void register_storage_object(GameObject &object) noexcept;
    void unregister_storage_object(GameObject &object, ObjectStorage storage) noexcept;
    void register_component(Component &component, std::string_view registeredTypeName) noexcept;
    void unregister_component(Component &component) noexcept;
    void release_tree(GameObject &object) noexcept;
    Entity enable_ecs(GameObject &object);
    void disable_ecs(GameObject &object) noexcept;
    void on_ecs_entity_destroyed(Entity entity) noexcept;
    void bind_entity_object(Entity entity, GameObject *object) noexcept;
    void unbind_entity_object(Entity entity) noexcept;
    void update_transform_pipeline() noexcept;
    void update_level_transforms() noexcept;
    void queue_transform_update(GameObject &object) noexcept;
    void mark_transform_subtree(GameObject &object) noexcept;
    void update_regular_pipeline(Seconds dt);
    void update_ecs_pipeline(Seconds dt);
    GameObject &create_object(std::string name, GameObject *parent, ObjectStorage storage);
    void reparent_object(GameObject &object, GameObject *parent);
    bool set_object_storage(GameObject &object, ObjectStorage storage);

    friend class GameObject;

  public:
    World();
    ~World();
    World(const World &) = delete;
    World &operator=(const World &) = delete;

    ecs::Registry &ecs() noexcept {
        return registry_;
    }
    const ecs::Registry &ecs() const noexcept {
        return registry_;
    }
    // Typed read/write kernels run after legacy EcsSystems. const T is a read;
    // T is a write. Only this explicitly declared lane overlaps systems.
    ecs::ChunkPipeline &chunk_pipeline() noexcept {
        return chunkPipeline_;
    }

    // Optional worker pool.  When set, independent root subtrees advance their
    // transforms in parallel; ECS systems may also use it for chunk dispatch.
    // The World never owns the pool, so its lifetime stays with the host.
    void set_job_system(jobs::JobSystem *system) noexcept {
        jobSystem_ = system;
    }
    jobs::JobSystem *job_system() const noexcept {
        return jobSystem_;
    }

    GameObject &create_object(std::string name = {});
    GameObject &create_object(std::string name, ObjectStorage storage);
    GameObject *find_object(ObjectId id) noexcept;
    const GameObject *find_object(ObjectId id) const noexcept;
    GameObject *find_object(Entity entity) noexcept;
    const GameObject *find_object(Entity entity) const noexcept;
    Component *find_component(ComponentId id) noexcept;
    const Component *find_component(ComponentId id) const noexcept;
    std::vector<std::string> component_types() const;

    template <class T> bool register_component_type(std::string name) {
        if (name.empty() || componentTypes_.find(name) != componentTypes_.end())
            return false;
        const auto key = name;
        componentTypes_.emplace(
            std::move(name),
            ComponentTypeDescriptor{key, [] { return std::make_unique<T>(); },
                                    [key](GameObject &object) {
                                        return object.template add_registered_component<T>(key);
                                    }});
        ++componentTypesRevision_;
        return true;
    }
    // Changed by manager operations, including edits made outside the editor.
    std::uint64_t hierarchy_revision() const noexcept {
        return hierarchyRevision_;
    }
    std::uint64_t instance_id() const noexcept {
        return instanceId_;
    }
    std::uint64_t component_types_revision() const noexcept {
        return componentTypesRevision_;
    }
    std::size_t object_count() const noexcept {
        return objectLookup_.size();
    }
    std::size_t regular_object_count() const noexcept;
    std::size_t ecs_object_count() const noexcept {
        return ecsObjectCount_;
    }
    std::size_t component_count() const noexcept {
        return componentLookup_.size();
    }
    Statistics statistics() const noexcept;
    void reserve(std::size_t objectCapacity);
    void optimize();
    void set_automatic_optimization(bool enabled) noexcept {
        automaticOptimization_ = enabled;
    }
    bool automatic_optimization() const noexcept {
        return automaticOptimization_;
    }
    void destroy_object(GameObject &object) noexcept;
    void clear_objects() noexcept;

    template <class Fn> void each_object(Fn &&fn) {
        for (auto &object : gameObjects_) {
            if (object && !object->destroy_requested())
                fn(*object);
        }
    }

    template <class Fn> void each_object(Fn &&fn) const {
        for (const auto &object : gameObjects_) {
            if (object && !object->destroy_requested())
                fn(*object);
        }
    }

    // Flat object traversal for systems that do not need hierarchy order.
    // The vector is maintained by the manager and never allocates during an
    // update, making this the preferred query for large object populations.
    template <class Fn> void each_game_object(Fn &&fn) {
        for (auto *object : objectOrder_) {
            if (object && !object->destroy_requested())
                fn(*object);
        }
    }

    template <class Fn> void each_game_object(Fn &&fn) const {
        for (const auto *object : objectOrder_) {
            if (object && !object->destroy_requested())
                fn(*object);
        }
    }

    template <class TObject, class... Args> TObject &add_object(Args &&...args) {
        auto object = std::make_unique<TObject>(std::forward<Args>(args)...);
        auto &result = *object;
        legacyObjects_.push_back(std::move(object));
        return result;
    }
    void add_system(std::function<void(World &, Seconds)> system);
    void clear_systems() {
        systems_.clear();
    }
    template <class T, class... Args> T &add_ecs_system(Args &&...args);
    void add_ecs_system(std::unique_ptr<EcsSystem> system);
    void clear_ecs_systems() noexcept;
    std::size_t ecs_system_count() const noexcept {
        return ecsSystems_.size();
    }
    void update(Seconds dt);
    // Flush spatial edits without advancing game logic (editor pause mode).
    void refresh_transforms() noexcept {
        update_transform_pipeline();
    }
};

template <class T, class... Args>
T *GameObject::add_component_internal(std::string_view registeredTypeName, Args &&...args) {
    static_assert(std::is_base_of_v<Component, T>, "T must derive from shinkou::Component");
    const auto key = std::type_index(typeid(T));
    if (componentLookup_.find(key) != componentLookup_.end())
        return nullptr;

    auto component = std::make_unique<T>(std::forward<Args>(args)...);
    T *result = component.get();
    components_.push_back(std::move(component));
    componentLookup_.emplace(key, result);
    attach_component(*result,
                     registeredTypeName.empty() ? result->type_name() : registeredTypeName);
    return result;
}

template <class T> T *GameObject::add_registered_component(std::string_view registeredTypeName) {
    return add_component_internal<T>(registeredTypeName);
}

template <class T, class... Args> T *GameObject::add_component(Args &&...args) {
    return add_component_internal<T>({}, std::forward<Args>(args)...);
}

template <class T, class... Args> T &GameObject::add_ecs_component(Args &&...args) {
    static_assert(
        !std::is_base_of_v<Component, T>,
        "authoring Components are owned by GameObject::components_; chunk memory is POD-only");
    static_assert(std::is_trivially_copyable_v<T>,
                  "chunk components are memcpy-migrated; they must be trivially copyable");
    if (!ecsEntity_)
        enable_ecs();
    return world_->ecs().template emplace_or_replace<T>(ecsEntity_, std::forward<Args>(args)...);
}

template <class T> T *GameObject::get_ecs_component() noexcept {
    if constexpr (std::is_base_of_v<Component, T>) {
        return get_component<T>();
    } else {
        return ecsEntity_ ? world_->ecs().template try_get<T>(ecsEntity_) : nullptr;
    }
}

template <class T> const T *GameObject::get_ecs_component() const noexcept {
    if constexpr (std::is_base_of_v<Component, T>) {
        return get_component<T>();
    } else {
        return ecsEntity_ ? world_->ecs().template try_get<T>(ecsEntity_) : nullptr;
    }
}

template <class T> void GameObject::remove_ecs_component() noexcept {
    if constexpr (std::is_base_of_v<Component, T>) {
        remove_component<T>();
    } else if (ecsEntity_) {
        world_->ecs().template remove<T>(ecsEntity_);
    }
}

template <class T, class... Args> T &World::add_ecs_system(Args &&...args) {
    auto system = std::make_unique<T>(std::forward<Args>(args)...);
    auto &result = *system;
    add_ecs_system(std::move(system));
    return result;
}
} // namespace shinkou

#pragma once

#include "shinkou/Types.h"
#include "shinkou/ecs/EntityStore.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace shinkou::ecs {

// EnTT-shaped facade over the chunk store.
//
// The facade exists so that simulation code keeps its familiar
// create/emplace/get/view vocabulary while the storage underneath is the
// DOTS-style archetype/chunk store.  There is no sparse-set indirection and no
// native-entity translation: an Entity is exactly the 8-byte store handle.
class Registry {
  public:
    using DestroyObserver = void (*)(void *, Entity) noexcept;

    Registry() = default;
    ~Registry() = default;
    Registry(const Registry &) = delete;
    Registry &operator=(const Registry &) = delete;

    Entity create() {
        return store_.create();
    }
    Entity create(Archetype &archetype) {
        return store_.create(archetype);
    }
    void create_batch(Archetype &archetype, Entity *output, std::size_t count) {
        store_.create_batch(archetype, output, count);
    }
    void destroy(Entity entity);
    void destroy_deferred(Entity entity);
    std::size_t flush_destroyed() noexcept;
    void clear() noexcept;
    bool valid(Entity entity) const noexcept {
        return store_.valid(entity);
    }
    void set_destroy_observer(void *context, DestroyObserver observer) noexcept {
        destroyObserverContext_ = context;
        destroyObserver_ = observer;
    }
    void reserve(std::size_t entityCapacity) {
        store_.reserve(entityCapacity);
    }
    std::size_t alive_count() const noexcept {
        return store_.alive_count();
    }
    std::size_t capacity() const noexcept {
        return store_.capacity();
    }
    std::size_t pending_destroy_count() const noexcept {
        return pendingDestroy_.size();
    }

    template <class Fn> void each_pending_destroy(Fn &&fn) const {
        for (const auto entity : pendingDestroy_) {
            if (store_.valid(entity))
                fn(entity);
        }
    }

    EntityStore &store() noexcept {
        return store_;
    }
    const EntityStore &store() const noexcept {
        return store_;
    }

    // Mirrors the EnTT view surface the engine already used.  Iteration is
    // chunk-major and allocation-free once the query cache is warm.
    template <class... Components> struct View {
        Query *query{nullptr};

        template <class Fn> void each(Fn &&fn) {
            query->template each<Components...>(std::forward<Fn>(fn));
        }
        std::size_t size() {
            return query->entity_count();
        }
        bool empty() {
            return size() == 0u;
        }
    };

    template <class... Components> View<Components...> view() const {
        return View<Components...>{&store_.query<Components...>()};
    }

    template <class T> bool has(Entity entity) const {
        return store_.has<T>(entity);
    }

    template <class T, class... Args> T &emplace(Entity entity, Args &&...args) {
        return store_.emplace<T>(entity, std::forward<Args>(args)...);
    }

    template <class T, class... Args> T &emplace_or_replace(Entity entity, Args &&...args) {
        return store_.emplace_or_replace<T>(entity, std::forward<Args>(args)...);
    }

    template <class T> T &get(Entity entity) {
        return store_.get<T>(entity);
    }

    template <class T> const T &get(Entity entity) const {
        return store_.get<T>(entity);
    }

    template <class T> T *try_get(Entity entity) {
        return store_.try_get<T>(entity);
    }

    template <class T> const T *try_get(Entity entity) const {
        return store_.try_get<T>(entity);
    }

    template <class T> void remove(Entity entity) {
        store_.remove<T>(entity);
    }

    template <class... Components, class Fn> void each(Fn &&fn) const {
        view<Components...>().each(std::forward<Fn>(fn));
    }

  private:
    EntityStore store_;
    std::vector<Entity> pendingDestroy_;
    void *destroyObserverContext_{nullptr};
    DestroyObserver destroyObserver_{nullptr};
};

} // namespace shinkou::ecs

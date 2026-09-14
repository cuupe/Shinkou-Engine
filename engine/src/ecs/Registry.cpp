#include "shinkou/ecs/Registry.h"

#include <algorithm>

namespace shinkou::ecs {
Entity Registry::create() {
    EntityId id = 0;
    if (!freeIds_.empty()) {
        id = freeIds_.back();
        freeIds_.pop_back();
    } else {
        id = nextId_++;
        generations_.resize(static_cast<std::size_t>(id) + 1u, 0);
        entities_.resize(static_cast<std::size_t>(id) + 1u, entt::null);
    }
    const auto native = registry_.create();
    if (id >= entities_.size()) entities_.resize(static_cast<std::size_t>(id) + 1u, entt::null);
    entities_[id] = native;
    const Entity result{id, generations_[id]};
    entityLookup_[entt::to_integral(native)] = result;
    return result;
}

void Registry::destroy(Entity entity) {
    if (!valid(entity)) return;
    const auto nativeEntity = native(entity);
    if (destroyObserver_) destroyObserver_(destroyObserverContext_, entity);
    entityLookup_.erase(entt::to_integral(nativeEntity));
    registry_.destroy(nativeEntity);
    entities_[entity.id] = entt::null;
    ++generations_[entity.id];
    freeIds_.push_back(entity.id);
}

void Registry::destroy_deferred(Entity entity) {
    if (valid(entity)) pendingDestroy_.push_back(entity);
}

std::size_t Registry::flush_destroyed() noexcept {
    std::size_t destroyed = 0;
    for (const auto entity : pendingDestroy_) {
        if (!valid(entity)) continue;
        destroy(entity);
        ++destroyed;
    }
    pendingDestroy_.clear();
    return destroyed;
}

void Registry::clear() noexcept {
    if (destroyObserver_) {
        for (const auto& entry : entityLookup_)
            destroyObserver_(destroyObserverContext_, entry.second);
    }
    registry_.clear();
    // Keep generation history across a clear.  Resetting it would make a
    // handle obtained before the clear valid again after the next create.
    for (std::size_t index = 1; index < generations_.size(); ++index) {
        ++generations_[index];
    }
    std::fill(entities_.begin(), entities_.end(), entt::null);
    nextId_ = 1;
    freeIds_.clear();
    entityLookup_.clear();
    pendingDestroy_.clear();
}

bool Registry::valid(Entity entity) const noexcept {
    return entity.id != 0 && entity.id < generations_.size() &&
           entity.id < entities_.size() && entities_[entity.id] != entt::null &&
           generations_[entity.id] == entity.generation;
}

void Registry::reserve(std::size_t entityCapacity) {
    generations_.reserve(entityCapacity + 1u);
    entities_.reserve(entityCapacity + 1u);
    freeIds_.reserve(entityCapacity);
    pendingDestroy_.reserve(entityCapacity);
    entityLookup_.reserve(entityCapacity);
}
}

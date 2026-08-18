#include "shinkou/ecs/Registry.h"

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
    entityLookup_.erase(entt::to_integral(nativeEntity));
    registry_.destroy(nativeEntity);
    entities_[entity.id] = entt::null;
    ++generations_[entity.id];
    freeIds_.push_back(entity.id);
}

void Registry::clear() noexcept {
    registry_.clear();
    nextId_ = 1;
    generations_.assign(1, 0);
    freeIds_.clear();
    entities_.assign(1, entt::null);
    entityLookup_.clear();
}

bool Registry::valid(Entity entity) const noexcept {
    return entity.id != 0 && entity.id < generations_.size() &&
           generations_[entity.id] == entity.generation;
}
}

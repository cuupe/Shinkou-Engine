#include "shinkou/ecs/Registry.h"

namespace shinkou::ecs {

void Registry::destroy(Entity entity) {
    if (!store_.valid(entity))
        return;
    // Fire the observer before the slot is recycled so the World can detach its
    // authoring object while the handle is still resolvable.
    if (destroyObserver_)
        destroyObserver_(destroyObserverContext_, entity);
    store_.destroy(entity);
}

void Registry::destroy_deferred(Entity entity) {
    if (store_.valid(entity))
        pendingDestroy_.push_back(entity);
}

std::size_t Registry::flush_destroyed() noexcept {
    std::size_t destroyed = 0;
    for (const auto entity : pendingDestroy_) {
        if (!store_.valid(entity))
            continue;
        destroy(entity);
        ++destroyed;
    }
    pendingDestroy_.clear();
    return destroyed;
}

void Registry::clear() noexcept {
    if (destroyObserver_) {
        store_.each_entity(
            [this](Entity entity) { destroyObserver_(destroyObserverContext_, entity); });
    }
    store_.clear();
    pendingDestroy_.clear();
}

} // namespace shinkou::ecs

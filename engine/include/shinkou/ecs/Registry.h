#pragma once

#include "shinkou/Types.h"
#include <entt/entt.hpp>
#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace shinkou::ecs {
class Registry {
    entt::registry registry_;
    EntityId nextId_{1};
    std::vector<Generation> generations_{1, 0};
    std::vector<EntityId> freeIds_;
    std::vector<entt::entity> entities_{entt::null};
    std::unordered_map<std::uint32_t, Entity> entityLookup_;

    entt::entity native(Entity entity) const noexcept {
        return valid(entity) ? entities_[entity.id] : entt::null;
    }

    Entity from_native(entt::entity entity) const noexcept {
        const auto it = entityLookup_.find(entt::to_integral(entity));
        return it == entityLookup_.end() ? Entity{} : it->second;
    }

public:
    Entity create();
    void destroy(Entity entity);
    void clear() noexcept;
    bool valid(Entity entity) const noexcept;

    entt::registry& native_registry() noexcept { return registry_; }
    const entt::registry& native_registry() const noexcept { return registry_; }

    template<class T>
    bool has(Entity entity) const {
        return valid(entity) && registry_.all_of<T>(native(entity));
    }

    template<class T, class... Args>
    T& emplace(Entity entity, Args&&... args) {
        assert(valid(entity));
        return registry_.emplace<T>(native(entity), std::forward<Args>(args)...);
    }

    template<class T, class... Args>
    T& emplace_or_replace(Entity entity, Args&&... args) {
        assert(valid(entity));
        return registry_.emplace_or_replace<T>(native(entity), std::forward<Args>(args)...);
    }

    template<class T>
    T& get(Entity entity) {
        assert(valid(entity));
        return registry_.get<T>(native(entity));
    }

    template<class T>
    const T& get(Entity entity) const {
        assert(valid(entity));
        return registry_.get<T>(native(entity));
    }

    template<class T>
    T* try_get(Entity entity) {
        return valid(entity) ? registry_.try_get<T>(native(entity)) : nullptr;
    }

    template<class T>
    const T* try_get(Entity entity) const {
        return valid(entity) ? registry_.try_get<T>(native(entity)) : nullptr;
    }

    template<class T>
    void remove(Entity entity) {
        if (valid(entity)) registry_.remove<T>(native(entity));
    }

    template<class T, class Fn>
    void each(Fn&& fn) {
        registry_.view<T>().each([this, &fn](const auto entity, T& component) {
            fn(from_native(entity), component);
        });
    }

    template<class T, class Fn>
    void each(Fn&& fn) const {
        registry_.view<T>().each([this, &fn](const auto entity, const T& component) {
            fn(from_native(entity), component);
        });
    }

    template<class First, class Second, class... Rest, class Fn>
    void each(Fn&& fn) {
        registry_.view<First, Second, Rest...>().each([this, &fn](const auto entity,
                                                                  First& first, Second& second,
                                                                  Rest&... rest) {
            fn(from_native(entity), first, second, rest...);
        });
    }

    template<class First, class Second, class... Rest, class Fn>
    void each(Fn&& fn) const {
        registry_.view<First, Second, Rest...>().each([this, &fn](const auto entity,
                                                                                    const First& first,
                                                                                    const Second& second,
                                                                                    const Rest&... rest) {
            fn(from_native(entity), first, second, rest...);
        });
    }
};
}

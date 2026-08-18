#pragma once

#include "shinkou/physics/PhysicsWorld.h"
#include <unordered_map>

namespace shinkou::physics {
class SimplePhysicsWorld final : public IPhysicsWorld {
    struct Body { BodyDesc desc; bool alive{true}; };
    std::unordered_map<BodyId, Body> bodies_;
    BodyId nextId_{1};
    math::Vec3 gravity_{0, -9.81f, 0};
public:
    BodyId create_body(const BodyDesc&) override;
    void destroy_body(BodyId) override;
    void step(float dt) override;
    bool raycast(math::Vec3 origin, math::Vec3 direction, float distance,
                 RaycastHit& hit) const override;
    void set_gravity(math::Vec3 gravity) noexcept { gravity_ = gravity; }
};
}

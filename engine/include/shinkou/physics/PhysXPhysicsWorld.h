#pragma once

#include "shinkou/physics/PhysicsWorld.h"
#include <memory>

namespace shinkou::physics {
class PhysXPhysicsWorld final : public IPhysicsWorld {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    PhysXPhysicsWorld();
    ~PhysXPhysicsWorld() override;
    BodyId create_body(const BodyDesc&) override;
    void destroy_body(BodyId) override;
    void step(float dt) override;
    bool raycast(math::Vec3 origin, math::Vec3 direction,
                 float distance, RaycastHit& hit) const override;
};
}

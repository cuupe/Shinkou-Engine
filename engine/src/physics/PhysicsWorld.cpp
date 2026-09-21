#include "shinkou/physics/PhysicsWorld.h"
#include "shinkou/physics/PhysXPhysicsWorld.h"
#include "shinkou/physics/SimplePhysicsWorld.h"
#include <algorithm>
#include <cmath>

namespace shinkou::physics {

bool PhysicsMaterialDesc::valid() const noexcept {
    return math::IsFinite(staticFriction) && math::IsFinite(dynamicFriction) && math::IsFinite(restitution) &&
           staticFriction >= 0.0f && dynamicFriction >= 0.0f && restitution >= 0.0f && restitution <= 1.0f;
}

std::unique_ptr<IPhysicsWorld> create_physics_world(const PhysicsWorldConfig& config) {
    if (config.backend == PhysicsBackend::Simple) return std::make_unique<SimplePhysicsWorld>(config);

    auto physx = std::make_unique<PhysXPhysicsWorld>(config);
    if (config.backend == PhysicsBackend::PhysX || physx->is_available()) return physx;
    return std::make_unique<SimplePhysicsWorld>(config);
}

bool physx_available() noexcept {
#if defined(SHINKOU_WITH_PHYSX)
    return true;
#else
    return false;
#endif
}

} // namespace shinkou::physics

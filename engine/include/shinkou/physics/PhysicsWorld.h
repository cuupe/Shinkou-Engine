#pragma once

#include "shinkou/Math.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace shinkou::physics {
using BodyId = std::uint32_t;

struct BodyDesc {
    math::Vec3 position{};
    math::Vec3 velocity{};
    math::Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    float mass{1.0f};
    bool dynamic{true};
};

struct RaycastHit {
    BodyId body{0};
    math::Vec3 point{};
    math::Vec3 normal{};
    float distance{0};
};

class IPhysicsWorld {
public:
    virtual ~IPhysicsWorld() = default;
    virtual BodyId create_body(const BodyDesc&) = 0;
    virtual void destroy_body(BodyId) = 0;
    virtual void step(float dt) = 0;
    virtual bool raycast(math::Vec3 origin, math::Vec3 direction,
                         float distance, RaycastHit& hit) const = 0;
};
}

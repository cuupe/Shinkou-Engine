#include "shinkou/physics/SimplePhysicsWorld.h"

namespace shinkou::physics {
BodyId SimplePhysicsWorld::create_body(const BodyDesc& desc) {
    const BodyId id = nextId_++;
    bodies_.emplace(id, Body{desc, true});
    return id;
}

void SimplePhysicsWorld::destroy_body(BodyId id) { bodies_.erase(id); }

void SimplePhysicsWorld::step(float dt) {
    for (auto& [id, body] : bodies_) {
        (void)id;
        if (!body.alive || !body.desc.dynamic) continue;
        body.desc.velocity = body.desc.velocity + gravity_ * dt;
        body.desc.position = body.desc.position + body.desc.velocity * dt;
        if (body.desc.position.y - body.desc.halfExtents.y < 0.0f) {
            body.desc.position.y = body.desc.halfExtents.y;
            if (body.desc.velocity.y < 0.0f) body.desc.velocity.y *= -0.35f;
        }
    }
}

bool SimplePhysicsWorld::raycast(math::Vec3 origin, math::Vec3 direction,
                                 float distance, RaycastHit& hit) const {
    const auto dir = math::Normalize(direction);
    bool found = false;
    float closest = distance;
    for (const auto& [id, body] : bodies_) {
        if (!body.alive) continue;
        const auto delta = body.desc.position - origin;
        const float projected = math::Dot(delta, dir);
        if (projected < 0 || projected > closest) continue;
        const auto point = origin + dir * projected;
        const auto error = point - body.desc.position;
        if (math::Length(error) <= math::Length(body.desc.halfExtents)) {
            closest = projected;
            hit = {id, point, math::Normalize(origin - body.desc.position), projected};
            found = true;
        }
    }
    return found;
}
}

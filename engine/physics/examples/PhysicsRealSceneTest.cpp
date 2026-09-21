#include "shinkou/physics/PhysicsWorld.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
using namespace shinkou::physics;
using shinkou::math::Vec3;

bool nearly_equal_or_above(float value, float lowerBound) {
    return std::isfinite(value) && value >= lowerBound;
}
}

int main() {
    PhysicsWorldConfig config;
    config.backend = PhysicsBackend::PhysX;
    config.gravity = {0.0f, -9.81f, 0.0f};
    config.useFixedTimeStep = true;
    config.fixedTimeStep = 1.0f / 60.0f;
    config.maxSubSteps = 4;
    config.enableContactEvents = true;
    config.contactEventCapacity = 16384;

    auto world = create_physics_world(config);
    if (!world || !world->is_available() || world->backend() != PhysicsBackend::PhysX) {
        std::cout << "REAL_PHYSX_SCENE SKIP: PhysX unavailable: "
                  << (world ? world->last_error() : "allocation failed") << '\n';
        return EXIT_SUCCESS;
    }

    std::size_t contactCallbacks = 0;
    world->set_contact_listener([&contactCallbacks](const ContactEvent&) { ++contactCallbacks; });

    BodyDesc ground;
    ground.dynamic = false;
    ground.position = {0.0f, -0.5f, 0.0f};
    ground.halfExtents = {8.0f, 0.5f, 8.0f};
    const BodyId groundId = world->create_body(ground);

    std::vector<BodyId> stack;
    for (int index = 0; index < 4; ++index) {
        BodyDesc body;
        body.dynamic = true;
        body.position = {0.0f, 1.0f + static_cast<float>(index) * 1.1f, 0.0f};
        body.velocity = {0.15f * static_cast<float>(index + 1), 0.0f, 0.0f};
        body.angularVelocity = {0.0f, 0.35f, 0.2f};
        body.halfExtents = {0.5f, 0.5f, 0.5f};
        body.mass = 1.0f + static_cast<float>(index) * 0.25f;
        body.userData = 100u + static_cast<std::uint64_t>(index);
        stack.push_back(world->create_body(body));
    }
    if (groundId == InvalidBodyId || stack.size() != 4 || stack.back() == InvalidBodyId) {
        std::cerr << "REAL_PHYSX_SCENE FAIL: body creation failed: " << world->last_error() << '\n';
        return 2;
    }

    BodyState initialTop;
    if (!world->get_body_state(stack.back(), initialTop)) return 3;
    for (int frame = 0; frame < 360; ++frame) world->step(1.0f / 60.0f);

    BodyState bottom;
    BodyState top;
    if (!world->get_body_state(stack.front(), bottom) || !world->get_body_state(stack.back(), top)) {
        std::cerr << "REAL_PHYSX_SCENE FAIL: state query failed\n";
        return 4;
    }

    RaycastQuery ray;
    ray.origin = {0.0f, 10.0f, 0.0f};
    ray.direction = {0.0f, -1.0f, 0.0f};
    ray.maxDistance = 20.0f;
    ray.includeDynamic = false;
    RaycastHit hit;
    if (!world->raycast(ray, hit) || hit.body != groundId) {
        std::cerr << "REAL_PHYSX_SCENE FAIL: ground raycast missed\n";
        return 5;
    }

    const bool fell = top.position.y < initialTop.position.y - 0.5f;
    const bool resting = nearly_equal_or_above(bottom.position.y, 0.40f);
    const bool stepped = world->statistics().simulationSteps >= 360;
    const bool contacted = contactCallbacks > 0 || world->statistics().lastContactEventCount > 0;
    if (!fell || !resting || !stepped || !contacted) {
        std::cerr << "REAL_PHYSX_SCENE FAIL: fell=" << fell
                  << " resting=" << resting
                  << " stepped=" << stepped
                  << " contacted=" << contacted
                  << " callbacks=" << contactCallbacks
                  << " lastContacts=" << world->statistics().lastContactEventCount << '\n';
        return 6;
    }

    const Vec3 beforeImpulse = top.position;
    if (!world->set_linear_velocity(stack.back(), {4.0f, 3.0f, 0.0f})) return 7;
    for (int frame = 0; frame < 30; ++frame) world->step(1.0f / 60.0f);
    BodyState afterImpulse;
    if (!world->get_body_state(stack.back(), afterImpulse) || afterImpulse.position.x <= beforeImpulse.x + 0.1f) {
        std::cerr << "REAL_PHYSX_SCENE FAIL: velocity interaction did not move the top body\n";
        return 8;
    }

    std::cout << "REAL_PHYSX_SCENE PASS\n"
              << "  backend: " << world->backend_name() << '\n'
              << "  bottom-y: " << bottom.position.y << '\n'
              << "  top-y: " << top.position.y << '\n'
              << "  contact-callbacks: " << contactCallbacks << '\n'
              << "  raycast-body: " << hit.body << '\n'
              << "  simulation-steps: " << world->statistics().simulationSteps << '\n';
    return EXIT_SUCCESS;
}

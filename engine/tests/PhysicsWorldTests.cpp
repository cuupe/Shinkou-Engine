#include "shinkou/physics/PhysicsWorld.h"
#include <iostream>
#include <vector>

int main() {
    using namespace shinkou::physics;
    PhysicsWorldConfig config;
    config.backend = PhysicsBackend::Simple;
    config.useFixedTimeStep = false;
    config.gravity = {0.0f, -9.81f, 0.0f};

    auto world = create_physics_world(config);
    if (!world || world->backend() != PhysicsBackend::Simple || !world->is_available()) return 1;
    if (!PhysicsMaterialDesc{}.valid()) return 2;
    PhysicsMaterialDesc invalidMaterial;
    invalidMaterial.restitution = 2.0f;
    if (invalidMaterial.valid()) return 3;

    BodyDesc ground;
    ground.position = {0.0f, -0.5f, 0.0f};
    ground.dynamic = false;
    ground.halfExtents = {5.0f, 0.5f, 5.0f};
    const BodyId groundId = world->create_body(ground);
    if (!world->is_valid(groundId)) return 4;

    BodyDesc body;
    body.position = {0.0f, 2.0f, 0.0f};
    body.mass = 2.0f;
    body.userData = 42;
    body.halfExtents = {0.5f, 0.5f, 0.5f};
    const BodyId bodyId = world->create_body(body);
    if (!world->is_valid(bodyId)) return 5;

    BodyState state;
    if (!world->get_body_state(bodyId, state) || state.userData != 42 || !state.dynamic) return 6;
    world->step(0.1f);
    if (!world->get_body_state(bodyId, state) || !(state.position.y < 2.0f)) return 7;
    if (!world->add_force(bodyId, {0.0f, 20.0f, 0.0f})) return 8;
    if (!world->set_linear_velocity(bodyId, {1.0f, 0.0f, 0.0f})) return 9;
    if (!world->set_body_transform(bodyId, {1.0f, 3.0f, 0.0f}, shinkou::math::Quat::Identity())) return 10;

    RaycastHit hit;
    if (!world->raycast({1.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, hit) || hit.body != bodyId || hit.userData != 42) return 11;
    RaycastQuery staticOnly;
    staticOnly.origin = {0.0f, 5.0f, 0.0f};
    staticOnly.direction = {0.0f, -1.0f, 0.0f};
    staticOnly.maxDistance = 10.0f;
    staticOnly.includeDynamic = false;
    if (!world->raycast(staticOnly, hit) || hit.body != groundId) return 12;

    std::vector<BodyId> ids;
    world->get_body_ids(ids);
    if (ids.size() != 2) return 13;
    std::vector<BodyState> states;
    world->get_body_states(states);
    if (states.size() != 2) return 14;
    world->destroy_body(bodyId);
    if (world->is_valid(bodyId)) return 15;

    auto automatic = create_physics_world();
    if (!automatic || !automatic->is_available()) return 16;
    const char* automaticBackend = automatic->backend_name();
    const BodyId automaticBody = automatic->create_body({});
    if (!automatic->is_valid(automaticBody)) return 17;

    if (physx_available()) {
        // PhysX permits one PxFoundation instance per process. Release the
        // Auto world before constructing the explicit PhysX coverage world.
        automatic.reset();
        PhysicsWorldConfig physxConfig = config;
        physxConfig.backend = PhysicsBackend::PhysX;
        auto physx = create_physics_world(physxConfig);
        if (!physx || !physx->is_available()) return 18;
        BodyDesc physxGround = ground;
        BodyDesc physxBody = body;
        physxBody.position = {0.0f, 2.0f, 0.0f};
        const BodyId physxGroundId = physx->create_body(physxGround);
        const BodyId physxBodyId = physx->create_body(physxBody);
        if (physxGroundId == InvalidBodyId || physxBodyId == InvalidBodyId) return 19;
        physx->step(0.1f);
        if (!physx->get_body_state(physxBodyId, state) || !(state.position.y < 2.0f)) return 20;
        physx->get_body_states(states);
        if (states.size() != 2) return 21;
        RaycastQuery physxStaticOnly;
        physxStaticOnly.origin = {0.0f, 5.0f, 0.0f};
        physxStaticOnly.direction = {0.0f, -1.0f, 0.0f};
        physxStaticOnly.maxDistance = 10.0f;
        physxStaticOnly.includeDynamic = false;
        if (!physx->raycast(physxStaticOnly, hit) || hit.body != physxGroundId) return 22;
        physx->destroy_body(physxBodyId);
        const BodyId recycledBodyId = physx->create_body(physxBody);
        if (recycledBodyId != physxBodyId || !physx->is_valid(recycledBodyId)) return 23;
    }

    std::cout << "physics world tests passed (" << automaticBackend << ")\n";
    return 0;
}

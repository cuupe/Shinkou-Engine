#include "shinkou/physics/PhysicsWorld.h"
#include <cmath>
#include <iostream>
#include <vector>

int main() {
    using namespace shinkou::physics;

    PhysicsWorldConfig config;
    config.backend = PhysicsBackend::Auto;
    config.useFixedTimeStep = false;
    config.gravity = {0.0f, -9.81f, 0.0f};

    auto world = create_physics_world(config);
    if (!world || !world->is_available()) {
        std::cerr << "physics backend unavailable: " << (world ? world->last_error() : "allocation failed") << '\n';
        return 1;
    }

    BodyDesc ground;
    ground.dynamic = false;
    ground.position = {0.0f, -0.5f, 0.0f};
    ground.halfExtents = {5.0f, 0.5f, 5.0f};
    const BodyId groundId = world->create_body(ground);

    BodyDesc fallingBody;
    fallingBody.position = {0.0f, 3.0f, 0.0f};
    fallingBody.halfExtents = {0.5f, 0.5f, 0.5f};
    fallingBody.userData = 1001;
    const BodyId bodyId = world->create_body(fallingBody);
    if (groundId == InvalidBodyId || bodyId == InvalidBodyId) {
        std::cerr << "body creation failed: " << world->last_error() << '\n';
        return 2;
    }

    BodyState state;
    for (int frame = 0; frame < 60; ++frame) world->step(1.0f / 60.0f);
    if (!world->get_body_state(bodyId, state) || !std::isfinite(state.position.y) || state.position.y >= 3.0f) {
        std::cerr << "simulation did not advance the body\n";
        return 3;
    }

    RaycastQuery query;
    query.origin = {0.0f, 5.0f, 0.0f};
    query.direction = {0.0f, -1.0f, 0.0f};
    query.maxDistance = 10.0f;
    query.includeDynamic = false;
    RaycastHit hit;
    if (!world->raycast(query, hit) || hit.body != groundId) {
        std::cerr << "static ground raycast failed\n";
        return 4;
    }

    std::vector<ContactEvent> contacts;
    world->drain_contact_events(contacts);
    std::cout << "standalone physics example\n"
              << "  backend: " << world->backend_name() << '\n'
              << "  body-y: " << state.position.y << '\n'
              << "  raycast-body: " << hit.body << '\n'
              << "  contact-events: " << contacts.size() << '\n'
              << "  simulation-steps: " << world->statistics().simulationSteps << '\n';
    return 0;
}

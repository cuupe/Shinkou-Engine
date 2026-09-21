#include "shinkou/physics/PhysicsWorld.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

int main() {
    using namespace shinkou::physics;
    PhysicsWorldConfig config;
    config.backend = PhysicsBackend::PhysX;
    config.useFixedTimeStep = false;
    config.workerThreads = 0;
    auto world = create_physics_world(config);
    if (!world || world->backend() != PhysicsBackend::PhysX || !world->is_available()) {
        std::cerr << "physics benchmark requires a real PhysX backend: "
                  << (world ? world->last_error() : "allocation failed") << '\n';
        return 1;
    }

    constexpr std::size_t bodyCount = 4096;
    constexpr int iterations = 120;
    for (std::size_t index = 0; index < bodyCount; ++index) {
        BodyDesc desc;
        desc.position = {static_cast<float>(index % 64u), 2.0f + static_cast<float>(index / 64u) * 1.1f, 0.0f};
        desc.halfExtents = {0.45f, 0.45f, 0.45f};
        desc.userData = index;
        if (world->create_body(desc) == InvalidBodyId) return 2;
    }

    const auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < iterations; ++iteration) world->step(1.0f / 60.0f);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::vector<BodyId> ids;
    world->get_body_ids(ids);
    double checksum = 0.0;
    BodyState state;
    for (const BodyId id : ids) if (world->get_body_state(id, state)) checksum += state.position.y;
    const double simulatedBodies = static_cast<double>(bodyCount) * iterations;
    std::cout << "physics benchmark\n"
              << "  backend: " << world->backend_name() << "\n"
              << "  bodies: " << bodyCount << "\n"
              << "  steps/s: " << static_cast<double>(iterations) / elapsed << "\n"
              << "  body-steps/s: " << simulatedBodies / elapsed << "\n"
              << "  checksum: " << checksum << "\n";
    return std::isfinite(checksum) ? 0 : 3;
}

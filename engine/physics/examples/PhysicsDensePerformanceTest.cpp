#include "shinkou/physics/PhysicsWorld.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace shinkou::physics;

struct Options {
    std::size_t bodies{4096};
    std::size_t warmupFrames{120};
    std::size_t measuredFrames{600};
    std::uint32_t workerThreads{0};
    float timestep{1.0f / 60.0f};
};

bool parse_size(const char* value, std::size_t& output) {
    if (!value || !*value) return false;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (*end != '\0' || parsed == 0 || parsed > std::numeric_limits<std::size_t>::max()) return false;
    output = static_cast<std::size_t>(parsed);
    return true;
}

bool parse_uint(const char* value, std::uint32_t& output) {
    if (!value || !*value) return false;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (*end != '\0' || parsed > std::numeric_limits<std::uint32_t>::max()) return false;
    output = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parse_float(const char* value, float& output) {
    if (!value || !*value) return false;
    char* end = nullptr;
    const float parsed = std::strtof(value, &end);
    if (*end != '\0' || !std::isfinite(parsed) || parsed <= 0.0f) return false;
    output = parsed;
    return true;
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--bodies" && index + 1 < argc) {
            if (!parse_size(argv[++index], options.bodies)) return false;
        } else if (argument == "--warmup" && index + 1 < argc) {
            if (!parse_size(argv[++index], options.warmupFrames)) return false;
        } else if (argument == "--frames" && index + 1 < argc) {
            if (!parse_size(argv[++index], options.measuredFrames)) return false;
        } else if (argument == "--threads" && index + 1 < argc) {
            if (!parse_uint(argv[++index], options.workerThreads)) return false;
        } else if (argument == "--dt" && index + 1 < argc) {
            if (!parse_float(argv[++index], options.timestep)) return false;
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "PhysX dense performance test\n"
                      << "  --bodies N    dynamic boxes (default 4096)\n"
                      << "  --warmup N    unmeasured simulation frames (default 120)\n"
                      << "  --frames N    measured simulation frames (default 600)\n"
                      << "  --threads N   PhysX worker threads, 0 = automatic\n"
                      << "  --dt seconds  simulation timestep (default 1/60)\n";
            return false;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            return false;
        }
    }
    return options.bodies > 0 && options.measuredFrames > 0 && options.timestep > 0.0f;
}

double percentile(std::vector<double> samples, double fraction) {
    if (samples.empty()) return 0.0;
    const auto index = static_cast<std::size_t>(std::clamp(
        fraction * static_cast<double>(samples.size() - 1), 0.0,
        static_cast<double>(samples.size() - 1)));
    std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(index), samples.end());
    return samples[index];
}

bool create_dense_scene(IPhysicsWorld& world, std::size_t bodyCount) {
    constexpr float halfExtent = 0.45f;
    constexpr float spacing = 0.90f;
    const auto gridSide = static_cast<std::size_t>(std::ceil(std::cbrt(static_cast<double>(bodyCount))));
    const float horizontalExtent = std::max(4.0f,
        static_cast<float>(gridSide) * spacing * 0.60f + halfExtent);

    BodyDesc ground;
    ground.dynamic = false;
    ground.position = {0.0f, -0.5f, 0.0f};
    ground.halfExtents = {horizontalExtent, 0.5f, horizontalExtent};
    ground.shapes.push_back(ShapeDesc::box(ground.halfExtents));
    if (world.create_body(ground) == InvalidBodyId) return false;

    const PhysicsMaterialDesc material{0.7f, 0.55f, 0.12f};
    const std::size_t planeSize = gridSide * gridSide;
    const float center = static_cast<float>(gridSide - 1) * 0.5f;
    for (std::size_t index = 0; index < bodyCount; ++index) {
        const std::size_t layer = index / planeSize;
        const std::size_t planeIndex = index % planeSize;
        const std::size_t row = planeIndex / gridSide;
        const std::size_t column = planeIndex % gridSide;

        BodyDesc body;
        body.dynamic = true;
        body.position = {
            (static_cast<float>(column) - center) * spacing,
            halfExtent + 0.05f + static_cast<float>(layer) * spacing,
            (static_cast<float>(row) - center) * spacing};
        body.halfExtents = {halfExtent, halfExtent, halfExtent};
        body.mass = 1.0f;
        body.angularVelocity = {
            0.01f * static_cast<float>((index % 7u) + 1u),
            0.015f * static_cast<float>((index % 11u) + 1u),
            0.01f * static_cast<float>((index % 5u) + 1u)};
        body.userData = static_cast<std::uint64_t>(index + 1u);
        auto shape = ShapeDesc::box(body.halfExtents);
        shape.material = material;
        body.shapes.push_back(shape);
        if (world.create_body(body) == InvalidBodyId) return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) return argc > 1 ? 2 : 0;

    PhysicsWorldConfig config;
    config.backend = PhysicsBackend::PhysX;
    config.gravity = {0.0f, -9.81f, 0.0f};
    config.workerThreads = options.workerThreads;
    config.useFixedTimeStep = false;
    config.fixedTimeStep = options.timestep;
    config.maxSubSteps = 1;
    config.enableContactEvents = false;
    config.collectContactPoints = false;
    // Keep contact accounting from becoming the measurement's artificial
    // ceiling when the pile is fully settled and every layer is touching.
    config.contactEventCapacity = std::max<std::size_t>(65536u, options.bodies * 64u);

    auto world = create_physics_world(config);
    if (!world || world->backend() != PhysicsBackend::PhysX || !world->is_available()) {
        std::cerr << "DENSE_PHYSX FAIL: a real PhysX backend is required; Simple is not accepted.\n"
                  << "  reason: " << (world ? world->last_error() : "allocation failed") << '\n';
        return 3;
    }

    const auto createStart = std::chrono::steady_clock::now();
    if (!create_dense_scene(*world, options.bodies)) {
        std::cerr << "DENSE_PHYSX FAIL: scene creation failed: " << world->last_error() << '\n';
        return 4;
    }
    const double creationSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - createStart).count();

    for (std::size_t frame = 0; frame < options.warmupFrames; ++frame) world->step(options.timestep);

    std::vector<double> stepMilliseconds;
    stepMilliseconds.reserve(options.measuredFrames);
    std::uint64_t totalContactEvents = 0;
    std::size_t peakContactEvents = 0;
    const auto measureStart = std::chrono::steady_clock::now();
    for (std::size_t frame = 0; frame < options.measuredFrames; ++frame) {
        const auto stepStart = std::chrono::steady_clock::now();
        world->step(options.timestep);
        const double elapsedMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - stepStart).count();
        stepMilliseconds.push_back(elapsedMilliseconds);
        const auto contacts = world->statistics().lastContactEventCount;
        totalContactEvents += contacts;
        peakContactEvents = std::max(peakContactEvents, contacts);
    }
    const double measureSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - measureStart).count();

    std::vector<BodyId> bodyIds;
    world->get_body_ids(bodyIds);
    double checksum = 0.0;
    BodyState state;
    for (const BodyId body : bodyIds) {
        if (!world->get_body_state(body, state)) {
            std::cerr << "DENSE_PHYSX FAIL: state query failed for body " << body << '\n';
            return 5;
        }
        checksum += static_cast<double>(state.position.x) * 0.31;
        checksum += static_cast<double>(state.position.y) * 0.47;
        checksum += static_cast<double>(state.position.z) * 0.19;
    }
    if (!std::isfinite(checksum) || bodyIds.size() != options.bodies + 1u) {
        std::cerr << "DENSE_PHYSX FAIL: invalid result bodyCount=" << bodyIds.size()
                  << " checksum=" << checksum << '\n';
        return 6;
    }

    const double stepsPerSecond = static_cast<double>(options.measuredFrames) / measureSeconds;
    const double bodyStepsPerSecond = static_cast<double>(options.bodies) * stepsPerSecond;
    const auto& statistics = world->statistics();
    std::cout << std::fixed << std::setprecision(3)
              << "DENSE_PHYSX PASS\n"
              << "  backend: " << world->backend_name() << '\n'
              << "  bodies: " << options.bodies << " dynamic + 1 ground\n"
              << "  worker-threads: " << (options.workerThreads == 0 ? "auto" : std::to_string(options.workerThreads)) << '\n'
              << "  creation-ms: " << creationSeconds * 1000.0 << '\n'
              << "  measured-frames: " << options.measuredFrames << '\n'
              << "  avg-step-ms: " << (measureSeconds * 1000.0 / static_cast<double>(options.measuredFrames)) << '\n'
              << "  p50-step-ms: " << percentile(stepMilliseconds, 0.50) << '\n'
              << "  p95-step-ms: " << percentile(stepMilliseconds, 0.95) << '\n'
              << "  p99-step-ms: " << percentile(stepMilliseconds, 0.99) << '\n'
              << "  max-step-ms: " << *std::max_element(stepMilliseconds.begin(), stepMilliseconds.end()) << '\n'
              << "  steps-per-second: " << stepsPerSecond << '\n'
              << "  body-steps-per-second: " << bodyStepsPerSecond << '\n'
              << "  avg-contact-events: " << (static_cast<double>(totalContactEvents) / static_cast<double>(options.measuredFrames)) << '\n'
              << "  peak-contact-events: " << peakContactEvents << '\n'
              << "  active-dynamics: " << statistics.activeDynamicBodyCount << '\n'
              << "  checksum: " << checksum << '\n';
    return 0;
}

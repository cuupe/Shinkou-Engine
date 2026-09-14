#include "shinkou/World.h"

#include <chrono>
#include <cstddef>
#include <iostream>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t ObjectCount = 50000;
constexpr std::size_t FrameCount = 240;
constexpr shinkou::Seconds DeltaSeconds = 1.0f / 60.0f;

struct Position {
    float value{0.0f};
};

struct Velocity {
    float value{1.0f};
};

struct RegularMotionComponent final : shinkou::Component {
    Position position{};
    Velocity velocity{};

    std::string_view type_name() const noexcept override { return "RegularMotion"; }

protected:
    void on_update(shinkou::Seconds dt) override {
        position.value += velocity.value * dt;
    }
};

struct BatchMotionSystem final : shinkou::EcsSystem {
protected:
    void on_update(shinkou::Seconds dt) override {
        world().ecs().view<Position, Velocity>().each(
            [dt](auto, Position& position, Velocity& velocity) {
                position.value += velocity.value * dt;
            });
    }
};

struct BenchmarkResult {
    std::size_t objectCount{0};
    double updateMicroseconds{0.0};
    double checksum{0.0};
};

template<class Setup, class Checksum>
BenchmarkResult measure(Setup&& setup, Checksum&& checksum) {
    shinkou::World world;
    world.reserve(ObjectCount);
    setup(world);

    // Warm up caches and branch state. Setup and warmup are excluded from
    // the measured update cost.
    for (std::size_t frame = 0; frame < 8; ++frame) world.update(DeltaSeconds);
    const auto start = Clock::now();
    for (std::size_t frame = 0; frame < FrameCount; ++frame) world.update(DeltaSeconds);
    const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();

    return {world.object_count(), elapsed / static_cast<double>(FrameCount), checksum(world)};
}
}

int main() {
    const auto regularObjects = measure(
        [](shinkou::World& world) {
            for (std::size_t index = 0; index < ObjectCount; ++index)
                world.create_object().add_component<RegularMotionComponent>();
        },
        [](const shinkou::World& world) {
            double result = 0.0;
            world.each_game_object([&](const shinkou::GameObject& object) {
                const auto* motion = object.get_component<RegularMotionComponent>();
                if (motion) result += motion->position.value;
            });
            return result;
        });

    const auto ecsBatch = measure(
        [](shinkou::World& world) {
            world.add_ecs_system<BatchMotionSystem>();
            for (std::size_t index = 0; index < ObjectCount; ++index) {
                auto& object = world.create_object({}, shinkou::ObjectStorage::Ecs);
                object.add_ecs_component<Position>();
                object.add_ecs_component<Velocity>();
            }
        },
        [](const shinkou::World& world) {
            double result = 0.0;
            world.ecs().view<Position, Velocity>().each(
                [&](auto, const Position& position, const Velocity&) { result += position.value; });
            return result;
        });

    const auto speedup = regularObjects.updateMicroseconds / ecsBatch.updateMicroseconds;
    std::cout << "objects=" << regularObjects.objectCount
              << " regular_component_update_us=" << regularObjects.updateMicroseconds
              << " ecs_batch_update_us=" << ecsBatch.updateMicroseconds
              << " ecs_speedup=" << speedup
              << " regular_checksum=" << regularObjects.checksum
              << " ecs_checksum=" << ecsBatch.checksum << '\n';
    return 0;
}

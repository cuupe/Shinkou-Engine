// Chunk-store benchmark: the engine's DOTS-style storage against the regular
// object lane and a hand-written EnTT reference, plus chunk-parallel dispatch.
//
// The goal is to expose layout costs, not to benchmark the whole frame:
//   * batch create (one archetype hit) vs create + two migrations
//   * steady-state update through a cached query
//   * the same update distributed over worker threads, one job per chunk
//   * destroy (swap-back + chunk recycling)
#include "shinkou/World.h"
#include "shinkou/ecs/EntityStore.h"
#include "shinkou/ecs/Parallel.h"
#include "shinkou/jobs/JobSystem.h"

#include <entt/entt.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kEntities = 100000;
constexpr int kFrames = 240;
constexpr int kWarmup = 16;
constexpr float kDt = 1.0f / 60.0f;

struct Position {
    float x{0.0f}, y{0.0f}, z{0.0f};
};
struct Velocity {
    float x{1.0f}, y{2.0f}, z{3.0f};
};

struct MotionComponent final : shinkou::Component {
    Position position{};
    Velocity velocity{};
    std::string_view type_name() const noexcept override {
        return "Motion";
    }
    void on_update(shinkou::Seconds dt) override {
        position.x += velocity.x * dt;
        position.y += velocity.y * dt;
        position.z += velocity.z * dt;
    }
};

template <class Body> double milliseconds(Body &&body) {
    const auto start = Clock::now();
    body();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void update_position(Position &position, const Velocity &velocity, float dt) {
    position.x += velocity.x * dt;
    position.y += velocity.y * dt;
    position.z += velocity.z * dt;
}

// Hand-vectorisable floor: same math, plain contiguous arrays, no ECS.
void soa_update(Position *__restrict positions, const Velocity *__restrict velocities,
                std::uint32_t count, float dt) {
    for (std::uint32_t index = 0; index < count; ++index)
        update_position(positions[index], velocities[index], dt);
}

} // namespace

int main() {
    std::printf("entities=%d frames=%d\n", kEntities, kFrames);

    double checksum = 0.0;

    // ---- regular object lane ---------------------------------------------
    {
        shinkou::World world;
        world.reserve(kEntities);
        const double build = milliseconds([&] {
            for (int index = 0; index < kEntities; ++index) {
                auto &object = world.create_object();
                auto *motion = object.add_component<MotionComponent>();
                motion->position.x = static_cast<float>(index);
            }
        });
        for (int frame = 0; frame < kWarmup; ++frame)
            world.update(kDt);
        const double update = milliseconds([&] {
            for (int frame = 0; frame < kFrames; ++frame)
                world.update(kDt);
        });
        world.each_game_object([&](const shinkou::GameObject &object) {
            if (const auto *motion = object.get_component<MotionComponent>())
                checksum += motion->position.x;
        });
        std::printf("regular_oop      build_ms=%8.2f  update_us/f=%9.2f\n", build,
                    update * 1000.0 / kFrames);
    }

    // ---- EnTT reference ---------------------------------------------------
    {
        entt::registry registry;
        const double build = milliseconds([&] {
            for (int index = 0; index < kEntities; ++index) {
                const auto entity = registry.create();
                registry.emplace<Position>(entity, Position{static_cast<float>(index), 0.0f, 0.0f});
                registry.emplace<Velocity>(entity, Velocity{});
            }
        });
        auto frame = [&] {
            registry.view<Position, Velocity>().each([](Position &position, Velocity &velocity) {
                update_position(position, velocity, kDt);
            });
        };
        for (int index = 0; index < kWarmup; ++index)
            frame();
        const double update = milliseconds([&] {
            for (int index = 0; index < kFrames; ++index)
                frame();
        });
        registry.view<Position>().each([&](const Position &position) { checksum += position.x; });
        std::printf("entt_sparse_set  build_ms=%8.2f  update_us/f=%9.2f\n", build,
                    update * 1000.0 / kFrames);
    }

    // ---- chunk store: create + two archetype migrations -------------------
    {
        shinkou::World world;
        auto &store = world.ecs().store();
        std::vector<shinkou::Entity> entities;
        entities.reserve(kEntities);
        const double build = milliseconds([&] {
            for (int index = 0; index < kEntities; ++index) {
                const auto entity = world.ecs().create();
                world.ecs().emplace<Position>(entity,
                                              Position{static_cast<float>(index), 0.0f, 0.0f});
                world.ecs().emplace<Velocity>(entity, Velocity{});
                entities.push_back(entity);
            }
        });
        auto &query = store.query<Position, Velocity>();
        auto frame = [&] {
            query.each<Position, Velocity>(
                [](shinkou::Entity, Position &position, const Velocity &velocity) {
                    update_position(position, velocity, kDt);
                });
        };
        for (int index = 0; index < kWarmup; ++index)
            frame();
        const double update = milliseconds([&] {
            for (int index = 0; index < kFrames; ++index)
                frame();
        });
        query.each<Position>([](shinkou::Entity, const Position &position) { (void)position; });
        std::printf("chunk_migrate    build_ms=%8.2f  update_us/f=%9.2f\n", build,
                    update * 1000.0 / kFrames);
        std::printf("  chunks=%zu entities_per_chunk=%u stride=%uB\n", query.chunks().size(),
                    query.chunks().empty() ? 0u : query.chunks().front()->capacity,
                    query.chunks().empty() ? 0u
                                           : query.chunks().front()->archetype->entity_stride());
    }

    // ---- chunk store: one-shot archetype create ---------------------------
    {
        shinkou::ecs::EntityStore store;
        auto &archetype = store.find_or_create_archetype(
            {shinkou::ecs::type_id<Position>(), shinkou::ecs::type_id<Velocity>()});
        std::vector<shinkou::Entity> entities;
        entities.reserve(kEntities);
        const double build = milliseconds([&] {
            for (int index = 0; index < kEntities; ++index) {
                const auto entity = store.create(archetype);
                auto &position = store.get<Position>(entity);
                position.x = static_cast<float>(index);
                entities.push_back(entity);
            }
        });
        auto &query = store.query<Position, Velocity>();
        auto frame = [&] {
            query.each<Position, Velocity>(
                [](shinkou::Entity, Position &position, const Velocity &velocity) {
                    update_position(position, velocity, kDt);
                });
        };
        for (int index = 0; index < kWarmup; ++index)
            frame();
        const double update = milliseconds([&] {
            for (int index = 0; index < kFrames; ++index)
                frame();
        });
        query.each<Position>(
            [&](shinkou::Entity, const Position &position) { checksum += position.x; });

        // Parallel dispatch: one work item per chunk (or per grain chunks).
        shinkou::jobs::JobSystem jobs(0);
        const auto &chunks = query.chunks();
        std::vector<double> parallelTimes;
        for (const std::size_t grain : {std::size_t{1}, std::size_t{8}, std::size_t{32}}) {
            auto parallel_frame = [&] {
                shinkou::ecs::parallel_chunks<Position, Velocity>(
                    jobs, query, grain,
                    [](shinkou::ecs::Chunk &chunk, Position *positions, Velocity *velocities) {
                        for (std::uint32_t row = 0; row < chunk.count; ++row)
                            update_position(positions[row], velocities[row], kDt);
                    });
            };
            for (int index = 0; index < kWarmup; ++index)
                parallel_frame();
            parallelTimes.push_back(milliseconds([&] {
                                        for (int index = 0; index < kFrames; ++index)
                                            parallel_frame();
                                    }) *
                                    1000.0 / kFrames);
        }

        const double destroy = milliseconds([&] {
            for (const auto entity : entities)
                store.destroy(entity);
        });
        std::printf("chunk_batch      build_ms=%8.2f  update_us/f=%9.2f  destroy_ms=%8.2f\n", build,
                    update * 1000.0 / kFrames, destroy);
        std::printf("  parallel_us/f  grain1=%8.2f  grain8=%8.2f  grain32=%8.2f  workers=%zu\n",
                    parallelTimes[0], parallelTimes[1], parallelTimes[2], jobs.worker_count());
        std::printf("  chunks=%zu entities_per_chunk=%u resident_chunks=%zu\n", chunks.size(),
                    chunks.empty() ? 0u : chunks.front()->capacity, store.chunk_count());
    }

    // ---- chunk transform pipeline (depth-ordered, optionally parallel) ----
    {
        const auto run = [](bool parallel, bool transformOnly) {
            shinkou::jobs::JobSystem jobs(0);
            shinkou::World world;
            world.reserve(kEntities);
            for (int index = 0; index < kEntities; ++index) {
                auto &object = world.create_object({}, shinkou::ObjectStorage::Ecs);
                if (auto *transform =
                        object.get_component<shinkou::components::TransformComponent>())
                    transform->local.position.x = static_cast<float>(index);
                if (parallel)
                    world.set_job_system(&jobs);
            }
            for (int index = 0; index < kWarmup; ++index) {
                if (transformOnly)
                    world.refresh_transforms();
                else
                    world.update(kDt);
            }
            const double elapsed = milliseconds([&] {
                for (int index = 0; index < kFrames; ++index) {
                    if (transformOnly)
                        world.refresh_transforms();
                    else
                        world.update(kDt);
                }
            });
            return elapsed * 1000.0 / kFrames;
        };
        const double serialTransform = run(false, true);
        const double parallelTransform = run(true, true);
        const double serialUpdate = run(false, false);
        const double parallelUpdate = run(true, false);
        std::printf("transform_pipeline transform_us/f serial=%.2f parallel=%.2f speedup=%.2fx\n",
                    serialTransform, parallelTransform, serialTransform / parallelTransform);
        std::printf(
            "                   world_update_us/f serial=%.2f parallel=%.2f speedup=%.2fx\n",
            serialUpdate, parallelUpdate, serialUpdate / parallelUpdate);

        // Animated scene: every frame edits every transform, so every object
        // enters the dirty list.  Only the pipeline time is accumulated; the
        // authoring edits themselves are benchmark scaffolding, not storage.
        const auto runAnimated = [](bool parallel) {
            shinkou::jobs::JobSystem jobs(0);
            shinkou::World world;
            world.reserve(kEntities);
            for (int index = 0; index < kEntities; ++index) {
                auto &object = world.create_object({}, shinkou::ObjectStorage::Ecs);
                if (auto *transform =
                        object.get_component<shinkou::components::TransformComponent>())
                    transform->local.position.x = static_cast<float>(index);
                if (parallel)
                    world.set_job_system(&jobs);
            }
            float value = 0.0f;
            auto editAll = [&] {
                world.each_game_object([&](shinkou::GameObject &object) {
                    if (auto *transform = object.transform_component())
                        transform->set_position({value, 0.0f, 0.0f});
                });
                value += 1.0f;
            };
            for (int index = 0; index < kWarmup; ++index) {
                editAll();
                world.refresh_transforms();
            }
            double total = 0.0;
            for (int index = 0; index < kFrames; ++index) {
                editAll();
                const auto start = Clock::now();
                world.refresh_transforms();
                total += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            }
            return total * 1000.0 / kFrames;
        };
        const double animatedSerial = runAnimated(false);
        const double animatedParallel = runAnimated(true);
        std::printf("transform_animated us/f serial=%.2f parallel=%.2f speedup=%.2fx\n",
                    animatedSerial, animatedParallel, animatedSerial / animatedParallel);
    }

    // ---- raw SoA floor ----------------------------------------------------
    {
        std::vector<Position> positions(static_cast<std::size_t>(kEntities));
        std::vector<Velocity> velocities(static_cast<std::size_t>(kEntities));
        for (int index = 0; index < kEntities; ++index)
            positions[static_cast<std::size_t>(index)].x = static_cast<float>(index);
        auto frame = [&] {
            soa_update(positions.data(), velocities.data(), static_cast<std::uint32_t>(kEntities),
                       kDt);
        };
        for (int index = 0; index < kWarmup; ++index)
            frame();
        const double update = milliseconds([&] {
            for (int index = 0; index < kFrames; ++index)
                frame();
        });
        std::printf("raw_soa_floor    update_us/f=%9.2f\n", update * 1000.0 / kFrames);
    }

    std::printf("checksum=%.1f\n", checksum);
    return 0;
}

// Focused ECS runtime costs. Keep construction outside timed steady-state loops;
// print checksums so the compiler cannot discard the work.
#include "shinkou/ecs/ChunkPipeline.h"
#include "shinkou/ecs/CommandBuffer.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {
struct Position {
    float x{}, y{}, z{};
};
struct Velocity {
    float x{1}, y{2}, z{3};
};
struct Health {
    int value{100};
};
using Clock = std::chrono::steady_clock;
template <class F> double ms(F &&fn) {
    const auto start = Clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Result {
    float value{};
};
double pipeline_benchmark(bool parallel, double &checksum) {
    shinkou::ecs::EntityStore store;
    store.reserve(100000);
    auto &shape = store.archetype<Position, Velocity, Result>();
    for (int i = 0; i < 100000; ++i)
        store.create(shape);
    shinkou::ecs::ChunkPipeline pipeline;
    // A whole system per task isolates cross-system scheduling from chunk
    // fan-out. Both independent producers must precede the result consumer.
    pipeline.add<Position>(
        store.query<Position>(),
        [](auto &c, Position *p) {
            for (unsigned i = 0; i < c.count; ++i)
                for (int n = 0; n < 32; ++n)
                    p[i].x = std::sin(p[i].x + .01f);
        },
        100000);
    pipeline.add<Velocity>(
        store.query<Velocity>(),
        [](auto &c, Velocity *v) {
            for (unsigned i = 0; i < c.count; ++i)
                for (int n = 0; n < 32; ++n)
                    v[i].x = std::cos(v[i].x + .01f);
        },
        100000);
    pipeline.add<const Position, const Velocity, Result>(
        store.query<Position, Velocity, Result>(),
        [](auto &c, const Position *p, const Velocity *v, Result *r) {
            for (unsigned i = 0; i < c.count; ++i)
                r[i].value = p[i].x + v[i].x;
        },
        100000);
    shinkou::jobs::JobSystem jobs(4);
    auto *pool = parallel ? &jobs : nullptr;
    for (int i = 0; i < 4; ++i)
        pipeline.run(pool);
    const double elapsed = ms([&] {
        for (int i = 0; i < 10; ++i)
            pipeline.run(pool);
    });
    store.query<Result>().each<Result>([&](auto, const Result &r) { checksum += r.value; });
    return elapsed / 10;
}
} // namespace

int main(int argc, char **argv) {
    constexpr int count = 100000, rounds = 20;
    shinkou::ecs::EntityStore store;
    store.reserve(count);
    auto &shape = store.archetype<Position, Velocity>();
    std::vector<shinkou::Entity> entities;
    entities.reserve(count);
    for (int i = 0; i < count; ++i)
        entities.push_back(store.create(shape));
    // Warm transition edges and command storage before measurement.
    for (auto e : entities)
        store.emplace<Health>(e);
    for (auto e : entities)
        store.remove<Health>(e);
    const double migrate = ms([&] {
        for (int r = 0; r < rounds; ++r) {
            for (auto e : entities)
                store.emplace<Health>(e);
            for (auto e : entities)
                store.remove<Health>(e);
        }
    });
    shinkou::ecs::CommandBuffer buffer(store);
    for (auto e : entities)
        buffer.set_component<Position>(e, Position{1, 2, 3});
    buffer.playback();
    double record = 0, playback = 0;
    for (int r = 0; r < rounds; ++r) {
        record += ms([&] {
            for (auto e : entities)
                buffer.set_component<Position>(e, Position{float(r), 2, 3});
        });
        playback += ms([&] { buffer.playback(); });
    }
    double checksum = 0;
    auto &query = store.query<Position, Velocity>();
    const double update = ms([&] {
        for (int r = 0; r < 1000; ++r)
            query.each<Position, Velocity>([](shinkou::Entity, Position &p, const Velocity &v) {
                p.x += v.x * .01f;
                p.y += v.y * .01f;
                p.z += v.z * .01f;
            });
    });
    query.each<Position>([&](shinkou::Entity, Position &p) { checksum += p.x; });
    const double destroy = ms([&] {
        for (auto e : entities)
            store.destroy(e);
    });
    std::printf("entities=%d rounds=%d migrate_ms/round=%.3f record_ms/round=%.3f "
                "playback_ms/round=%.3f update_us/frame=%.3f destroy_ms=%.3f checksum=%.3f\n",
                count, rounds, migrate / rounds, record / rounds, playback / rounds,
                update * 1000 / 1000, destroy, checksum);
    if (argc > 1 && std::string_view(argv[1]) == "--storage-only")
        return 0;
    double serialChecksum = 0, parallelChecksum = 0;
    const auto serial = pipeline_benchmark(false, serialChecksum);
    const auto parallel = pipeline_benchmark(true, parallelChecksum);
    std::printf("pipeline_ms/frame serial=%.3f parallel=%.3f speedup=%.2fx checksum_serial=%.6f "
                "checksum_parallel=%.6f\n",
                serial, parallel, serial / parallel, serialChecksum, parallelChecksum);
    return serialChecksum == parallelChecksum ? 0 : 1;
}

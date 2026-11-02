#include "shinkou/ecs/ChunkPipeline.h"
#include "shinkou/ecs/CommandBuffer.h"
#include "shinkou/ecs/EntityStore.h"
#include "shinkou/ecs/Parallel.h"
#include "shinkou/ecs/Registry.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
struct Position { int value{}; };
struct Velocity { int value{2}; };
struct Result { int value{}; };
struct Tracked {
    static inline int alive = 0;
    static inline int copies = 0;
    std::string text;
    Tracked() { ++alive; }
    explicit Tracked(std::string s) : text(std::move(s)) { ++alive; }
    Tracked(const Tracked& t) : text(t.text) { ++alive; ++copies; }
    ~Tracked() { --alive; }
};
struct alignas(256) Aligned {
    std::uint64_t values[32]{};
    Aligned() {
        if (reinterpret_cast<std::uintptr_t>(this) % alignof(Aligned))
            throw std::runtime_error("misaligned arena payload");
    }
};
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void storage_lifetimes() {
    shinkou::ecs::EntityStore store;
    auto& shape = store.archetype<Tracked, Position>();
    std::vector<shinkou::Entity> entities;
    for (int i = 0; i < 3000; ++i) {
        auto e = store.create(shape);
        store.get<Tracked>(e).text = std::to_string(i);
        store.get<Position>(e).value = i;
        entities.push_back(e);
    }
    // Delete non-last slots, then delete chunks from the tail. Both the live
    // source element and the swapped chunk index must be handled correctly.
    for (int i = 0; i < 3000; i += 2) store.destroy(entities[i]);
    check(Tracked::alive == 1500, "swap-back leaked the old last component");
    for (int i = 2999; i >= 1; i -= 2) {
        check(store.get<Tracked>(entities[i]).text == std::to_string(i), "swap-back corrupted payload");
        store.emplace<Velocity>(entities[i]);
        store.remove<Velocity>(entities[i]);
        store.destroy(entities[i]);
    }
    check(Tracked::alive == 0, "migration/retirement leaked components");
}

void command_lifetimes() {
    shinkou::ecs::EntityStore store;
    const auto e = store.create();
    shinkou::ecs::CommandBuffer commands(store);
    commands.reserve(10000);
    for (int i = 0; i < 10000; ++i) commands.add_component<Tracked>(e, std::to_string(i));
    check(Tracked::alive == 10000, "recorded payload ownership");
    const auto capacity = commands.payload_capacity();
    commands.playback();
    check(Tracked::alive == 1 && store.get<Tracked>(e).text == "9999", "payload destructor/playback");
    for (int i = 0; i < 10000; ++i) commands.add_component<Tracked>(e, "reused");
    check(commands.payload_capacity() == capacity, "arena pages not reused");
    commands.clear();
    check(Tracked::alive == 1, "clear must destroy unplayed payloads");
    // Verify over-aligned placement without inserting into a chunk (the chunk
    // allocator only promises 64-byte alignment).
    for (int i = 0; i < 128; ++i) commands.add_component<Aligned>(e);
    commands.clear();
    store.clear();
    check(Tracked::alive == 0, "store teardown leaked payloads");
}

void dependency_graph(std::size_t workers) {
    shinkou::jobs::JobSystem jobs(workers);
    shinkou::ecs::EntityStore store;
    auto& shape = store.archetype<Position, Velocity, Result>();
    std::vector<shinkou::Entity> entities;
    for (int i = 0; i < 20000; ++i) entities.push_back(store.create(shape));
    auto& p = store.query<Position>();
    auto& v = store.query<Velocity>();
    auto& all = store.query<Position, Velocity, Result>();
    shinkou::ecs::ChunkPipeline pipeline;
    pipeline.add<Position>(p, [](auto& c, Position* values) {
        for (unsigned i = 0; i < c.count; ++i) ++values[i].value;
    });
    pipeline.add<Velocity>(v, [](auto& c, Velocity* values) {
        for (unsigned i = 0; i < c.count; ++i) ++values[i].value;
    });
    pipeline.add<const Position, const Velocity, Result>(all,
        [](auto& c, const Position* p, const Velocity* v, Result* result) {
            for (unsigned i = 0; i < c.count; ++i) result[i].value = p[i].value + v[i].value;
        });
    // Write-after-read must wait for the result system, not just the first writer.
    pipeline.add<Position>(p, [](auto& c, Position* p) {
        for (unsigned i = 0; i < c.count; ++i) p[i].value += 10;
    });
    for (int frame = 1; frame <= 30; ++frame) {
        pipeline.run(frame % 2 ? &jobs : nullptr);
        for (auto e : entities) {
            check(store.get<Position>(e).value == frame * 11, "write-after-read dependency");
            check(store.get<Result>(e).value == frame * 12 - 8, "diamond dependency");
        }
    }
    // Query cache preparation must observe chunks allocated after graph compilation.
    const auto later = store.create(shape);
    pipeline.run(&jobs);
    check(store.get<Result>(later).value == 4, "new entity missing from pipeline query");
    pipeline.clear();
    bool fail = true;
    pipeline.add<Position>(p, [&](auto&, Position*) { if (fail) throw std::runtime_error("kernel failure"); });
    pipeline.add<Position>(p, [](auto&, Position*) {});
    pipeline.add<const Velocity>(v, [](auto&, const Velocity*) {});
    bool caught = false;
    try { pipeline.run(&jobs); } catch (const std::runtime_error&) { caught = true; }
    check(caught, "kernel exception not delivered to caller");
    fail = false;
    pipeline.run(&jobs);
    check(jobs.take_exception() == nullptr, "pipeline leaked an exception to the shared pool");
}

void independent_readers() {
    shinkou::jobs::JobSystem jobs(4);
    shinkou::ecs::EntityStore store;
    store.create(store.archetype<Position>());
    auto& query = store.query<Position>();
    shinkou::ecs::ChunkPipeline pipeline;
    std::atomic<int> entered{0};
    auto reader = [&](auto&, const Position*) {
        entered.fetch_add(1);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (entered.load() != 2) {
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("readers serialized");
            std::this_thread::yield();
        }
    };
    pipeline.add<const Position>(query, reader);
    pipeline.add<const Position>(query, reader);
    pipeline.run(&jobs);
}

void parallel_commands() {
    shinkou::jobs::JobSystem jobs(4);
    shinkou::ecs::EntityStore store;
    auto& shape = store.archetype<Position>();
    for (int i = 0; i < 10000; ++i) store.get<Position>(store.create(shape)).value = i;
    auto& query = store.query<Position>();
    shinkou::ecs::CommandBufferGroup commands(store);
    const auto destination = store.create(store.archetype<Result>());
    std::size_t created = 0;
    for (int frame = 0; frame < 3; ++frame) {
        const auto expectedLast = query.chunks().back()->entities()[0].id;
        const auto sourceChunks = query.chunks().size();
        shinkou::ecs::parallel_chunks_recorded<Position>(jobs, query, 2, commands,
            [destination](auto& chunk, auto& buffer, Position* p) {
                for (unsigned i = 0; i < chunk.count; ++i)
                    buffer.template set_component<Result>(chunk.entities()[i], Result{p[i].value * 2});
                const auto child = buffer.create_entity();
                buffer.template add_component<Velocity>(child);
                buffer.template set_component<Result>(destination, Result{int(chunk.entities()[0].id)});
            });
        commands.playback();
        created += sourceChunks;
        check(store.get<Result>(destination).value == int(expectedLast), "partition playback order changed");
        check(store.query<Velocity>().entity_count() == created, "partition placeholders not resolved");
        check(store.query<Position, Result>().entity_count() == 10000, "parallel commands lost entities");
        store.query<Position, Result>().each<Position, Result>([](auto, auto& p, auto& r) {
            check(r.value == p.value * 2, "parallel command payload corrupted");
        });
    }
    bool caught = false;
    try {
        shinkou::ecs::parallel_chunks_recorded<Position>(jobs, query, 1, commands,
            [](auto&, auto& buffer, Position*) { buffer.create_entity(); throw std::runtime_error("record failure"); });
    } catch (const std::runtime_error&) { caught = true; }
    check(caught, "recording exception not propagated");
    commands.prepare(1); // Failed recording must have dropped every partition.
}

void bulk_entities() {
    shinkou::ecs::Registry registry;
    auto& store = registry.store();
    auto& shape = store.archetype<Position, Velocity, Tracked>();
    std::vector<shinkou::Entity> first(17), batch(12345);
    registry.create_batch(shape, first.data(), first.size());
    for (std::size_t i = 0; i < first.size(); i += 2) registry.destroy(first[i]);
    registry.create_batch(shape, batch.data(), batch.size());
    check(Tracked::alive == 12353, "batch creation did not preserve partial chunks");
    for (auto e : batch) {
        check(store.valid(e), "batch entity not live");
        check(store.get<Position>(e).value == 0 && store.get<Velocity>(e).value == 2,
              "batch component defaults differ from scalar creation");
        check(store.get<Tracked>(e).text.empty(), "nontrivial batch defaults");
    }
    registry.create_batch(shape, nullptr, 0);
    auto& query = store.query<Position, Velocity>();
    check(query.entity_count() == 12353, "batch query missed entities");
    int observed = 0;
    registry.set_destroy_observer(&observed, [](void* state, shinkou::Entity) noexcept {
        ++*static_cast<int*>(state);
    });
    const auto copies = Tracked::copies;
    registry.clear();
    check(observed == 12353 && Tracked::alive == 0, "bulk clear observer/destructor lifetime");
    check(Tracked::copies == copies, "bulk clear must not copy dying components");
    check(shape.size() == 0 && store.chunk_count() == 0, "bulk clear left live typed chunks");
    check(query.entity_count() == 0, "cached query retained cleared chunks");
    std::vector<shinkou::Entity> reused(batch.size());
    registry.create_batch(shape, reused.data(), reused.size());
    for (auto old : batch) check(!store.valid(old), "pre-clear handle became valid after batch reuse");
    check(query.entity_count() == reused.size(), "query missed recycled batch chunks");
    registry.clear();
    check(Tracked::alive == 0, "repeated bulk clear leaked components");
}

void incremental_queries() {
    shinkou::ecs::EntityStore store;
    auto& all = store.query<>();
    auto& withoutPosition = store.query_with({}, {shinkou::ecs::type_id<Position>()});
    auto& positions = store.query<Position>();
    check(all.entity_count() == 0 && withoutPosition.entity_count() == 0, "initial queries not empty");
    std::vector<shinkou::Entity> empty(5000);
    store.create_batch(store.empty_archetype(), empty.data(), empty.size());
    check(all.entity_count() == 5000 && withoutPosition.entity_count() == 5000,
          "query omitted empty-archetype entities");
    const auto p = store.create(store.archetype<Position>());
    check(positions.entity_count() == 1 && all.entity_count() == 5001, "new matching archetype not scanned");
    store.create(store.archetype<Velocity>());
    check(withoutPosition.entity_count() == 5001 && positions.entity_count() == 1,
          "new unrelated archetype invalidated query signature");
    store.emplace<Velocity>(p);
    check(positions.entity_count() == 1, "new combined archetype missed");
    store.clear();
    check(store.chunk_count() == 1, "empty archetype should retain only one spare chunk");
    check(all.entity_count() == 0 && positions.entity_count() == 0 && withoutPosition.entity_count() == 0,
          "clear left cached query data");
    store.create(store.archetype<Position, Result>());
    check(positions.entity_count() == 1 && all.entity_count() == 1,
          "incremental query failed after clear and new archetype");
    for (auto e : empty) check(!store.valid(e), "empty batch handle revived after clear");
}
}

int main() {
    try {
        storage_lifetimes();
        command_lifetimes();
        dependency_graph(1);
        dependency_graph(4);
        independent_readers();
        parallel_commands();
        bulk_entities();
        incremental_queries();
        std::puts("ecs runtime tests passed");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}

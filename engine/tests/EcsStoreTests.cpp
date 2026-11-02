// Contract tests for the DOTS-style chunk store: entity handles, archetype
// migration, chunk swap-back, cached queries, deferred structural changes.
#include "shinkou/ecs/CommandBuffer.h"
#include "shinkou/ecs/EntityStore.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using shinkou::Entity;
using shinkou::ecs::EntityStore;

struct Position {
    float x{0.0f}, y{0.0f}, z{0.0f};
};
struct Velocity {
    float x{0.0f}, y{0.0f}, z{0.0f};
};
struct Health {
    std::int32_t value{0};
};
struct Tag {};

struct Tracked {
    static inline int alive{0};
    static inline int copies{0};
    std::string text;
    Tracked() { ++alive; }
    explicit Tracked(std::string value) : text(std::move(value)) { ++alive; }
    Tracked(const Tracked& other) : text(other.text) {
        ++alive;
        ++copies;
    }
    Tracked& operator=(const Tracked& other) {
        text = other.text;
        return *this;
    }
    ~Tracked() { --alive; }
};

int fail(const char* what, int code) {
    std::printf("FAIL %s (code %d)\n", what, code);
    return code;
}

#define CHECK(condition, code, what) \
    do { if (!(condition)) return fail(what, code); } while (false)

} // namespace

int main() {
    CHECK(sizeof(Entity) == 8u, 1, "Entity must stay an 8-byte handle");

    EntityStore store;

    // ---- empty entities ---------------------------------------------------
    const auto a = store.create();
    const auto b = store.create();
    CHECK(store.valid(a) && store.valid(b), 2, "created handles valid");
    CHECK(a.id != b.id, 3, "distinct indices");
    CHECK(store.alive_count() == 2u, 4, "alive count");
    CHECK(store.locate(a).chunk != nullptr, 5, "empty entity still has a chunk slot");
    CHECK(store.locate(a).chunk->archetype->types().empty(), 6, "empty archetype");

    // ---- archetype migration keeps shared data ----------------------------
    store.emplace<Position>(a, Position{1.0f, 2.0f, 3.0f});
    CHECK(store.has<Position>(a) && !store.has<Velocity>(a), 7, "single component");
    const auto& positionArchetype = store.find_or_create_archetype({shinkou::ecs::type_id<Position>()});
    CHECK(store.locate(a).chunk->archetype == &positionArchetype, 8, "archetype identity");

    store.emplace<Velocity>(b, Velocity{0.0f, 0.0f, 0.0f});
    CHECK(store.get<Position>(a).x == 1.0f, 9, "value survives first add");
    store.emplace<Velocity>(a, Velocity{4.0f, 5.0f, 6.0f});
    CHECK(store.get<Position>(a).y == 2.0f, 10, "value survives second add");
    CHECK(store.get<Velocity>(a).z == 6.0f, 11, "new component value");
    CHECK(store.has<Tag>(a) == false, 12, "tag absent");

    store.emplace<Tag>(a);
    CHECK(store.has<Tag>(a), 13, "tag component present");
    CHECK(store.locate(a).chunk->archetype->offset(shinkou::ecs::type_id<Tag>()) == 0u, 14,
          "tag components occupy no chunk storage");

    // ---- component pointers are chunk-relative, values are per slot --------
    auto& typedA = store.get<Position>(a);
    typedA.x = 42.0f;
    CHECK(store.get<Position>(a).x == 42.0f, 15, "mutable reference writes the chunk slot");

    // ---- swap-back on destroy --------------------------------------------
    std::vector<Entity> crowd;
    for (int index = 0; index < 100; ++index) {
        const auto entity = store.create();
        store.emplace<Position>(entity, Position{static_cast<float>(index), 0.0f, 0.0f});
        store.emplace<Health>(entity, Health{index});
        crowd.push_back(entity);
    }
    const auto victim = crowd[10];
    const auto survivor = crowd[99];
    const auto victimChunk = store.locate(victim).chunk;
    CHECK(victimChunk == store.locate(survivor).chunk, 16, "crowd entities share a chunk");
    store.destroy(victim);
    CHECK(!store.valid(victim), 17, "destroyed handle invalid");
    CHECK(store.valid(survivor), 18, "survivor handle still valid");
    CHECK(store.get<Health>(survivor).value == 99, 19, "swap-back moved the right slot");
    CHECK(store.locate(survivor).chunk == victimChunk, 20, "chunk pointer stable across swap-back");

    // ---- query matching ---------------------------------------------------
    auto& positionQuery = store.query<Position>();
    const auto positionCount = positionQuery.entity_count();
    CHECK(positionCount == 100u, 21, "query entity count (99 crowd + a)");

    auto& pairQuery = store.query<Position, Health>();
    CHECK(pairQuery.entity_count() == 99u, 22, "query needs every component");
    std::size_t pairsSeen = 0;
    pairQuery.each<Position, Health>([&](Entity, Position& position, Health& health) {
        ++pairsSeen;
        position.y = static_cast<float>(health.value);
    });
    CHECK(pairsSeen == pairQuery.entity_count(), 23, "each visits every match once");

    auto& withTag = store.query_with({shinkou::ecs::type_id<Position>()}, {shinkou::ecs::type_id<Tag>()});
    CHECK(withTag.entity_count() == 99u, 24, "query without tag excludes tagged entity");

    // ---- per-chunk arrays share one index space ---------------------------
    std::size_t chunkRows = 0;
    std::size_t chunkEntities = 0;
    bool arrayMismatch = false;
    bool arrayMissing = false;
    bool chunkInvalid = false;
    pairQuery.each_chunk<Position, Health>([&](shinkou::ecs::Chunk& chunk, Position* positions, Health* health) {
        // Lambdas stay void: no CHECK macro (its `return fail(...)` would make
        // the lambda non-void and fall off the end).
        if (!positions || !health) {
            arrayMissing = true;
            return;
        }
        if (chunk.count == 0u || chunk.count > chunk.capacity) {
            chunkInvalid = true;
            return;
        }
        for (std::uint32_t index = 0; index < chunk.count; ++index) {
            // The crowd was built with position.x == health.value, so a shared
            // index space is observable here.
            if (health[index].value != static_cast<std::int32_t>(positions[index].x)) arrayMismatch = true;
        }
        ++chunkRows;
        chunkEntities += chunk.count;
    });
    CHECK(!arrayMissing, 25, "chunk arrays resolved");
    CHECK(!chunkInvalid, 26, "chunk count in range");
    CHECK(!arrayMismatch, 27, "arrays index the same entity slot");
    CHECK(chunkRows > 0u && chunkEntities == 99u, 28, "each_chunk covers all entities");

    // ---- chunk capacity honours the 16 KiB budget -------------------------
    const auto capacity = pairQuery.chunks().front()->capacity;
    const auto stride = pairQuery.chunks().front()->archetype->entity_stride();
    CHECK(static_cast<std::uint32_t>(sizeof(shinkou::ecs::Chunk)) + capacity * stride <= shinkou::ecs::kChunkSize,
          29, "chunk layout fits 16 KiB");
    CHECK(capacity > 1u, 30, "chunk holds multiple entities");

    // ---- cached query invalidates on structural change --------------------
    const auto beforeAdd = positionQuery.entity_count();
    const auto later = store.create();
    store.emplace<Position>(later, Position{});
    CHECK(positionQuery.entity_count() == beforeAdd + 1u, 31, "query cache invalidated by new archetype");

    // ---- remove component -------------------------------------------------
    store.remove<Tag>(a);
    CHECK(!store.has<Tag>(a), 32, "component removed");
    CHECK(store.has<Position>(a) && store.get<Position>(a).x == 42.0f, 33, "other components preserved");
    CHECK(withTag.entity_count() == pairQuery.entity_count() + 2u, 34, "query reflects removal");

    // ---- non-trivial component lifetime through migration -----------------
    {
        const auto tracked = store.create();
        store.emplace<Tracked>(tracked, std::string("payload"));
        CHECK(Tracked::alive == 1, 35, "non-trivial component constructed");
        store.emplace<Position>(tracked, Position{});
        CHECK(Tracked::alive == 1 && Tracked::copies >= 1, 36, "migration copies, never leaks");
        CHECK(store.get<Tracked>(tracked).text == "payload", 37, "non-trivial payload survives migration");
        store.remove<Tracked>(tracked);
        CHECK(Tracked::alive == 0, 38, "removal destroys exactly once");
        store.destroy(tracked);
    }

    // ---- deferred structural changes --------------------------------------
    {
        const auto beforePlayback = store.alive_count();
        shinkou::ecs::CommandBuffer buffer(store);
        const auto placeholder = buffer.create_entity();
        buffer.add_component<Position>(placeholder, Position{7.0f, 8.0f, 9.0f});
        buffer.add_component<Health>(placeholder, Health{70});
        const auto doomed = buffer.create_entity();
        buffer.destroy_entity(doomed);
        CHECK(store.alive_count() == beforePlayback, 39, "buffer is inert before playback");
        buffer.playback();
        CHECK(buffer.empty(), 40, "buffer clears after playback");

        std::size_t resolved = 0;
        store.query<Position, Health>().each<Position, Health>(
            [&](Entity, Position& position, Health& health) {
                if (position.x == 7.0f && health.value == 70) ++resolved;
            });
        CHECK(resolved == 1u, 41, "placeholder entity resolved during playback");
    }

    // ---- clear invalidates every handle -----------------------------------
    const auto preClear = store.create();
    store.emplace<Position>(preClear, Position{});
    store.clear();
    CHECK(store.alive_count() == 0u, 42, "clear empties the store");
    CHECK(!store.valid(preClear), 43, "pre-clear handle invalid");
    CHECK(store.query<Position>().entity_count() == 0u, 44, "queries empty after clear");
    const auto postClear = store.create();
    CHECK(store.valid(postClear), 45, "store reusable after clear");
    CHECK(store.query<Position>().entity_count() == 0u, 46, "recycled index has no stale components");

    CHECK(Tracked::alive == 0, 47, "no leaked component instances");

    std::printf("ecs store tests passed (chunks=%zu, archetypes=%zu)\n", store.chunk_count(),
                store.archetype_count());
    return 0;
}

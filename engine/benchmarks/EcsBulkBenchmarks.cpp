#include "shinkou/ecs/EntityStore.h"
#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

namespace {
struct Position {
    float x{}, y{}, z{};
};
struct Velocity {
    float x{1}, y{2}, z{3};
};
struct Unrelated {
    int value{};
};
template <std::size_t I> struct Marker {
    int value{};
};
template <std::size_t... I>
void unrelated_shapes(shinkou::ecs::EntityStore &store, std::index_sequence<I...>) {
    (store.archetype<Marker<I>>(), ...);
}
using Clock = std::chrono::steady_clock;
template <class Fn> double ms(Fn &&fn) {
    auto start = Clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
} // namespace

int main() {
    constexpr std::size_t count = 100000, rounds = 20;
    shinkou::ecs::EntityStore store;
    store.reserve(count);
    auto &shape = store.archetype<Position, Velocity>();
    std::vector<shinkou::Entity> entities(count);
    double creation = 0, clearing = 0;
    std::uint64_t checksum = 0;
    for (std::size_t r = 0; r <= rounds; ++r) {
        const auto build = ms([&] {
            for (auto &e : entities)
                e = store.create(shape);
        });
        checksum += store.alive_count();
        const auto clear = ms([&] { store.clear(); });
        if (r) {
            creation += build;
            clearing += clear;
        }
    }
    std::printf("entities=%zu rounds=%zu create_loop_ms=%.3f clear_ms=%.3f checksum=%llu\n", count,
                rounds, creation / rounds, clearing / rounds,
                static_cast<unsigned long long>(checksum));

    double batch = 0;
    std::uint64_t batchChecksum = 0;
    for (std::size_t r = 0; r <= rounds; ++r) {
        const auto elapsed =
            ms([&] { store.create_batch(shape, entities.data(), entities.size()); });
        batchChecksum += store.alive_count();
        store.clear();
        if (r)
            batch += elapsed;
    }
    std::printf("create_batch_ms=%.3f checksum=%llu\n", batch / rounds,
                static_cast<unsigned long long>(batchChecksum));

    // A matching query in a heterogeneous world should not repeatedly inspect
    // signatures unrelated to it when another entity changes chunk membership.
    unrelated_shapes(store, std::make_index_sequence<128>{});
    store.create(shape);
    auto &unrelated = store.archetype<Unrelated>();
    auto &query = store.query<Position, Velocity>();
    query.chunks();
    constexpr int mutations = 10000;
    const auto refresh = ms([&] {
        for (int i = 0; i < mutations; ++i) {
            const auto e = store.create(unrelated);
            checksum += query.entity_count();
            store.destroy(e);
            checksum += query.entity_count();
        }
    });
    std::printf("unrelated_archetypes=128 mutations=%d query_refresh_ms=%.3f checksum=%llu\n",
                mutations, refresh, static_cast<unsigned long long>(checksum));
}

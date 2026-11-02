#pragma once

#include "shinkou/ecs/Chunk.h"

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace shinkou::ecs {

class EntityStore;

// Cached archetype match for a component signature.
//
// The chunk list is rebuilt only when the store's structure version changes
// (archetype created, chunk allocated or released).  Between structural changes
// iterating a query costs one pointer bump per chunk and one linear scan per
// matching array, with no per-entity indirection and no hashing.
class Query {
  public:
    Query(const EntityStore &store, std::vector<TypeId> all, std::vector<TypeId> none = {});

    const std::vector<TypeId> &all() const noexcept {
        return all_;
    }
    const std::vector<TypeId> &none() const noexcept {
        return none_;
    }

    // Allocation-free signature check used by the query cache.  `all` must be
    // sorted ascending, exactly like the stored signature.
    bool signature_matches(const TypeId *all, std::size_t count) const noexcept {
        if (all_.size() != count || !none_.empty())
            return false;
        if (count == 0u)
            return true;
        return std::equal(all_.begin(), all_.end(), all);
    }

    const std::vector<Chunk *> &chunks();
    std::size_t entity_count();

    // fn(Entity, T&...) for every matching entity.
    template <class... Ts, class Fn> void each(Fn &&fn) {
        each_impl<Ts...>(fn, std::index_sequence_for<Ts...>{});
    }

    // fn(Chunk&, T*...) once per matching chunk; the arrays are tightly packed
    // and share the chunk's index space, so this is the parallel-friendly form.
    template <class... Ts, class Fn> void each_chunk(Fn &&fn) {
        for (Chunk *chunk : chunks())
            fn(*chunk, chunk->template data<Ts>()...);
    }

  private:
    template <class... Ts, class Fn, std::size_t... Is>
    void each_impl(Fn &fn, std::index_sequence<Is...>) {
        for (Chunk *chunk : chunks()) {
            auto pointers = std::make_tuple(chunk->template data<Ts>()...);
            const Entity *entities = chunk->entities();
            const std::uint32_t count = chunk->count;
            for (std::uint32_t index = 0; index < count; ++index)
                fn(entities[index], std::get<Is>(pointers)[index]...);
        }
    }

    bool matches(const Archetype &archetype) const noexcept;

    const EntityStore *store_{nullptr};
    std::vector<TypeId> all_;
    std::vector<TypeId> none_;
    // Archetypes never change their signatures or disappear. Match only newly
    // appended archetypes; unrelated chunk churn must not rescan old signatures.
    std::vector<const Archetype *> matchedArchetypes_;
    std::size_t scannedArchetypes_{0};
    std::vector<Chunk *> chunkList_;
    std::uint64_t cachedStructureVersion_{0};
    bool valid_{false};
};

} // namespace shinkou::ecs

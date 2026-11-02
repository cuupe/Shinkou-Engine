#pragma once

#include "shinkou/Types.h"
#include "shinkou/ecs/Chunk.h"
#include "shinkou/ecs/Query.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace shinkou::ecs {

// FNV-1a over a sorted type-id signature.  Shared by the template query cache
// and the runtime query builder so a query is created exactly once per shape.
inline std::uint64_t hash_type_ids(const TypeId *ids, std::size_t count) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t index = 0; index < count; ++index) {
        hash ^= ids[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Entity -> (chunk, slot) indirection, exactly one flat array entry per entity
// index.  Resolving an entity is one array read, never a hash lookup.
struct EntityInChunk {
    Chunk *chunk{nullptr};
    std::uint32_t index{0};
};

// DOTS-style entity/component storage.
//
// Responsibilities:
//   * 8-byte entity handles: {index, generation}, recycled indices bumped on
//     destroy so stale handles fail validation.
//   * archetypes keyed by the unique component-type set, stored in 16 KiB
//     struct-of-arrays chunks.
//   * structural changes (add/remove component) migrate an entity between
//     archetypes, copying the shared component values and swap-removing the
//     source slot.
//   * cached queries that iterate whole chunks.
//
// The store is single-threaded for structural changes (like Unity's
// EntityManager); component reads and writes inside a chunk are safe to
// distribute over worker threads between sync points.
class EntityStore {
  public:
    EntityStore();
    ~EntityStore();
    EntityStore(const EntityStore &) = delete;
    EntityStore &operator=(const EntityStore &) = delete;

    // ---- entities -------------------------------------------------------
    Entity create() noexcept;
    Entity create(Archetype &archetype) noexcept;
    // Fills caller-owned output with entities, initializing contiguous component
    // columns once per chunk. Same defaults and generation rules as create().
    // output must hold count handles; archetype must belong to this store.
    void create_batch(Archetype &archetype, Entity *output, std::size_t count) noexcept;
    void destroy(Entity entity) noexcept;
    void clear() noexcept;
    bool valid(Entity entity) const noexcept;
    std::size_t alive_count() const noexcept {
        return aliveCount_;
    }
    std::size_t capacity() const noexcept {
        return table_.size() > 0u ? table_.size() - 1u : 0u;
    }
    void reserve(std::size_t entityCapacity);
    EntityInChunk locate(Entity entity) const noexcept;

    // ---- archetypes -----------------------------------------------------
    Archetype &empty_archetype() noexcept {
        return *emptyArchetype_;
    }
    const Archetype &empty_archetype() const noexcept {
        return *emptyArchetype_;
    }

    template <class... Ts> Archetype &archetype() {
        return find_or_create_archetype(std::initializer_list<TypeId>{type_id<Ts>()...});
    }

    Archetype &find_or_create_archetype(std::initializer_list<TypeId> types);
    Archetype &find_or_create_archetype(const std::vector<TypeId> &types);

    std::size_t archetype_count() const noexcept {
        return archetypeList_.size();
    }
    const std::vector<Archetype *> &archetypes() const noexcept {
        return archetypeList_;
    }
    std::uint64_t structure_version() const noexcept {
        return structureVersion_;
    }
    std::size_t chunk_count() const noexcept {
        return allocator_.live_chunks();
    }

    // ---- components -----------------------------------------------------
    template <class T> bool has(Entity entity) const noexcept {
        const auto in = locate(entity);
        return in.chunk && in.chunk->archetype->has(type_id<T>());
    }

    template <class T> T *try_get(Entity entity) noexcept {
        const auto in = locate(entity);
        if (!in.chunk)
            return nullptr;
        const auto offset = in.chunk->archetype->offset(type_id<T>());
        if (offset == 0u)
            return nullptr;
        return reinterpret_cast<T *>(in.chunk->bytes() + offset + in.index * sizeof(T));
    }

    template <class T> const T *try_get(Entity entity) const noexcept {
        return const_cast<EntityStore *>(this)->try_get<T>(entity);
    }

    template <class T> T &get(Entity entity) {
        T *value = try_get<T>(entity);
        assert(value && "component not present on entity");
        return *value;
    }

    template <class T> const T &get(Entity entity) const {
        return const_cast<EntityStore *>(this)->template get<T>(entity);
    }

    template <class T, class... Args> T &emplace(Entity entity, Args &&...args) {
        assert(valid(entity));
        const auto in = table_[entity.id];
        const auto type = type_id<T>();
        if (in.chunk && in.chunk->archetype->has(type)) {
            T *existing = reinterpret_cast<T *>(
                in.chunk->bytes() + in.chunk->archetype->offset(type) + in.index * sizeof(T));
            existing->~T();
            ::new (static_cast<void *>(existing)) T(std::forward<Args>(args)...);
            return *existing;
        }

        Archetype &target = archetype_plus(*in.chunk->archetype, type);
        std::uint32_t targetIndex = 0;
        Chunk *targetChunk = acquire_slot(target, targetIndex);
        T *slot = reinterpret_cast<T *>(targetChunk->bytes() + target.offset(type) +
                                        targetIndex * sizeof(T));
        ::new (static_cast<void *>(slot)) T(std::forward<Args>(args)...);

        copy_shared(*in.chunk, in.index, *targetChunk, targetIndex, kInvalidTypeId);
        targetChunk->entities()[targetIndex] = entity;
        destroy_slot(*in.chunk, in.index);
        release_slot(*in.chunk, in.index);
        table_[entity.id] = {targetChunk, targetIndex};
        return *slot;
    }

    template <class T, class... Args> T &emplace_or_replace(Entity entity, Args &&...args) {
        return emplace<T>(entity, std::forward<Args>(args)...);
    }

    template <class T> void set(Entity entity, const T &value) {
        T *slot = try_get<T>(entity);
        if (slot) {
            *slot = value;
        } else {
            emplace<T>(entity, value);
        }
    }

    template <class T> void remove(Entity entity) {
        const auto in = locate(entity);
        if (!in.chunk)
            return;
        const auto type = type_id<T>();
        if (!in.chunk->archetype->has(type))
            return;
        Archetype &target = archetype_minus(*in.chunk->archetype, type);

        std::uint32_t targetIndex = 0;
        Chunk *targetChunk = acquire_slot(target, targetIndex);
        copy_shared(*in.chunk, in.index, *targetChunk, targetIndex, type);
        targetChunk->entities()[targetIndex] = entity;

        destroy_slot(*in.chunk, in.index);
        release_slot(*in.chunk, in.index);
        table_[entity.id] = {targetChunk, targetIndex};
    }

    // ---- queries --------------------------------------------------------
    template <class... Ts> Query &query() const {
        std::array<TypeId, sizeof...(Ts)> ids{type_id<Ts>()...};
        std::sort(ids.begin(), ids.end());
        const std::uint64_t key = hash_type_ids(ids.data(), ids.size());
        // operator[] on an existing key does not allocate, so the steady-state
        // frame path performs no heap allocation.
        auto &bucket = queries_[key];
        for (auto &candidate : bucket) {
            if (candidate->signature_matches(ids.data(), ids.size()))
                return *candidate;
        }
        bucket.push_back(std::make_unique<Query>(*this, std::vector<TypeId>(ids.begin(), ids.end()),
                                                 std::vector<TypeId>{}));
        return *bucket.back();
    }
    Query &query_with(std::vector<TypeId> all, std::vector<TypeId> none = {}) const;

    // Visits every live entity exactly once.  Intended for editor and debug
    // paths, not for per-frame simulation.
    template <class Fn> void each_entity(Fn &&fn) {
        for (std::uint32_t id = 1; id < table_.size(); ++id) {
            const auto in = table_[id];
            if (!in.chunk)
                continue;
            fn(Entity{id, versions_[id]});
        }
    }

  private:
    friend class Query;
    friend class CommandBuffer;

    std::uint32_t allocate_entity_index() noexcept;
    Archetype &find_or_create_archetype_sorted(std::vector<TypeId> types);
    Chunk *acquire_slot(Archetype &archetype, std::uint32_t &index) noexcept;
    void release_slot(Chunk &chunk, std::uint32_t index) noexcept;
    void copy_shared(Chunk &source, std::uint32_t sourceIndex, Chunk &target,
                     std::uint32_t targetIndex, TypeId skip) noexcept;
    void destroy_slot(Chunk &chunk, std::uint32_t index) noexcept;
    Archetype &archetype_plus(Archetype &base, TypeId add);
    Archetype &archetype_minus(Archetype &base, TypeId remove);

    std::vector<EntityInChunk> table_{EntityInChunk{}};
    std::vector<std::uint32_t> versions_{0u};
    std::vector<std::uint32_t> freeIndices_;
    std::unordered_map<std::uint64_t, std::vector<std::unique_ptr<Archetype>>> archetypesByKey_;
    std::vector<Archetype *> archetypeList_;
    std::unique_ptr<Archetype> emptyArchetype_;
    ChunkAllocator allocator_;
    std::vector<TypeId> typeScratch_;
    mutable std::unordered_map<std::uint64_t, std::vector<std::unique_ptr<Query>>> queries_;
    std::size_t aliveCount_{0};
    std::uint32_t nextIndex_{1};
    std::uint64_t structureVersion_{1};
};

} // namespace shinkou::ecs

#pragma once

#include "shinkou/Types.h"
#include "shinkou/ecs/ComponentType.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace shinkou::ecs {

class Archetype;
class ChunkAllocator;
class EntityStore;

inline constexpr std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) noexcept {
    if (alignment <= 1u)
        return value;
    return (value + alignment - 1u) & ~(alignment - 1u);
}

// One 16 KiB block of an archetype.
//
//   [ Chunk header ][ Entity array ][ component array A ][ component array B ]...
//
// All arrays are tightly packed and share one index space: the entity at index i
// owns slot i of every component array.  Removing an entity swaps the chunk's
// last entity into the hole, which is why chunk pointers stay stable and why a
// structural change invalidates component pointers but never the chunk itself.
struct Chunk {
    Archetype *archetype{nullptr};
    std::uint32_t count{0};
    std::uint32_t capacity{0};
    // Position of this chunk inside Archetype::chunks(), kept in sync by
    // Archetype::add_chunk/remove_chunk so a freed slot can restore the
    // partial-chunk hint without a search.
    std::uint32_t indexInArchetype{0};
    // Bumped by any structural change touching this chunk.  Systems that cache
    // per-chunk derived data compare this instead of rebuilding every frame.
    std::uint32_t orderVersion{0};

    std::uint8_t *bytes() noexcept {
        return reinterpret_cast<std::uint8_t *>(this);
    }
    const std::uint8_t *bytes() const noexcept {
        return reinterpret_cast<const std::uint8_t *>(this);
    }

    Entity *entities() noexcept;
    const Entity *entities() const noexcept;

    void *data(TypeId id) noexcept;
    const void *data(TypeId id) const noexcept;

    template <class T> T *data() noexcept {
        return reinterpret_cast<T *>(data(type_id<T>()));
    }
    template <class T> const T *data() const noexcept {
        return reinterpret_cast<const T *>(data(type_id<T>()));
    }

    bool empty() const noexcept {
        return count == 0u;
    }
    bool full() const noexcept {
        return count >= capacity;
    }
};

// Unique component-type combination.  Every entity in an archetype is stored in
// the same shape of chunks, so a query that matches an archetype can iterate
// whole chunks without inspecting individual entities.
class Archetype {
  public:
    Archetype(std::vector<TypeId> types, ChunkAllocator &allocator);
    ~Archetype();
    Archetype(const Archetype &) = delete;
    Archetype &operator=(const Archetype &) = delete;

    const std::vector<TypeId> &types() const noexcept {
        return types_;
    }
    const std::vector<const ComponentTypeInfo *> &infos() const noexcept {
        return infos_;
    }
    std::uint64_t key() const noexcept {
        return key_;
    }

    bool has(TypeId id) const noexcept;
    // Offset from the start of the chunk; 0 means "not present" (the entity
    // array always starts after the header, so no component array sits at 0).
    std::uint32_t offset(TypeId id) const noexcept;
    std::uint32_t entity_offset() const noexcept {
        return entityOffset_;
    }
    std::uint32_t entity_stride() const noexcept {
        return entityStride_;
    }
    std::uint32_t capacity() const noexcept {
        return capacity_;
    }
    std::uint32_t size() const noexcept {
        return entityCount_;
    }

    const std::vector<Chunk *> &chunks() const noexcept {
        return chunks_;
    }

  private:
    void compute_layout(std::uint32_t capacity) noexcept;
    std::uint32_t layout_end(std::uint32_t capacity) const noexcept;
    void add_chunk(Chunk *chunk) noexcept;
    void remove_chunk(Chunk *chunk) noexcept;
    void note_allocated() noexcept {
        ++entityCount_;
    }
    void note_freed() noexcept {
        --entityCount_;
    }

    std::vector<TypeId> types_;
    std::vector<const ComponentTypeInfo *> infos_;
    std::vector<std::uint32_t> offsetsById_; // 0 == absent
    std::uint32_t entityOffset_{0};
    std::uint32_t entityStride_{0};
    std::uint32_t capacity_{0};
    std::uint32_t entityCount_{0};
    std::uint64_t key_{0};
    std::vector<Chunk *> chunks_;
    // Archetypes live for the store lifetime. Cache migration edges so repeated
    // add/remove operations never rebuild and sort a component signature.
    std::unordered_map<TypeId, Archetype *> addEdges_;
    std::unordered_map<TypeId, Archetype *> removeEdges_;
    std::size_t firstPartialChunk_{0};
    ChunkAllocator *allocator_{nullptr};

    friend class EntityStore;
};

// Allocates fixed 16 KiB chunks.  Pointers are stable for the store lifetime,
// which is what lets EntityInChunk live in a flat table.
class ChunkAllocator {
  public:
    ChunkAllocator();
    ~ChunkAllocator();
    ChunkAllocator(const ChunkAllocator &) = delete;
    ChunkAllocator &operator=(const ChunkAllocator &) = delete;

    Chunk *allocate(Archetype &archetype);
    void deallocate(Chunk *chunk) noexcept;
    std::size_t live_chunks() const noexcept {
        return live_;
    }
    std::size_t reserved_chunks() const noexcept {
        return free_.size();
    }

  private:
    std::vector<std::unique_ptr<std::uint8_t[]>> blocks_;
    std::vector<Chunk *> free_;
    std::size_t live_{0};
};

inline Entity *Chunk::entities() noexcept {
    return reinterpret_cast<Entity *>(bytes() + archetype->entity_offset());
}

inline const Entity *Chunk::entities() const noexcept {
    return reinterpret_cast<const Entity *>(bytes() + archetype->entity_offset());
}

inline void *Chunk::data(TypeId id) noexcept {
    return bytes() + archetype->offset(id);
}

inline const void *Chunk::data(TypeId id) const noexcept {
    return bytes() + archetype->offset(id);
}

} // namespace shinkou::ecs

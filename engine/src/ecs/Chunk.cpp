#include "shinkou/ecs/Chunk.h"

#include <algorithm>
#include <cassert>
#include <cstdint>

namespace shinkou::ecs {
namespace {

std::uint64_t hash_types(const std::vector<TypeId> &types) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto id : types) {
        hash ^= id;
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

Archetype::Archetype(std::vector<TypeId> types, ChunkAllocator &allocator)
    : types_(std::move(types)), allocator_(&allocator) {
    std::sort(types_.begin(), types_.end());
    types_.erase(std::unique(types_.begin(), types_.end()), types_.end());
    key_ = hash_types(types_);

    TypeId maxId = 0;
    for (const auto id : types_)
        maxId = std::max(maxId, id);
    offsetsById_.assign(static_cast<std::size_t>(maxId) + 1u, 0u);

    infos_.reserve(types_.size());
    entityOffset_ = align_up(static_cast<std::uint32_t>(sizeof(Chunk)), alignof(Entity));
    entityStride_ = static_cast<std::uint32_t>(sizeof(Entity));
    for (const auto id : types_) {
        const auto *info = type_info(id);
        assert(info && "component type used before registration");
        infos_.push_back(info);
        if (!info->tag)
            entityStride_ += info->size;
    }

    // Find the largest capacity whose exact layout still fits in 16 KiB.
    const std::uint32_t available = kChunkSize - entityOffset_;
    std::uint32_t capacity = available / std::max(entityStride_, 1u);
    if (capacity == 0u)
        capacity = 1u;
    while (capacity > 1u && layout_end(capacity) > kChunkSize)
        --capacity;
    compute_layout(capacity);
}

Archetype::~Archetype() = default;

std::uint32_t Archetype::layout_end(std::uint32_t capacity) const noexcept {
    std::uint32_t cursor = entityOffset_ + capacity * static_cast<std::uint32_t>(sizeof(Entity));
    for (const auto *info : infos_) {
        if (info->tag)
            continue;
        cursor = align_up(cursor, info->alignment);
        cursor += capacity * info->size;
    }
    return cursor;
}

void Archetype::compute_layout(std::uint32_t capacity) noexcept {
    std::fill(offsetsById_.begin(), offsetsById_.end(), 0u);
    capacity_ = capacity;
    std::uint32_t cursor = entityOffset_ + capacity * static_cast<std::uint32_t>(sizeof(Entity));
    for (const auto *info : infos_) {
        if (info->tag)
            continue;
        cursor = align_up(cursor, info->alignment);
        if (info->id < offsetsById_.size())
            offsetsById_[info->id] = cursor;
        cursor += capacity * info->size;
    }
    assert(cursor <= kChunkSize);
}

bool Archetype::has(TypeId id) const noexcept {
    return std::binary_search(types_.begin(), types_.end(), id);
}

std::uint32_t Archetype::offset(TypeId id) const noexcept {
    return id < offsetsById_.size() ? offsetsById_[id] : 0u;
}

void Archetype::add_chunk(Chunk *chunk) noexcept {
    if (chunks_.empty() || firstPartialChunk_ > chunks_.size())
        firstPartialChunk_ = 0;
    chunk->indexInArchetype = static_cast<std::uint32_t>(chunks_.size());
    chunks_.push_back(chunk);
}

void Archetype::remove_chunk(Chunk *chunk) noexcept {
    const auto index = static_cast<std::size_t>(chunk->indexInArchetype);
    assert(index < chunks_.size() && chunks_[index] == chunk);
    if (index < firstPartialChunk_)
        firstPartialChunk_ = index;
    chunks_[index] = chunks_.back();
    chunks_.pop_back();
    if (index < chunks_.size())
        chunks_[index]->indexInArchetype = static_cast<std::uint32_t>(index);
    if (firstPartialChunk_ >= chunks_.size())
        firstPartialChunk_ = 0;
}

ChunkAllocator::ChunkAllocator() = default;
ChunkAllocator::~ChunkAllocator() = default;

Chunk *ChunkAllocator::allocate(Archetype &archetype) {
    Chunk *chunk = nullptr;
    if (!free_.empty()) {
        chunk = free_.back();
        free_.pop_back();
    } else {
        // 16 KiB blocks are kept alive for the allocator lifetime so chunk
        // pointers never move; the extra 64 bytes allow a 64-byte alignment.
        auto block = std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(kChunkSize) + 64u);
        const auto address = reinterpret_cast<std::uintptr_t>(block.get());
        const auto aligned = (address + 63u) & ~static_cast<std::uintptr_t>(63u);
        chunk = reinterpret_cast<Chunk *>(aligned);
        blocks_.push_back(std::move(block));
    }
    chunk->archetype = &archetype;
    chunk->count = 0;
    chunk->capacity = archetype.capacity();
    chunk->orderVersion = 0;
    ++live_;
    return chunk;
}

void ChunkAllocator::deallocate(Chunk *chunk) noexcept {
    if (!chunk)
        return;
    chunk->archetype = nullptr;
    chunk->count = 0;
    free_.push_back(chunk);
    --live_;
}

} // namespace shinkou::ecs

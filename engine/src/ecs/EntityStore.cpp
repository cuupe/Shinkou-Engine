#include "shinkou/ecs/EntityStore.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace shinkou::ecs {

EntityStore::EntityStore() {
    emptyArchetype_.reset(new Archetype({}, allocator_));
}

EntityStore::~EntityStore() {
    // Run component destructors for anything still alive.  Chunk memory itself
    // is released by the allocator (the chunk list can outlive entities), but
    // non-trivial components still need their teardown.
    clear();
}

std::uint32_t EntityStore::allocate_entity_index() noexcept {
    if (!freeIndices_.empty()) {
        const auto index = freeIndices_.back();
        freeIndices_.pop_back();
        return index;
    }
    const auto index = nextIndex_++;
    if (index >= table_.size())
        table_.resize(static_cast<std::size_t>(index) + 1u);
    if (index >= versions_.size())
        versions_.resize(static_cast<std::size_t>(index) + 1u, 0u);
    return index;
}

Entity EntityStore::create() noexcept {
    return create(*emptyArchetype_);
}

Entity EntityStore::create(Archetype &archetype) noexcept {
    const auto index = allocate_entity_index();
    std::uint32_t slot = 0;
    Chunk *chunk = acquire_slot(archetype, slot);

    std::uint8_t *base = chunk->bytes();
    for (const auto *info : archetype.infos()) {
        if (info->tag)
            continue;
        auto *destination = base + archetype.offset(info->id) + slot * info->size;
        std::memset(destination, 0, info->size);
        info->construct(destination, 1);
    }

    const Entity entity{index, versions_[index]};
    chunk->entities()[slot] = entity;
    table_[index] = {chunk, slot};
    ++aliveCount_;
    return entity;
}

void EntityStore::create_batch(Archetype &archetype, Entity *output, std::size_t count) noexcept {
    if (!count)
        return;
    assert(output);
    const auto fresh = count > freeIndices_.size() ? count - freeIndices_.size() : 0u;
    assert(fresh <= std::numeric_limits<std::uint32_t>::max() - nextIndex_);
    const auto required = static_cast<std::size_t>(nextIndex_) + fresh;
    if (required > table_.size())
        table_.resize(required);
    if (required > versions_.size())
        versions_.resize(required, 0u);

    for (std::size_t written = 0; written < count;) {
        std::uint32_t first = 0;
        Chunk *chunk = acquire_slot(archetype, first);
        const auto rows = static_cast<std::uint32_t>(
            std::min<std::size_t>(count - written, chunk->capacity - first));
        // acquire_slot already accounts for the first row.
        chunk->count += rows - 1u;
        chunk->orderVersion += rows - 1u;
        archetype.entityCount_ += rows - 1u;
        for (const auto *info : archetype.infos()) {
            if (info->tag)
                continue;
            auto *destination = chunk->bytes() + archetype.offset(info->id) + first * info->size;
            std::memset(destination, 0, static_cast<std::size_t>(rows) * info->size);
            info->construct(destination, rows);
        }
        Entity *handles = chunk->entities() + first;
        for (std::uint32_t row = 0; row < rows; ++row) {
            std::uint32_t index;
            if (freeIndices_.empty())
                index = nextIndex_++;
            else {
                index = freeIndices_.back();
                freeIndices_.pop_back();
            }
            const Entity entity{index, versions_[index]};
            handles[row] = entity;
            output[written + row] = entity;
            table_[index] = {chunk, first + row};
        }
        written += rows;
        aliveCount_ += rows;
    }
}

void EntityStore::destroy(Entity entity) noexcept {
    if (!valid(entity))
        return;
    const auto in = table_[entity.id];
    destroy_slot(*in.chunk, in.index);
    release_slot(*in.chunk, in.index);
    table_[entity.id] = EntityInChunk{};
    ++versions_[entity.id];
    freeIndices_.push_back(entity.id);
    --aliveCount_;
}

void EntityStore::clear() noexcept {
    // No survivors need to fill holes: destroy whole columns and retire chunks
    // directly instead of swap-copying every entity through destroy().
    const auto clearArchetype = [this](Archetype &archetype, bool keepOneChunk) {
        for (Chunk *chunk : archetype.chunks_) {
            for (const auto *info : archetype.infos()) {
                if (info->tag || info->triviallyDestructible)
                    continue;
                info->destruct(chunk->bytes() + archetype.offset(info->id), chunk->count);
            }
            const auto *entities = chunk->entities();
            for (std::uint32_t row = 0; row < chunk->count; ++row)
                ++versions_[entities[row].id];
            chunk->count = 0;
            ++chunk->orderVersion;
        }
        const std::size_t retained = keepOneChunk && !archetype.chunks_.empty() ? 1u : 0u;
        while (archetype.chunks_.size() > retained) {
            auto *chunk = archetype.chunks_.back();
            archetype.chunks_.pop_back();
            allocator_.deallocate(chunk);
        }
        archetype.entityCount_ = 0;
        archetype.firstPartialChunk_ = 0;
    };
    clearArchetype(*emptyArchetype_, true);
    for (auto *archetype : archetypeList_)
        clearArchetype(*archetype, false);
    std::fill(table_.begin(), table_.end(), EntityInChunk{});
    freeIndices_.clear();
    nextIndex_ = 1;
    aliveCount_ = 0;
    ++structureVersion_;
}

bool EntityStore::valid(Entity entity) const noexcept {
    return entity.id != 0u && entity.id < table_.size() && table_[entity.id].chunk != nullptr &&
           versions_[entity.id] == entity.generation;
}

void EntityStore::reserve(std::size_t entityCapacity) {
    table_.reserve(entityCapacity + 1u);
    versions_.reserve(entityCapacity + 1u);
    freeIndices_.reserve(entityCapacity);
}

EntityInChunk EntityStore::locate(Entity entity) const noexcept {
    if (entity.id == 0u || entity.id >= table_.size())
        return EntityInChunk{};
    if (versions_[entity.id] != entity.generation)
        return EntityInChunk{};
    return table_[entity.id];
}

Archetype &EntityStore::find_or_create_archetype(std::initializer_list<TypeId> types) {
    return find_or_create_archetype_sorted(std::vector<TypeId>(types));
}

Archetype &EntityStore::find_or_create_archetype(const std::vector<TypeId> &types) {
    return find_or_create_archetype_sorted(types);
}

Archetype &EntityStore::find_or_create_archetype_sorted(std::vector<TypeId> types) {
    std::sort(types.begin(), types.end());
    types.erase(std::unique(types.begin(), types.end()), types.end());

    if (types.empty())
        return *emptyArchetype_;

    const std::uint64_t key = hash_type_ids(types.data(), types.size());
    auto &bucket = archetypesByKey_[key];
    for (auto &candidate : bucket) {
        if (candidate->types() == types)
            return *candidate;
    }

    auto archetype = std::make_unique<Archetype>(types, allocator_);
    auto *raw = archetype.get();
    bucket.push_back(std::move(archetype));
    archetypeList_.push_back(raw);
    ++structureVersion_;
    return *raw;
}

Archetype &EntityStore::archetype_plus(Archetype &base, TypeId add) {
    const auto edge = base.addEdges_.find(add);
    if (edge != base.addEdges_.end())
        return *edge->second;
    std::vector<TypeId> types(base.types());
    types.push_back(add);
    auto &target = find_or_create_archetype_sorted(std::move(types));
    base.addEdges_.emplace(add, &target);
    target.removeEdges_.emplace(add, &base);
    return target;
}

Archetype &EntityStore::archetype_minus(Archetype &base, TypeId remove) {
    const auto edge = base.removeEdges_.find(remove);
    if (edge != base.removeEdges_.end())
        return *edge->second;
    std::vector<TypeId> types;
    types.reserve(base.types().size());
    for (const auto id : base.types())
        if (id != remove)
            types.push_back(id);
    auto &target = find_or_create_archetype_sorted(std::move(types));
    base.removeEdges_.emplace(remove, &target);
    target.addEdges_.emplace(remove, &base);
    return target;
}

Chunk *EntityStore::acquire_slot(Archetype &archetype, std::uint32_t &index) noexcept {
    Chunk *chunk = nullptr;
    auto &chunks = archetype.chunks_;
    for (std::size_t position = archetype.firstPartialChunk_; position < chunks.size();
         ++position) {
        if (!chunks[position]->full()) {
            chunk = chunks[position];
            archetype.firstPartialChunk_ = position;
            break;
        }
    }
    if (!chunk) {
        chunk = allocator_.allocate(archetype);
        archetype.add_chunk(chunk);
        archetype.firstPartialChunk_ = archetype.chunks_.size() - 1u;
        ++structureVersion_;
    }
    index = chunk->count++;
    archetype.note_allocated();
    ++chunk->orderVersion;
    return chunk;
}

void EntityStore::release_slot(Chunk &chunk, std::uint32_t index) noexcept {
    const std::uint32_t last = chunk.count - 1u;
    if (index != last) {
        std::uint8_t *base = chunk.bytes();
        for (const auto *info : chunk.archetype->infos()) {
            if (info->tag)
                continue;
            const auto offset = chunk.archetype->offset(info->id);
            info->copy(base + offset + index * info->size, base + offset + last * info->size, 1);
            // copy constructs the replacement; the old last element must also
            // be destroyed before its storage leaves the live range.
            if (!info->triviallyDestructible)
                info->destruct(base + offset + last * info->size, 1);
        }
        const Entity moved = chunk.entities()[last];
        chunk.entities()[index] = moved;
        table_[moved.id] = {&chunk, index};
    }
    --chunk.count;

    Archetype &archetype = *chunk.archetype;
    archetype.note_freed();
    archetype.firstPartialChunk_ =
        std::min<std::size_t>(archetype.firstPartialChunk_, chunk.indexInArchetype);

    if (chunk.count == 0u && &archetype != emptyArchetype_.get()) {
        archetype.remove_chunk(&chunk);
        allocator_.deallocate(&chunk);
        ++structureVersion_;
    }
}

void EntityStore::copy_shared(Chunk &source, std::uint32_t sourceIndex, Chunk &target,
                              std::uint32_t targetIndex, TypeId skip) noexcept {
    std::uint8_t *sourceBase = source.bytes();
    std::uint8_t *targetBase = target.bytes();
    for (const auto *info : target.archetype->infos()) {
        if (info->tag || info->id == skip)
            continue;
        const auto targetOffset = target.archetype->offset(info->id);
        if (targetOffset == 0u)
            continue;
        const auto sourceOffset = source.archetype->offset(info->id);
        if (sourceOffset == 0u)
            continue;
        info->copy(targetBase + targetOffset + targetIndex * info->size,
                   sourceBase + sourceOffset + sourceIndex * info->size, 1);
    }
}

void EntityStore::destroy_slot(Chunk &chunk, std::uint32_t index) noexcept {
    std::uint8_t *base = chunk.bytes();
    for (const auto *info : chunk.archetype->infos()) {
        if (info->tag || info->triviallyDestructible)
            continue;
        info->destruct(base + chunk.archetype->offset(info->id) + index * info->size, 1);
    }
}

Query &EntityStore::query_with(std::vector<TypeId> all, std::vector<TypeId> none) const {
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    std::sort(none.begin(), none.end());
    none.erase(std::unique(none.begin(), none.end()), none.end());

    // The key covers the required types only; "none" variants share the bucket
    // and are disambiguated by the full signature comparison below.
    const std::uint64_t key = hash_type_ids(all.data(), all.size());

    auto &bucket = queries_[key];
    for (auto &candidate : bucket)
        if (candidate->all() == all && candidate->none() == none)
            return *candidate;

    bucket.push_back(std::make_unique<Query>(*this, std::move(all), std::move(none)));
    return *bucket.back();
}

} // namespace shinkou::ecs

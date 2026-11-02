#include "shinkou/ecs/Query.h"

#include "shinkou/ecs/EntityStore.h"

namespace shinkou::ecs {

Query::Query(const EntityStore &store, std::vector<TypeId> all, std::vector<TypeId> none)
    : store_(&store), all_(std::move(all)), none_(std::move(none)) {
    if (matches(store.empty_archetype()))
        matchedArchetypes_.push_back(&store.empty_archetype());
}

bool Query::matches(const Archetype &archetype) const noexcept {
    for (const auto id : all_) {
        if (!archetype.has(id))
            return false;
    }
    for (const auto id : none_) {
        if (archetype.has(id))
            return false;
    }
    return true;
}

const std::vector<Chunk *> &Query::chunks() {
    const auto version = store_->structure_version();
    if (valid_ && version == cachedStructureVersion_)
        return chunkList_;

    const auto &archetypes = store_->archetypes();
    while (scannedArchetypes_ < archetypes.size()) {
        auto *archetype = archetypes[scannedArchetypes_];
        if (matches(*archetype))
            matchedArchetypes_.push_back(archetype);
        ++scannedArchetypes_;
    }
    chunkList_.clear();
    for (const auto *archetype : matchedArchetypes_) {
        for (Chunk *chunk : archetype->chunks())
            chunkList_.push_back(chunk);
    }
    cachedStructureVersion_ = version;
    valid_ = true;
    return chunkList_;
}

std::size_t Query::entity_count() {
    std::size_t total = 0;
    for (const Chunk *chunk : chunks())
        total += chunk->count;
    return total;
}

} // namespace shinkou::ecs

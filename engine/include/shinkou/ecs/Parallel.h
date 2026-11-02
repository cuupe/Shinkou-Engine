#pragma once

#include "shinkou/ecs/CommandBuffer.h"
#include "shinkou/ecs/Query.h"
#include "shinkou/jobs/JobSystem.h"

#include <cstddef>

namespace shinkou::ecs {

// Runs a chunk-shaped body on the worker pool, one work item per `grain`
// chunks.  Chunks are independent storage blocks, so this is the same
// partitioning DOTS uses for IJobChunk.
//
// The callback runs on worker threads: it must only touch the arrays it
// receives (and data reachable from them).  Structural changes are forbidden
// inside the callback; record commands on a CommandBuffer instead.
template <class... Ts, class Fn>
void parallel_chunks(jobs::JobSystem &system, Query &query, std::size_t grain, Fn &&fn) {
    const auto &list = query.chunks();
    if (list.empty())
        return;
    system.parallel_for(list.size(), grain == 0u ? 1u : grain, [&list, &fn](std::size_t index) {
        Chunk &chunk = *list[index];
        fn(chunk, chunk.template data<Ts>()...);
    });
}

// fn(Chunk&, CommandBuffer&, T*...). Each contiguous partition owns its command
// buffer, including its deferred entity namespace. Do not share placeholders
// across partitions. Playback is explicit and happens after workers have joined.
template <class... Ts, class Fn>
void parallel_chunks_recorded(jobs::JobSystem &system, Query &query, std::size_t grain,
                              CommandBufferGroup &commands, Fn &&fn) {
    const auto &chunks = query.chunks();
    grain = grain ? grain : 1;
    const auto partitions = chunks.size() / grain + (chunks.size() % grain != 0);
    commands.prepare(partitions);
    try {
        system.parallel_for(partitions, 1, [&](std::size_t partition) {
            const auto begin = partition * grain;
            const auto end = std::min(chunks.size(), begin + grain);
            auto &buffer = commands[partition];
            for (auto index = begin; index < end; ++index) {
                auto &chunk = *chunks[index];
                fn(chunk, buffer, chunk.template data<Ts>()...);
            }
        });
    } catch (...) {
        // parallel_for joins its helpers before throwing. Drop commands from
        // failed recording; component array writes are not transactional.
        commands.clear();
        throw;
    }
}

} // namespace shinkou::ecs

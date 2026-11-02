#pragma once

#include "shinkou/ecs/Query.h"
#include "shinkou/jobs/JobSystem.h"
#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace shinkou::ecs {

// Persistent, opt-in chunk systems. const T declares a read; T declares a write.
// Conflicting systems keep registration order. Independent systems can overlap,
// and each system may also distribute its chunks over the same worker pool.
//
// Kernels may only access the declared arrays and immutable external inputs.
// No structural changes, authoring callbacks, pipeline mutation or concurrent
// run() calls are allowed. Query/store lifetimes must exceed the pipeline's.
class ChunkPipeline {
  public:
    ChunkPipeline() = default;
    ChunkPipeline(const ChunkPipeline &) = delete;
    ChunkPipeline &operator=(const ChunkPipeline &) = delete;

    template <class... Ts, class Fn> void add(Query &query, Fn &&fn, std::size_t grain = 8) {
        static_assert(sizeof...(Ts) > 0, "declare every component read or written");
        static_assert((!std::is_empty_v<Ts> && ...), "tags are query filters, not arrays");
        require_idle();
        std::vector<Access> accesses{{type_id<std::remove_const_t<Ts>>(), !std::is_const_v<Ts>}...};
        for (const auto access : accesses) {
            if (!std::binary_search(query.all().begin(), query.all().end(), access.type))
                throw std::invalid_argument(
                    "pipeline component missing from query's required types");
        }
        auto node = std::make_unique<Node>();
        node->query = &query;
        node->accesses = std::move(accesses);
        node->kernel = [body = std::forward<Fn>(fn), grain = grain ? grain : 1](
                           jobs::JobSystem *jobs, const std::vector<Chunk *> &chunks) mutable {
            auto work = [&](std::size_t i) {
                auto &chunk = *chunks[i];
                std::invoke(body, chunk,
                            static_cast<Ts *>(chunk.template data<std::remove_const_t<Ts>>())...);
            };
            if (jobs)
                jobs->parallel_for(chunks.size(), grain, work);
            else
                for (std::size_t i = 0; i < chunks.size(); ++i)
                    work(i);
        };
        const auto index = nodes_.size();
        node->task = [this, index] { execute(index); };
        nodes_.push_back(std::move(node));
        dirty_ = true;
    }

    // Blocks at the pipeline boundary, rather than after each system. Queries
    // are prepared on the caller before workers read their cached chunk lists.
    void run(jobs::JobSystem *jobs = nullptr);
    void clear();
    std::size_t size() const noexcept {
        return nodes_.size();
    }

  private:
    struct Access {
        TypeId type;
        bool write;
    };
    struct Node {
        Query *query{nullptr};
        const std::vector<Chunk *> *chunks{nullptr};
        std::vector<Access> accesses;
        std::vector<std::size_t> successors;
        std::size_t predecessors{0};
        std::size_t depth{1};
        std::atomic<std::size_t> pending{0};
        std::function<void(jobs::JobSystem *, const std::vector<Chunk *> &)> kernel;
        jobs::JobSystem::Function task;
    };
    void require_idle() const;
    void compile();
    void dispatch(std::size_t index);
    void execute(std::size_t index) noexcept;
    void record_error(std::exception_ptr error) noexcept;

    std::vector<std::unique_ptr<Node>> nodes_;
    jobs::JobSystem *jobs_{nullptr};
    jobs::Counter completion_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::mutex errorMutex_;
    std::exception_ptr error_;
    bool dirty_{true};
    bool parallelSystems_{false};
};

} // namespace shinkou::ecs

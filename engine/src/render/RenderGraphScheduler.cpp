#include "shinkou/render/RenderGraphScheduler.h"
#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace shinkou::render {
namespace {

bool is_write_usage(ResourceUsage usage) {
    return usage == ResourceUsage::ShaderWrite || usage == ResourceUsage::StorageWrite ||
        (usage >= ResourceUsage::ColorAttachment0 && usage <= ResourceUsage::ColorAttachment7) ||
        usage == ResourceUsage::DepthStencil || usage == ResourceUsage::CopyDestination;
}

struct ResourceKey {
    std::uint32_t id{0};
    ResourceKind kind{ResourceKind::Buffer};
    bool operator==(const ResourceKey& other) const noexcept {
        return id == other.id && kind == other.kind;
    }
};

struct ResourceKeyHash {
    std::size_t operator()(const ResourceKey& key) const noexcept {
        return (static_cast<std::size_t>(key.id) << 3u) ^
            static_cast<std::size_t>(key.kind);
    }
};

CommandResourceAccessMode default_policy(ResourceHandle, ResourceUsage usage) {
    return is_write_usage(usage) ? CommandResourceAccessMode::Exclusive
                                 : CommandResourceAccessMode::SharedRead;
}

std::size_t default_concurrency() {
    const auto hardware = std::thread::hardware_concurrency();
    return hardware == 0 ? 1u : static_cast<std::size_t>(hardware);
}

void set_plan_error(CommandRecordingPlan& plan, std::string* error, std::string message) {
    plan.valid = false;
    plan.error = std::move(message);
    if (error) *error = plan.error;
}

} // namespace

const CommandRecordingTask* CommandRecordingPlan::task_for_pass(std::size_t passIndex) const noexcept {
    const auto found = std::find_if(tasks.begin(), tasks.end(),
        [passIndex](const CommandRecordingTask& task) { return task.passIndex == passIndex; });
    return found == tasks.end() ? nullptr : &*found;
}

const CommandRecordingResult* CommandRecordingPlan::result_for_pass(std::size_t passIndex) const noexcept {
    const auto task = task_for_pass(passIndex);
    if (!task || task->recordingOrder >= results.size()) return nullptr;
    return &results[task->recordingOrder];
}

CommandRecordingPlan RenderGraphScheduler::build(const RenderGraph& graph, std::string* error) const {
    CommandRecordingPlan plan;
    if (error) error->clear();

    const auto& passes = graph.passes();
    const auto& executionOrder = graph.execution_order();
    const auto& graphBatches = graph.queue_batches();
    const std::size_t count = executionOrder.size();
    if (!passes.empty() && executionOrder.empty() &&
        graph.diagnostics().culledPassCount != passes.size()) {
        set_plan_error(plan, error, "render graph scheduler requires a successfully compiled render graph");
        return plan;
    }
    std::unordered_set<std::size_t> active;
    active.reserve(count);
    for (const auto passIndex : executionOrder) {
        if (passIndex >= passes.size() || !active.insert(passIndex).second) {
            set_plan_error(plan, error, "render graph scheduler received an invalid execution order");
            return plan;
        }
    }

    std::unordered_map<std::size_t, std::uint32_t> passToBatch;
    for (const auto& batch : graphBatches) {
        for (const auto passIndex : batch.passIndices) {
            if (!active.count(passIndex) || passToBatch.find(passIndex) != passToBatch.end()) {
                set_plan_error(plan, error, "render graph scheduler received inconsistent queue batches");
                return plan;
            }
            passToBatch.emplace(passIndex, batch.index);
        }
    }
    plan.batches.clear();
    for (const auto& batch : graphBatches) {
        CommandRecordingBatch copy{batch.index, batch.queue, {}, batch.waitBatches};
        for (const auto passIndex : batch.passIndices) {
            if (active.count(passIndex)) copy.passIndices.push_back(passIndex);
        }
        if (!copy.passIndices.empty()) plan.batches.push_back(std::move(copy));
    }
    plan.submissionOrder.clear();
    for (const auto& batch : plan.batches) {
        plan.submissionOrder.insert(plan.submissionOrder.end(), batch.passIndices.begin(), batch.passIndices.end());
    }
    if (plan.submissionOrder.size() != count) {
        set_plan_error(plan, error, "render graph scheduler could not map every pass to a queue batch");
        return plan;
    }

    std::unordered_map<std::size_t, std::size_t> orderRank;
    for (std::size_t order = 0; order < executionOrder.size(); ++order) orderRank[executionOrder[order]] = order;

    std::vector<std::vector<std::size_t>> edges(passes.size());
    std::vector<std::size_t> indegree(passes.size(), 0);
    std::unordered_map<std::uint32_t, std::size_t> lastWriter;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> readersSinceWrite;
    const auto addEdge = [&](std::size_t from, std::size_t to) {
        if (from == to) return;
        auto& outgoing = edges[from];
        if (std::find(outgoing.begin(), outgoing.end(), to) == outgoing.end()) {
            outgoing.push_back(to);
            ++indegree[to];
            plan.dependencies.push_back({from, to});
        }
    };
    // executionOrder is already topological. Replaying the graph's
    // read/write hazard rule gives the scheduler the full pass dependency
    // relation without exposing RenderGraph's private compiler state.
    for (const auto passIndex : executionOrder) {
        const auto& pass = passes[passIndex];
        for (const auto resource : pass.reads) {
            const auto writer = lastWriter.find(resource.id);
            if (writer != lastWriter.end()) addEdge(writer->second, passIndex);
            readersSinceWrite[resource.id].push_back(passIndex);
        }
        for (const auto resource : pass.writes) {
            const auto writer = lastWriter.find(resource.id);
            if (writer != lastWriter.end()) addEdge(writer->second, passIndex);
            for (const auto reader : readersSinceWrite[resource.id]) addEdge(reader, passIndex);
            readersSinceWrite[resource.id].clear();
            lastWriter[resource.id] = passIndex;
        }
    }

    std::vector<std::size_t> level(passes.size(), 0);
    for (const auto passIndex : executionOrder) {
        for (const auto next : edges[passIndex]) level[next] = std::max(level[next], level[passIndex] + 1);
    }
    std::size_t levelCount = 0;
    for (const auto passIndex : executionOrder) levelCount = std::max(levelCount, level[passIndex] + 1);
    plan.levels.resize(levelCount);
    for (std::size_t index = 0; index < levelCount; ++index) plan.levels[index].index = static_cast<std::uint32_t>(index);

    plan.tasks.reserve(count);
    for (std::size_t order = 0; order < executionOrder.size(); ++order) {
        const auto passIndex = executionOrder[order];
        const auto& pass = passes[passIndex];
        const auto batch = passToBatch.find(passIndex);
        if (batch == passToBatch.end()) {
            set_plan_error(plan, error, "render graph scheduler found a pass without a queue batch");
            return plan;
        }
        CommandRecordingTask task;
        task.passIndex = passIndex;
        task.recordingOrder = order;
        task.dependencyLevel = static_cast<std::uint32_t>(level[passIndex]);
        task.queueBatch = batch->second;
        task.queue = pass.queue;
        task.passName = pass.name;
        task.reads = pass.reads;
        task.writes = pass.writes;
        task.accesses = pass.accesses;
        for (const auto access : pass.accesses) {
            task.resourceLocks.push_back({access.resource, access.usage, default_policy(access.resource, access.usage)});
        }
        plan.tasks.push_back(std::move(task));
        plan.levels[level[passIndex]].passIndices.push_back(passIndex);
    }
    plan.results.resize(plan.tasks.size());
    plan.valid = true;
    return plan;
}

CommandRecordingPlan RenderGraphScheduler::prepare(const RenderGraph& graph,
                                                    ICommandRecordingExecutor& executor) const {
    return prepare(graph, executor, Options{});
}

CommandRecordingPlan RenderGraphScheduler::prepare(const RenderGraph& graph,
                                                    ICommandRecordingExecutor& executor,
                                                    const Options& options) const {
    CommandRecordingPlan plan = build(graph);
    if (!plan.valid || plan.tasks.empty()) return plan;

    const auto policy = options.resourcePolicy ? options.resourcePolicy :
        Options::ResourcePolicy(default_policy);
    const std::size_t maxConcurrency = std::max<std::size_t>(1, options.maxConcurrency == 0
        ? default_concurrency() : options.maxConcurrency);

    std::unordered_map<ResourceKey, std::shared_ptr<std::shared_mutex>, ResourceKeyHash> resourceMutexes;
    for (const auto& task : plan.tasks) {
        for (const auto access : task.accesses) {
            const ResourceKey key{access.resource.id, access.resource.kind};
            if (resourceMutexes.find(key) == resourceMutexes.end()) {
                resourceMutexes.emplace(key, std::make_shared<std::shared_mutex>());
            }
        }
    }

    std::vector<std::size_t> taskByPass(graph.passes().size(), std::numeric_limits<std::size_t>::max());
    for (std::size_t index = 0; index < plan.tasks.size(); ++index) taskByPass[plan.tasks[index].passIndex] = index;

    bool stop = false;
    for (const auto& level : plan.levels) {
        if (stop) break;
        std::vector<std::size_t> pending;
        for (const auto passIndex : level.passIndices) {
            const auto taskIndex = taskByPass[passIndex];
            bool mainThreadOnly = false;
            for (auto& lock : plan.tasks[taskIndex].resourceLocks) {
                try {
                    lock.mode = policy(lock.resource, lock.usage);
                } catch (const std::exception& exception) {
                    plan.results[taskIndex] = {CommandRecordingStatus::Failed,
                        std::string("resource policy threw: ") + exception.what(), {}};
                    plan.usedFallback = true;
                    stop = true;
                    break;
                } catch (...) {
                    plan.results[taskIndex] = {CommandRecordingStatus::Failed,
                        "resource policy threw an unknown exception", {}};
                    plan.usedFallback = true;
                    stop = true;
                    break;
                }
                if (lock.mode == CommandResourceAccessMode::MainThreadOnly) mainThreadOnly = true;
            }
            if (stop) break;
            if (mainThreadOnly) {
                plan.results[taskIndex] = {CommandRecordingStatus::Fallback,
                    "resource policy requires main-thread recording", {}};
                plan.usedFallback = true;
                stop = true;
                break;
            }
            pending.push_back(taskIndex);
        }
        if (stop) break;

        for (std::size_t offset = 0; offset < pending.size() && !stop; offset += maxConcurrency) {
            const auto end = std::min(pending.size(), offset + maxConcurrency);
            std::vector<CommandRecordingResult> results(end - offset);
            std::vector<std::thread> workers;
            workers.reserve(end - offset);
            for (std::size_t position = offset; position < end; ++position) {
                workers.emplace_back([&, position] {
                    const auto taskIndex = pending[position];
                    const auto& task = plan.tasks[taskIndex];
                    try {
                        // Locks are acquired in sorted resource-key order so a
                        // future executor policy cannot introduce lock cycles.
                        std::vector<std::pair<ResourceKey, CommandResourceAccessMode>> keys;
                        for (const auto lock : task.resourceLocks) {
                            keys.push_back({{lock.resource.id, lock.resource.kind}, lock.mode});
                        }
                        std::sort(keys.begin(), keys.end(), [](const auto& lhs, const auto& rhs) {
                            if (lhs.first.id != rhs.first.id) return lhs.first.id < rhs.first.id;
                            return static_cast<int>(lhs.first.kind) < static_cast<int>(rhs.first.kind);
                        });
                        keys.erase(std::unique(keys.begin(), keys.end(), [](const auto& lhs, const auto& rhs) {
                            return lhs.first == rhs.first;
                        }), keys.end());
                        std::vector<std::shared_lock<std::shared_mutex>> sharedLocks;
                        std::vector<std::unique_lock<std::shared_mutex>> exclusiveLocks;
                        for (const auto& key : keys) {
                            const auto mutex = resourceMutexes.at(key.first);
                            if (key.second == CommandResourceAccessMode::Exclusive) {
                                exclusiveLocks.emplace_back(*mutex);
                            } else {
                                sharedLocks.emplace_back(*mutex);
                            }
                        }
                        results[position - offset] = executor.record(task);
                        if (results[position - offset].status == CommandRecordingStatus::Pending) {
                            results[position - offset].status = CommandRecordingStatus::Failed;
                            results[position - offset].diagnostic = "recording executor returned Pending";
                        }
                    } catch (const std::exception& exception) {
                        results[position - offset] = {CommandRecordingStatus::Failed,
                            std::string("recording executor threw: ") + exception.what(), {}};
                    } catch (...) {
                        results[position - offset] = {CommandRecordingStatus::Failed,
                            "recording executor threw an unknown exception", {}};
                    }
                });
            }
            for (auto& worker : workers) worker.join();
            for (std::size_t index = 0; index < results.size(); ++index) {
                const auto taskIndex = pending[offset + index];
                plan.results[taskIndex] = std::move(results[index]);
                if (plan.results[taskIndex].status != CommandRecordingStatus::Recorded) {
                    plan.usedFallback = true;
                    stop = true;
                }
            }
        }
    }

    if (stop) {
        for (std::size_t index = 0; index < plan.results.size(); ++index) {
            if (plan.results[index].status == CommandRecordingStatus::Pending) {
                plan.results[index] = {CommandRecordingStatus::Fallback,
                    "not dispatched after an earlier recording failure", {}};
            }
        }
    }
    for (std::size_t index = 0; index < plan.results.size(); ++index) {
        if (plan.results[index].status == CommandRecordingStatus::Failed ||
            plan.results[index].status == CommandRecordingStatus::Fallback) {
            plan.fallbackPassIndices.push_back(plan.tasks[index].passIndex);
        }
    }
    return plan;
}

} // namespace shinkou::render

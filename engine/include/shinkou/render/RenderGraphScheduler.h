#pragma once

#include "shinkou/render/RenderBackend.h"
#include "shinkou/render/RenderGraph.h"
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace shinkou::render {

// This is a recording-time policy, not a backend resource lifetime policy.
// Read-only accesses may be recorded concurrently by default. Writes are
// exclusive. MainThreadOnly is useful for resources whose command encoder or
// producer-side state is not safe to touch from a worker.
enum class CommandResourceAccessMode {
    SharedRead,
    Exclusive,
    MainThreadOnly
};

struct CommandResourceLock {
    ResourceHandle resource{};
    ResourceUsage usage{ResourceUsage::Unknown};
    CommandResourceAccessMode mode{CommandResourceAccessMode::SharedRead};
};

struct CommandRecordingTask {
    std::size_t passIndex{0};
    std::size_t recordingOrder{0};
    std::uint32_t dependencyLevel{0};
    std::uint32_t queueBatch{0};
    RenderQueue queue{RenderQueue::Graphics};
    std::string passName;
    std::vector<ResourceHandle> reads;
    std::vector<ResourceHandle> writes;
    std::vector<ResourceAccess> accesses;
    std::vector<CommandResourceLock> resourceLocks;

    // The returned context owns no backend state. The name view refers to
    // this task; the access vectors are copied into the context value.
    RenderPassContext pass_context() const {
        return {passName, reads, writes, accesses, queue};
    }
};

enum class CommandRecordingStatus {
    Pending,
    Recorded,
    Failed,
    Fallback
};

struct CommandRecordingResult {
    CommandRecordingStatus status{CommandRecordingStatus::Pending};
    std::string diagnostic;
    // The scheduler deliberately does not interpret this. A backend-neutral
    // executor may store an immutable command list/token here for a later
    // integration layer.
    std::shared_ptr<const void> payload;
};

class ICommandRecordingExecutor {
public:
    virtual ~ICommandRecordingExecutor() = default;
    virtual CommandRecordingResult record(const CommandRecordingTask& task) = 0;
};

class FunctionCommandRecordingExecutor final : public ICommandRecordingExecutor {
public:
    using Function = std::function<CommandRecordingResult(const CommandRecordingTask&)>;

    explicit FunctionCommandRecordingExecutor(Function function) : function_(std::move(function)) {}
    CommandRecordingResult record(const CommandRecordingTask& task) override {
        return function_ ? function_(task) : CommandRecordingResult{};
    }

private:
    Function function_;
};

struct CommandRecordingDependency {
    std::size_t producerPass{0};
    std::size_t consumerPass{0};
};

struct CommandRecordingLevel {
    std::uint32_t index{0};
    std::vector<std::size_t> passIndices;
};

struct CommandRecordingBatch {
    std::uint32_t index{0};
    RenderQueue queue{RenderQueue::Graphics};
    std::vector<std::size_t> passIndices;
    std::vector<std::uint32_t> waitBatches;
};

struct CommandRecordingPlan {
    bool valid{false};
    bool usedFallback{false};
    std::string error;
    std::vector<CommandRecordingTask> tasks;
    std::vector<CommandRecordingDependency> dependencies;
    std::vector<CommandRecordingLevel> levels;
    std::vector<CommandRecordingBatch> batches;
    // This is the only order in which a later integration layer may submit
    // queue batches. It is intentionally independent of worker completion.
    std::vector<std::size_t> submissionOrder;
    std::vector<CommandRecordingResult> results;
    std::vector<std::size_t> fallbackPassIndices;

    const CommandRecordingTask* task_for_pass(std::size_t passIndex) const noexcept;
    const CommandRecordingResult* result_for_pass(std::size_t passIndex) const noexcept;
};

class RenderGraphScheduler {
public:
    struct Options {
        // Zero selects a bounded default based on hardware_concurrency().
        std::size_t maxConcurrency{0};
        using ResourcePolicy = std::function<CommandResourceAccessMode(
            ResourceHandle, ResourceUsage)>;
        ResourcePolicy resourcePolicy;
    };

    // Builds only dependency levels, recording tasks, queue batches and the
    // main-thread submission order. It never invokes a pass callback or a
    // backend method.
    CommandRecordingPlan build(const RenderGraph& graph, std::string* error = nullptr) const;

    // Runs worker recording level by level. A worker failure stops dispatching
    // later levels and marks all not-yet-recorded tasks for main-thread
    // fallback, preserving the graph's submission order.
    CommandRecordingPlan prepare(const RenderGraph& graph,
                                 ICommandRecordingExecutor& executor) const;
    CommandRecordingPlan prepare(const RenderGraph& graph,
                                 ICommandRecordingExecutor& executor,
                                 const Options& options) const;
};

} // namespace shinkou::render

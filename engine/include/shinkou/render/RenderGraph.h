#pragma once

#include "shinkou/render/RenderBackend.h"
#include "shinkou/render/GpuMemoryAllocator.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <cstdint>

namespace shinkou::render {
struct RenderPassTiming {
    std::string name;
    std::uint64_t cpuNanoseconds{0};
    std::uint64_t gpuNanoseconds{0};
    std::uint64_t drawCalls{0};
    std::uint64_t barriers{0};
    std::uint64_t resourceReads{0};
    std::uint64_t resourceWrites{0};
};

struct RenderGraphDiagnostics {
    std::uint64_t compileNanoseconds{0};
    std::uint64_t executeNanoseconds{0};
    std::size_t passCount{0};
    std::size_t resourceCount{0};
    std::size_t physicalTextureCount{0};
    std::size_t aliasedTextureCount{0};
    std::size_t physicalBufferCount{0};
    std::size_t aliasedBufferCount{0};
    std::size_t plannedTransitionCount{0};
    std::size_t validationErrorCount{0};
    std::size_t crossQueueDependencyCount{0};
    std::size_t graphicsPassCount{0};
    std::size_t computePassCount{0};
    std::size_t copyPassCount{0};
    std::size_t culledPassCount{0};
    std::size_t hdrPassCount{0};
    std::size_t executedPassCount{0};
    std::size_t failedPassIndex{static_cast<std::size_t>(-1)};
    std::uint32_t failedQueueBatch{static_cast<std::uint32_t>(-1)};
    bool executionFailed{false};
    bool gpuTimingDelayed{false};
    std::string lastError;
};

struct RenderResourceTransition {
    ResourceHandle resource{};
    ResourceUsage before{ResourceUsage::Unknown};
    ResourceUsage after{ResourceUsage::Unknown};
    std::size_t passIndex{0};
};

struct RenderQueueDependency {
    std::size_t producerPass{0};
    std::size_t consumerPass{0};
    RenderQueue producerQueue{RenderQueue::Graphics};
    RenderQueue consumerQueue{RenderQueue::Graphics};
    ResourceHandle resource{};
};

struct RenderQueueBatch {
    std::uint32_t index{0};
    RenderQueue queue{RenderQueue::Graphics};
    std::vector<std::size_t> passIndices;
    std::vector<std::uint32_t> waitBatches;
};

class RenderGraph {
public:
    using PassCallback = std::function<void(IRenderBackend&, const RenderPassContext&)>;

    struct Pass {
        std::string name;
        std::vector<ResourceHandle> reads;
        std::vector<ResourceHandle> writes;
        std::vector<ResourceAccess> accesses;
        PassCallback callback;
        RenderQueue queue{RenderQueue::Graphics};
        bool sideEffect{true};
        bool clearAttachments{true};
    };

    struct ResourceNode {
        ResourceHandle handle;
        ResourceHandle physicalHandle{};
        ResourceDesc description;
        bool external{false};
    };

private:
    std::vector<Pass> passes_;
    std::vector<ResourceNode> resources_;
    // Graph-owned resources use a dedicated high-bit namespace so imported
    // renderer handles cannot collide with transient logical handles.
    std::uint32_t nextResource_{0x80000000u};
    std::vector<std::size_t> executionOrder_;
    std::vector<RenderResourceTransition> transitions_;
    std::vector<RenderQueueDependency> queueDependencies_;
    std::vector<RenderQueueBatch> queueBatches_;
    std::vector<RenderPassTiming> passTimings_;
    TransientAliasPlan aliasPlan_;
    RenderGraphDiagnostics diagnostics_{};
    bool compiled_{false};
    ResourceHandle allocate_resource(ResourceKind kind) noexcept;

public:
    ResourceHandle create_texture(const TextureDesc&);
    ResourceHandle create_sampler(const SamplerDesc&);
    void import_resource(ResourceHandle, const ResourceDesc&);
    void import_texture(ResourceHandle, const TextureDesc&);
    ResourceHandle create_depth_stencil(const TextureDesc&);
    ResourceHandle create_buffer(const BufferDesc&);
    ResourceHandle create_shader(const ShaderDesc&);
    ResourceHandle create_pipeline(const PipelineDesc&);
    ResourceHandle create_material(const MaterialDesc&);
    bool contains_resource(ResourceHandle) const noexcept;
    bool is_external_resource(ResourceHandle) const noexcept;
    std::size_t add_pass(std::string name, std::vector<ResourceHandle> reads,
                         std::vector<ResourceHandle> writes, PassCallback callback,
                         RenderQueue queue = RenderQueue::Graphics, bool sideEffect = true,
                         bool clearAttachments = true);
    std::size_t add_pass(std::string name, std::vector<ResourceAccess> accesses,
                         PassCallback callback, RenderQueue queue = RenderQueue::Graphics,
                         bool sideEffect = true, bool clearAttachments = true);
    bool compile(std::string* error = nullptr);
    void execute(IRenderBackend& backend, std::string* error = nullptr,
                 const std::function<void(IRenderBackend&)>& beforePresent = {});
    void reset();
    const std::vector<Pass>& passes() const noexcept { return passes_; }
    const std::vector<ResourceNode>& resources() const noexcept { return resources_; }
    ResourceHandle physical_resource(ResourceHandle logical) const noexcept;
    const std::vector<std::size_t>& execution_order() const noexcept { return executionOrder_; }
    const std::vector<RenderResourceTransition>& transitions() const noexcept { return transitions_; }
    const std::vector<RenderQueueDependency>& queue_dependencies() const noexcept { return queueDependencies_; }
    const std::vector<RenderQueueBatch>& queue_batches() const noexcept { return queueBatches_; }
    const TransientAliasPlan& alias_plan() const noexcept { return aliasPlan_; }
    const TransientAliasPlan& transient_alias_plan() const noexcept { return aliasPlan_; }
    const std::vector<RenderPassTiming>& pass_timings() const noexcept { return passTimings_; }
    const RenderGraphDiagnostics& diagnostics() const noexcept { return diagnostics_; }
};
}

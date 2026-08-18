#pragma once

#include "shinkou/render/RenderGraph.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::render {

constexpr std::size_t kInvalidRenderPassIndex = std::numeric_limits<std::size_t>::max();

// These types intentionally contain no API-specific handles.  A D3D12 or Vulkan
// backend can populate them without making the core diagnostics module depend on
// either SDK.
struct GpuMemoryHeapSnapshot {
    std::uint32_t index{0};
    std::string name;
    std::uint64_t budgetBytes{0};
    std::uint64_t usageBytes{0};
    std::uint64_t availableBytes{0};
    std::uint64_t reservedBytes{0};
    bool budgetKnown{false};
};

struct GpuMemoryBudgetSnapshot {
    BackendApi api{BackendApi::Null};
    std::uint64_t timestampNanoseconds{0};
    std::string source;
    std::string error;
    std::vector<GpuMemoryHeapSnapshot> heaps;
    bool valid{false};
    bool estimated{false};
};

struct RenderGraphResourceUse {
    std::size_t passIndex{kInvalidRenderPassIndex};
    RenderQueue queue{RenderQueue::Graphics};
    ResourceUsage usage{ResourceUsage::Unknown};
};

struct RenderGraphResourceLifetime {
    ResourceHandle logical{};
    ResourceHandle physical{};
    bool external{false};
    bool aliased{false};
    std::size_t firstPass{kInvalidRenderPassIndex};
    std::size_t lastPass{kInvalidRenderPassIndex};
    std::uint64_t estimatedBytes{0};
    std::vector<RenderGraphResourceUse> uses;
};

struct RenderTimingReport {
    std::uint64_t cpuNanoseconds{0};
    std::uint64_t gpuNanoseconds{0};
    bool cpuTimingAvailable{false};
    bool gpuTimingAvailable{false};
    bool cpuTimingDelayed{false};
    bool gpuTimingDelayed{false};
};

struct RenderGraphBarrierReport {
    ResourceHandle resource{};
    ResourceUsage before{ResourceUsage::Unknown};
    ResourceUsage after{ResourceUsage::Unknown};
    std::size_t passIndex{kInvalidRenderPassIndex};
    RenderQueue queue{RenderQueue::Graphics};
};

struct RenderGraphAliasReport {
    ResourceHandle logical{};
    ResourceHandle physical{};
    std::size_t firstPass{kInvalidRenderPassIndex};
    std::size_t lastPass{kInvalidRenderPassIndex};
    std::uint64_t estimatedBytes{0};
};

struct RenderGraphReport {
    bool compiled{false};
    RenderGraphDiagnostics diagnostics{};
    std::vector<std::size_t> executionOrder;
    std::vector<RenderGraphResourceLifetime> lifetimes;
    std::vector<RenderGraphBarrierReport> barriers;
    std::vector<RenderGraphAliasReport> aliases;
    std::vector<RenderQueueDependency> queueDependencies;
    std::vector<RenderQueueBatch> queueBatches;
    std::vector<RenderPassTiming> passTimings;
    RenderTimingReport timing;
};

enum class CaptureMarkerPhase { Begin, End, Instant };

struct RenderCaptureMarker {
    std::uint64_t id{0};
    std::uint64_t timestampNanoseconds{0};
    std::uint64_t frameIndex{0};
    std::uint32_t depth{0};
    std::string name;
    std::string category;
    RenderQueue queue{RenderQueue::Graphics};
    CaptureMarkerPhase phase{CaptureMarkerPhase::Instant};
};

struct RenderCaptureMetadata {
    std::string tool{"none"};
    std::string name;
    std::string filePath;
    std::uint64_t beginTimestampNanoseconds{0};
    std::uint64_t endTimestampNanoseconds{0};
    std::uint64_t markerCount{0};
    bool active{false};
    bool allowAsync{true};
};

enum class RenderBudgetSeverity { Warning, Critical };

struct RenderBudgetViolation {
    std::uint64_t frameIndex{0};
    std::uint64_t timestampNanoseconds{0};
    std::uint32_t heapIndex{0};
    std::string heapName;
    std::uint64_t budgetBytes{0};
    std::uint64_t usageBytes{0};
    std::uint64_t overBytes{0};
    RenderBudgetSeverity severity{RenderBudgetSeverity::Warning};
};

struct RenderBudgetFaultCorrelation {
    bool correlated{false};
    std::uint64_t violationFrameIndex{0};
    std::uint64_t violationTimestampNanoseconds{0};
    std::uint64_t overBytes{0};
    std::string reason;
};

enum class DeviceFaultSource { None, Backend, D3D12Dred, VulkanDeviceFault };

struct DeviceFaultDiagnostic {
    BackendApi api{BackendApi::Null};
    DeviceFaultSource source{DeviceFaultSource::None};
    std::uint64_t timestampNanoseconds{0};
    std::uint64_t faultAddress{0};
    std::string deviceName;
    std::string reason;
    std::string description;
    std::vector<std::string> breadcrumbs;
    std::vector<std::string> pageFaultOperations;
    std::vector<std::string> vendorInfo;
    bool deviceLost{false};
    bool recovered{false};
};

// Optional hooks are the only integration point a platform backend needs.  A
// D3D12 implementation may map DRED breadcrumbs/page-fault data here, while a
// Vulkan implementation may map VK_EXT_device_fault data here.
struct RenderDiagnosticsHooks {
    std::function<bool(GpuMemoryBudgetSnapshot&)> queryMemoryBudget;
    std::function<bool(DeviceFaultDiagnostic&)> queryDeviceFault;
    std::function<void(const RenderCaptureMarker&)> emitCaptureMarker;
};

struct RenderDiagnosticsSnapshot {
    std::uint64_t frameIndex{0};
    std::uint64_t timestampNanoseconds{0};
    GpuMemoryBudgetSnapshot memory;
    RenderGraphReport graph;
    std::vector<RenderCaptureMarker> captureMarkers;
    RenderCaptureMetadata captureMetadata;
    std::vector<RenderBudgetViolation> budgetViolations;
    RenderBudgetFaultCorrelation budgetFaultCorrelation;
    DeviceFaultDiagnostic deviceFault;
};

GpuMemoryBudgetSnapshot query_gpu_memory_budget(const RenderDiagnosticsHooks& hooks,
                                                BackendApi api = BackendApi::Null);
DeviceFaultDiagnostic query_device_fault(const RenderDiagnosticsHooks& hooks,
                                          BackendApi api = BackendApi::Null);
RenderGraphReport build_render_graph_report(const RenderGraph& graph);

class RenderCaptureRecorder {
public:
    explicit RenderCaptureRecorder(RenderDiagnosticsHooks hooks = {});

    void set_hooks(RenderDiagnosticsHooks hooks);
    void begin(std::string_view name, std::string_view category = {},
               std::uint64_t frameIndex = 0, RenderQueue queue = RenderQueue::Graphics);
    void end(std::string_view name = {}, std::string_view category = {},
             std::uint64_t frameIndex = 0, RenderQueue queue = RenderQueue::Graphics);
    void instant(std::string_view name, std::string_view category = {},
                 std::uint64_t frameIndex = 0, RenderQueue queue = RenderQueue::Graphics);
    void clear();

    const std::vector<RenderCaptureMarker>& markers() const noexcept { return markers_; }

private:
    void record(CaptureMarkerPhase phase, std::string name, std::string category,
                std::uint64_t frameIndex, RenderQueue queue, std::uint32_t depth);

    RenderDiagnosticsHooks hooks_;
    std::vector<RenderCaptureMarker> markers_;
    std::vector<std::string> scopeStack_;
    std::uint64_t nextId_{1};
};

RenderDiagnosticsSnapshot collect_diagnostics(const RenderDiagnosticsHooks& hooks,
                                               const RenderGraph& graph,
                                               std::vector<RenderCaptureMarker> captureMarkers = {},
                                               BackendApi api = BackendApi::Null);

std::string serialize_json(const RenderDiagnosticsSnapshot& snapshot);
std::string serialize_text(const RenderDiagnosticsSnapshot& snapshot);

const char* to_string(BackendApi api) noexcept;
const char* to_string(RenderQueue queue) noexcept;
const char* to_string(ResourceKind kind) noexcept;
const char* to_string(ResourceUsage usage) noexcept;
const char* to_string(CaptureMarkerPhase phase) noexcept;
const char* to_string(RenderBudgetSeverity severity) noexcept;
const char* to_string(DeviceFaultSource source) noexcept;

} // namespace shinkou::render

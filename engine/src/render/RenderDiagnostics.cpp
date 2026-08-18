#include "shinkou/render/RenderDiagnostics.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>
#include <string>

namespace shinkou::render {
namespace {

std::uint64_t now_nanoseconds() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    return left > max - right ? max : left + right;
}

std::uint64_t saturating_multiply(std::uint64_t left, std::uint64_t right) noexcept {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    return left != 0 && right > max / left ? max : left * right;
}

std::uint32_t format_bytes_per_pixel(const std::string& format) noexcept {
    if (format == "r8") return 1;
    if (format == "rg8") return 2;
    if (format == "r16f") return 2;
    if (format == "rg16f") return 4;
    if (format == "r32f") return 4;
    if (format == "rgba16f") return 8;
    if (format == "rgba32f") return 16;
    if (format == "d24s8" || format == "d32" || format == "depth24stencil8") return 4;
    if (format == "rgba8" || format == "bgra8" || format == "rgb10a2") return 4;
    return 0;
}

std::uint64_t estimate_bytes(const ResourceDesc& description) noexcept {
    if (const auto* buffer = std::get_if<BufferDesc>(&description)) {
        return static_cast<std::uint64_t>(buffer->size);
    }
    const auto* texture = std::get_if<TextureDesc>(&description);
    if (!texture) return 0;
    const auto bytesPerPixel = format_bytes_per_pixel(texture->format);
    if (bytesPerPixel == 0) return 0;

    std::uint64_t total = 0;
    for (std::uint32_t mip = 0; mip < texture->mipLevels; ++mip) {
        const auto width = std::max<std::uint32_t>(1, texture->width >> std::min(mip, 31u));
        const auto height = std::max<std::uint32_t>(1, texture->height >> std::min(mip, 31u));
        auto mipBytes = saturating_multiply(width, height);
        mipBytes = saturating_multiply(mipBytes, texture->layers);
        mipBytes = saturating_multiply(mipBytes, bytesPerPixel);
        total = saturating_add(total, mipBytes);
    }
    return total;
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    for (const char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                result += "\\u00";
                constexpr char digits[] = "0123456789abcdef";
                result += digits[(static_cast<unsigned char>(character) >> 4) & 0xf];
                result += digits[static_cast<unsigned char>(character) & 0xf];
            } else {
                result += character;
            }
            break;
        }
    }
    return result;
}

void json_string(std::ostringstream& out, std::string_view value) {
    out << '"' << json_escape(value) << '"';
}

void json_handle(std::ostringstream& out, ResourceHandle handle) {
    out << "{\"id\":" << handle.id << ",\"kind\":";
    json_string(out, to_string(handle.kind));
    out << '}';
}

void json_string_array(std::ostringstream& out, const std::vector<std::string>& values) {
    out << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) out << ',';
        json_string(out, values[index]);
    }
    out << ']';
}

void json_diagnostics(std::ostringstream& out, const RenderGraphDiagnostics& diagnostics) {
    out << "{\"compileNanoseconds\":" << diagnostics.compileNanoseconds
        << ",\"executeNanoseconds\":" << diagnostics.executeNanoseconds
        << ",\"passCount\":" << diagnostics.passCount
        << ",\"resourceCount\":" << diagnostics.resourceCount
        << ",\"physicalTextureCount\":" << diagnostics.physicalTextureCount
        << ",\"aliasedTextureCount\":" << diagnostics.aliasedTextureCount
        << ",\"physicalBufferCount\":" << diagnostics.physicalBufferCount
        << ",\"aliasedBufferCount\":" << diagnostics.aliasedBufferCount
        << ",\"plannedTransitionCount\":" << diagnostics.plannedTransitionCount
        << ",\"validationErrorCount\":" << diagnostics.validationErrorCount
        << ",\"crossQueueDependencyCount\":" << diagnostics.crossQueueDependencyCount
        << ",\"graphicsPassCount\":" << diagnostics.graphicsPassCount
        << ",\"computePassCount\":" << diagnostics.computePassCount
        << ",\"copyPassCount\":" << diagnostics.copyPassCount
        << ",\"culledPassCount\":" << diagnostics.culledPassCount
        << ",\"hdrPassCount\":" << diagnostics.hdrPassCount
        << ",\"executedPassCount\":" << diagnostics.executedPassCount
        << ",\"failedPassIndex\":";
    if (diagnostics.failedPassIndex == kInvalidRenderPassIndex) out << "null";
    else out << diagnostics.failedPassIndex;
    out << ",\"failedQueueBatch\":";
    if (diagnostics.failedQueueBatch == std::numeric_limits<std::uint32_t>::max()) out << "null";
    else out << diagnostics.failedQueueBatch;
    out << ",\"executionFailed\":" << (diagnostics.executionFailed ? "true" : "false")
        << ",\"gpuTimingDelayed\":" << (diagnostics.gpuTimingDelayed ? "true" : "false")
        << ",\"lastError\":";
    json_string(out, diagnostics.lastError);
    out << '}';
}

void json_timing(std::ostringstream& out, const RenderTimingReport& timing) {
    out << "{\"cpuNanoseconds\":" << timing.cpuNanoseconds
        << ",\"gpuNanoseconds\":" << timing.gpuNanoseconds
        << ",\"cpuTimingAvailable\":" << (timing.cpuTimingAvailable ? "true" : "false")
        << ",\"gpuTimingAvailable\":" << (timing.gpuTimingAvailable ? "true" : "false")
        << ",\"cpuTimingDelayed\":" << (timing.cpuTimingDelayed ? "true" : "false")
        << ",\"gpuTimingDelayed\":" << (timing.gpuTimingDelayed ? "true" : "false")
        << '}';
}

void json_pass_timing(std::ostringstream& out, const RenderPassTiming& timing) {
    out << "{\"name\":";
    json_string(out, timing.name);
    out << ",\"cpuNanoseconds\":" << timing.cpuNanoseconds
        << ",\"gpuNanoseconds\":" << timing.gpuNanoseconds
        << ",\"drawCalls\":" << timing.drawCalls
        << ",\"barriers\":" << timing.barriers
        << ",\"resourceReads\":" << timing.resourceReads
        << ",\"resourceWrites\":" << timing.resourceWrites << '}';
}

void json_memory(std::ostringstream& out, const GpuMemoryBudgetSnapshot& memory) {
    out << "{\"api\":";
    json_string(out, to_string(memory.api));
    out << ",\"timestampNanoseconds\":" << memory.timestampNanoseconds
        << ",\"source\":";
    json_string(out, memory.source);
    out << ",\"error\":";
    json_string(out, memory.error);
    out << ",\"valid\":" << (memory.valid ? "true" : "false")
        << ",\"estimated\":" << (memory.estimated ? "true" : "false")
        << ",\"heaps\":[";
    for (std::size_t index = 0; index < memory.heaps.size(); ++index) {
        if (index != 0) out << ',';
        const auto& heap = memory.heaps[index];
        out << "{\"index\":" << heap.index << ",\"name\":";
        json_string(out, heap.name);
        out << ",\"budgetBytes\":" << heap.budgetBytes
            << ",\"usageBytes\":" << heap.usageBytes
            << ",\"availableBytes\":" << heap.availableBytes
            << ",\"reservedBytes\":" << heap.reservedBytes
            << ",\"budgetKnown\":" << (heap.budgetKnown ? "true" : "false") << '}';
    }
    out << "]}";
}

void json_graph(std::ostringstream& out, const RenderGraphReport& graph) {
    out << "{\"compiled\":" << (graph.compiled ? "true" : "false")
        << ",\"diagnostics\":";
    json_diagnostics(out, graph.diagnostics);
    out << ",\"executionOrder\":[";
    for (std::size_t index = 0; index < graph.executionOrder.size(); ++index) {
        if (index != 0) out << ',';
        out << graph.executionOrder[index];
    }
    out << "],\"lifetimes\":[";
    for (std::size_t index = 0; index < graph.lifetimes.size(); ++index) {
        if (index != 0) out << ',';
        const auto& lifetime = graph.lifetimes[index];
        out << "{\"logical\":";
        json_handle(out, lifetime.logical);
        out << ",\"physical\":";
        json_handle(out, lifetime.physical);
        out << ",\"external\":" << (lifetime.external ? "true" : "false")
            << ",\"aliased\":" << (lifetime.aliased ? "true" : "false")
            << ",\"firstPass\":";
        if (lifetime.firstPass == kInvalidRenderPassIndex) out << "null";
        else out << lifetime.firstPass;
        out << ",\"lastPass\":";
        if (lifetime.lastPass == kInvalidRenderPassIndex) out << "null";
        else out << lifetime.lastPass;
        out << ",\"estimatedBytes\":" << lifetime.estimatedBytes << ",\"uses\":[";
        for (std::size_t useIndex = 0; useIndex < lifetime.uses.size(); ++useIndex) {
            if (useIndex != 0) out << ',';
            const auto& use = lifetime.uses[useIndex];
            out << "{\"passIndex\":" << use.passIndex << ",\"queue\":";
            json_string(out, to_string(use.queue));
            out << ",\"usage\":";
            json_string(out, to_string(use.usage));
            out << '}';
        }
        out << "]}";
    }
    out << "],\"barriers\":[";
    for (std::size_t index = 0; index < graph.barriers.size(); ++index) {
        if (index != 0) out << ',';
        const auto& barrier = graph.barriers[index];
        out << "{\"resource\":";
        json_handle(out, barrier.resource);
        out << ",\"before\":";
        json_string(out, to_string(barrier.before));
        out << ",\"after\":";
        json_string(out, to_string(barrier.after));
        out << ",\"passIndex\":" << barrier.passIndex << ",\"queue\":";
        json_string(out, to_string(barrier.queue));
        out << '}';
    }
    out << "],\"aliases\":[";
    for (std::size_t index = 0; index < graph.aliases.size(); ++index) {
        if (index != 0) out << ',';
        const auto& alias = graph.aliases[index];
        out << "{\"logical\":";
        json_handle(out, alias.logical);
        out << ",\"physical\":";
        json_handle(out, alias.physical);
        out << ",\"firstPass\":";
        if (alias.firstPass == kInvalidRenderPassIndex) out << "null";
        else out << alias.firstPass;
        out << ",\"lastPass\":";
        if (alias.lastPass == kInvalidRenderPassIndex) out << "null";
        else out << alias.lastPass;
        out << ",\"estimatedBytes\":" << alias.estimatedBytes << '}';
    }
    out << "],\"queueDependencies\":[";
    for (std::size_t index = 0; index < graph.queueDependencies.size(); ++index) {
        if (index != 0) out << ',';
        const auto& dependency = graph.queueDependencies[index];
        out << "{\"producerPass\":" << dependency.producerPass
            << ",\"consumerPass\":" << dependency.consumerPass << ",\"producerQueue\":";
        json_string(out, to_string(dependency.producerQueue));
        out << ",\"consumerQueue\":";
        json_string(out, to_string(dependency.consumerQueue));
        out << ",\"resource\":";
        json_handle(out, dependency.resource);
        out << '}';
    }
    out << "],\"queueBatches\":[";
    for (std::size_t index = 0; index < graph.queueBatches.size(); ++index) {
        if (index != 0) out << ',';
        const auto& batch = graph.queueBatches[index];
        out << "{\"index\":" << batch.index << ",\"queue\":";
        json_string(out, to_string(batch.queue));
        out << ",\"passIndices\":[";
        for (std::size_t pass = 0; pass < batch.passIndices.size(); ++pass) {
            if (pass != 0) out << ',';
            out << batch.passIndices[pass];
        }
        out << "],\"waitBatches\":[";
        for (std::size_t wait = 0; wait < batch.waitBatches.size(); ++wait) {
            if (wait != 0) out << ',';
            out << batch.waitBatches[wait];
        }
        out << "]}";
    }
    out << "],\"passTimings\":[";
    for (std::size_t index = 0; index < graph.passTimings.size(); ++index) {
        if (index != 0) out << ',';
        json_pass_timing(out, graph.passTimings[index]);
    }
    out << "],\"timing\":";
    json_timing(out, graph.timing);
    out << "}";
}

void json_fault(std::ostringstream& out, const DeviceFaultDiagnostic& fault) {
    out << "{\"api\":";
    json_string(out, to_string(fault.api));
    out << ",\"source\":";
    json_string(out, to_string(fault.source));
    out << ",\"timestampNanoseconds\":" << fault.timestampNanoseconds
        << ",\"faultAddress\":" << fault.faultAddress << ",\"deviceName\":";
    json_string(out, fault.deviceName);
    out << ",\"reason\":";
    json_string(out, fault.reason);
    out << ",\"description\":";
    json_string(out, fault.description);
    out << ",\"breadcrumbs\":";
    json_string_array(out, fault.breadcrumbs);
    out << ",\"pageFaultOperations\":";
    json_string_array(out, fault.pageFaultOperations);
    out << ",\"vendorInfo\":";
    json_string_array(out, fault.vendorInfo);
    out << ",\"deviceLost\":" << (fault.deviceLost ? "true" : "false")
        << ",\"recovered\":" << (fault.recovered ? "true" : "false") << '}';
}

void json_marker(std::ostringstream& out, const RenderCaptureMarker& marker) {
    out << "{\"id\":" << marker.id
        << ",\"timestampNanoseconds\":" << marker.timestampNanoseconds
        << ",\"frameIndex\":" << marker.frameIndex
        << ",\"depth\":" << marker.depth << ",\"name\":";
    json_string(out, marker.name);
    out << ",\"category\":";
    json_string(out, marker.category);
    out << ",\"queue\":";
    json_string(out, to_string(marker.queue));
    out << ",\"phase\":";
    json_string(out, to_string(marker.phase));
    out << '}';
}

void json_capture_metadata(std::ostringstream& out, const RenderCaptureMetadata& metadata) {
    out << "{\"tool\":";
    json_string(out, metadata.tool);
    out << ",\"name\":";
    json_string(out, metadata.name);
    out << ",\"filePath\":";
    json_string(out, metadata.filePath);
    out << ",\"beginTimestampNanoseconds\":" << metadata.beginTimestampNanoseconds
        << ",\"endTimestampNanoseconds\":" << metadata.endTimestampNanoseconds
        << ",\"markerCount\":" << metadata.markerCount
        << ",\"active\":" << (metadata.active ? "true" : "false")
        << ",\"allowAsync\":" << (metadata.allowAsync ? "true" : "false") << '}';
}

void json_budget_violation(std::ostringstream& out, const RenderBudgetViolation& violation) {
    out << "{\"frameIndex\":" << violation.frameIndex
        << ",\"timestampNanoseconds\":" << violation.timestampNanoseconds
        << ",\"heapIndex\":" << violation.heapIndex << ",\"heapName\":";
    json_string(out, violation.heapName);
    out << ",\"budgetBytes\":" << violation.budgetBytes
        << ",\"usageBytes\":" << violation.usageBytes
        << ",\"overBytes\":" << violation.overBytes << ",\"severity\":";
    json_string(out, to_string(violation.severity));
    out << '}';
}

void text_line(std::ostringstream& out, std::string_view key, std::string_view value) {
    out << key << '=';
    json_string(out, value);
    out << '\n';
}

} // namespace

const char* to_string(BackendApi api) noexcept {
    switch (api) {
    case BackendApi::Null: return "null";
    case BackendApi::DirectX11: return "d3d11";
    case BackendApi::DirectX12: return "d3d12";
    case BackendApi::Vulkan: return "vulkan";
    }
    return "unknown";
}

const char* to_string(RenderQueue queue) noexcept {
    switch (queue) {
    case RenderQueue::Graphics: return "graphics";
    case RenderQueue::Compute: return "compute";
    case RenderQueue::Copy: return "copy";
    }
    return "unknown";
}

const char* to_string(ResourceKind kind) noexcept {
    switch (kind) {
    case ResourceKind::Texture2D: return "texture2d";
    case ResourceKind::Buffer: return "buffer";
    case ResourceKind::DepthStencil: return "depth_stencil";
    case ResourceKind::Shader: return "shader";
    case ResourceKind::Pipeline: return "pipeline";
    case ResourceKind::Material: return "material";
    case ResourceKind::Sampler: return "sampler";
    }
    return "unknown";
}

const char* to_string(ResourceUsage usage) noexcept {
    switch (usage) {
    case ResourceUsage::Unknown: return "unknown";
    case ResourceUsage::ShaderRead: return "shader_read";
    case ResourceUsage::ShaderWrite: return "shader_write";
    case ResourceUsage::ColorAttachment: return "color_attachment";
    case ResourceUsage::ColorAttachment1: return "color_attachment1";
    case ResourceUsage::ColorAttachment2: return "color_attachment2";
    case ResourceUsage::ColorAttachment3: return "color_attachment3";
    case ResourceUsage::ColorAttachment4: return "color_attachment4";
    case ResourceUsage::ColorAttachment5: return "color_attachment5";
    case ResourceUsage::ColorAttachment6: return "color_attachment6";
    case ResourceUsage::ColorAttachment7: return "color_attachment7";
    case ResourceUsage::DepthStencil: return "depth_stencil";
    case ResourceUsage::VertexBuffer: return "vertex_buffer";
    case ResourceUsage::IndexBuffer: return "index_buffer";
    case ResourceUsage::CopySource: return "copy_source";
    case ResourceUsage::CopyDestination: return "copy_destination";
    case ResourceUsage::UniformBuffer: return "uniform_buffer";
    case ResourceUsage::StorageRead: return "storage_read";
    case ResourceUsage::StorageWrite: return "storage_write";
    case ResourceUsage::IndirectArguments: return "indirect_arguments";
    case ResourceUsage::Present: return "present";
    }
    return "unknown";
}

const char* to_string(CaptureMarkerPhase phase) noexcept {
    switch (phase) {
    case CaptureMarkerPhase::Begin: return "begin";
    case CaptureMarkerPhase::End: return "end";
    case CaptureMarkerPhase::Instant: return "instant";
    }
    return "unknown";
}

const char* to_string(RenderBudgetSeverity severity) noexcept {
    switch (severity) {
    case RenderBudgetSeverity::Warning: return "warning";
    case RenderBudgetSeverity::Critical: return "critical";
    }
    return "unknown";
}

const char* to_string(DeviceFaultSource source) noexcept {
    switch (source) {
    case DeviceFaultSource::None: return "none";
    case DeviceFaultSource::Backend: return "backend";
    case DeviceFaultSource::D3D12Dred: return "d3d12_dred";
    case DeviceFaultSource::VulkanDeviceFault: return "vulkan_device_fault";
    }
    return "unknown";
}

GpuMemoryBudgetSnapshot query_gpu_memory_budget(const RenderDiagnosticsHooks& hooks, BackendApi api) {
    GpuMemoryBudgetSnapshot result;
    result.api = api;
    result.timestampNanoseconds = now_nanoseconds();
    result.source = "unavailable";
    if (!hooks.queryMemoryBudget) {
        result.error = "memory budget hook is not installed";
        return result;
    }
    const bool success = hooks.queryMemoryBudget(result);
    if (result.timestampNanoseconds == 0) result.timestampNanoseconds = now_nanoseconds();
    if (result.api == BackendApi::Null) result.api = api;
    if (result.source.empty()) result.source = "backend-hook";
    if (!success && result.error.empty()) result.error = "memory budget hook failed";
    result.valid = success && result.error.empty();
    return result;
}

DeviceFaultDiagnostic query_device_fault(const RenderDiagnosticsHooks& hooks, BackendApi api) {
    DeviceFaultDiagnostic result;
    result.api = api;
    result.timestampNanoseconds = now_nanoseconds();
    if (!hooks.queryDeviceFault) return result;
    const bool success = hooks.queryDeviceFault(result);
    if (result.timestampNanoseconds == 0) result.timestampNanoseconds = now_nanoseconds();
    if (result.api == BackendApi::Null) result.api = api;
    if (!success && result.source == DeviceFaultSource::None) result.source = DeviceFaultSource::Backend;
    return result;
}

RenderGraphReport build_render_graph_report(const RenderGraph& graph) {
    RenderGraphReport report;
    report.compiled = !graph.execution_order().empty() || graph.passes().empty();
    report.diagnostics = graph.diagnostics();
    report.executionOrder = graph.execution_order();
    report.queueDependencies = graph.queue_dependencies();
    report.queueBatches = graph.queue_batches();
    report.passTimings = graph.pass_timings();
    report.timing.cpuNanoseconds = report.diagnostics.executeNanoseconds;
    report.timing.cpuTimingAvailable = !report.passTimings.empty() ||
        report.diagnostics.executeNanoseconds != 0;
    report.timing.gpuTimingDelayed = report.diagnostics.gpuTimingDelayed &&
        !report.passTimings.empty();
    for (const auto& timing : report.passTimings) {
        report.timing.gpuNanoseconds = saturating_add(report.timing.gpuNanoseconds,
                                                      timing.gpuNanoseconds);
        report.timing.gpuTimingAvailable = report.timing.gpuTimingAvailable ||
            timing.gpuNanoseconds != 0;
    }

    report.lifetimes.reserve(graph.resources().size());
    for (const auto& node : graph.resources()) {
        RenderGraphResourceLifetime lifetime;
        lifetime.logical = node.handle;
        lifetime.physical = node.physicalHandle;
        lifetime.external = node.external;
        lifetime.aliased = node.physicalHandle.id != 0 && node.physicalHandle.id != node.handle.id;
        lifetime.estimatedBytes = estimate_bytes(node.description);
        report.lifetimes.push_back(std::move(lifetime));
    }

    for (std::size_t passIndex = 0; passIndex < graph.passes().size(); ++passIndex) {
        const auto& pass = graph.passes()[passIndex];
        for (const auto& access : pass.accesses) {
            const auto found = std::find_if(report.lifetimes.begin(), report.lifetimes.end(),
                [&access](const RenderGraphResourceLifetime& lifetime) {
                    return lifetime.logical.id == access.resource.id &&
                        lifetime.logical.kind == access.resource.kind;
                });
            if (found == report.lifetimes.end()) continue;
            found->uses.push_back({passIndex, pass.queue, access.usage});
            found->firstPass = std::min(found->firstPass, passIndex);
            found->lastPass = std::max(found->lastPass, passIndex);
        }
    }

    report.aliases.reserve(report.lifetimes.size());
    for (const auto& lifetime : report.lifetimes) {
        if (!lifetime.aliased) continue;
        report.aliases.push_back({lifetime.logical, lifetime.physical, lifetime.firstPass,
                                  lifetime.lastPass, lifetime.estimatedBytes});
    }

    report.barriers.reserve(graph.transitions().size());
    for (const auto& transition : graph.transitions()) {
        RenderGraphBarrierReport barrier;
        barrier.resource = transition.resource;
        barrier.before = transition.before;
        barrier.after = transition.after;
        barrier.passIndex = transition.passIndex;
        if (transition.passIndex < graph.passes().size()) barrier.queue = graph.passes()[transition.passIndex].queue;
        report.barriers.push_back(barrier);
    }
    return report;
}

RenderCaptureRecorder::RenderCaptureRecorder(RenderDiagnosticsHooks hooks) : hooks_(std::move(hooks)) {}

void RenderCaptureRecorder::set_hooks(RenderDiagnosticsHooks hooks) { hooks_ = std::move(hooks); }

void RenderCaptureRecorder::record(CaptureMarkerPhase phase, std::string name, std::string category,
                                   std::uint64_t frameIndex, RenderQueue queue, std::uint32_t depth) {
    RenderCaptureMarker marker;
    marker.id = nextId_++;
    marker.timestampNanoseconds = now_nanoseconds();
    marker.frameIndex = frameIndex;
    marker.depth = depth;
    marker.name = std::move(name);
    marker.category = std::move(category);
    marker.queue = queue;
    marker.phase = phase;
    markers_.push_back(marker);
    if (hooks_.emitCaptureMarker) hooks_.emitCaptureMarker(markers_.back());
}

void RenderCaptureRecorder::begin(std::string_view name, std::string_view category,
                                  std::uint64_t frameIndex, RenderQueue queue) {
    const std::string scopeName(name);
    record(CaptureMarkerPhase::Begin, scopeName, std::string(category), frameIndex, queue,
           static_cast<std::uint32_t>(scopeStack_.size()));
    scopeStack_.push_back(scopeName);
}

void RenderCaptureRecorder::end(std::string_view name, std::string_view category,
                                std::uint64_t frameIndex, RenderQueue queue) {
    std::string markerName(name);
    if (markerName.empty() && !scopeStack_.empty()) markerName = scopeStack_.back();
    const auto depth = scopeStack_.empty() ? 0u : static_cast<std::uint32_t>(scopeStack_.size() - 1);
    if (!scopeStack_.empty()) scopeStack_.pop_back();
    record(CaptureMarkerPhase::End, std::move(markerName), std::string(category), frameIndex, queue, depth);
}

void RenderCaptureRecorder::instant(std::string_view name, std::string_view category,
                                    std::uint64_t frameIndex, RenderQueue queue) {
    record(CaptureMarkerPhase::Instant, std::string(name), std::string(category), frameIndex, queue,
           static_cast<std::uint32_t>(scopeStack_.size()));
}

void RenderCaptureRecorder::clear() {
    markers_.clear();
    scopeStack_.clear();
}

RenderDiagnosticsSnapshot collect_diagnostics(const RenderDiagnosticsHooks& hooks, const RenderGraph& graph,
                                               std::vector<RenderCaptureMarker> captureMarkers, BackendApi api) {
    RenderDiagnosticsSnapshot snapshot;
    snapshot.timestampNanoseconds = now_nanoseconds();
    snapshot.memory = query_gpu_memory_budget(hooks, api);
    snapshot.graph = build_render_graph_report(graph);
    snapshot.captureMarkers = std::move(captureMarkers);
    for (const auto& marker : snapshot.captureMarkers) {
        if (marker.frameIndex != 0) snapshot.frameIndex = marker.frameIndex;
    }
    snapshot.deviceFault = query_device_fault(hooks, api);
    return snapshot;
}

std::string serialize_json(const RenderDiagnosticsSnapshot& snapshot) {
    std::ostringstream out;
    out << "{\"frameIndex\":" << snapshot.frameIndex
        << ",\"timestampNanoseconds\":" << snapshot.timestampNanoseconds << ",\"memory\":";
    json_memory(out, snapshot.memory);
    out << ",\"graph\":";
    json_graph(out, snapshot.graph);
    out << ",\"captureMarkers\":[";
    for (std::size_t index = 0; index < snapshot.captureMarkers.size(); ++index) {
        if (index != 0) out << ',';
        json_marker(out, snapshot.captureMarkers[index]);
    }
    out << "],\"captureMetadata\":";
    json_capture_metadata(out, snapshot.captureMetadata);
    out << ",\"budgetViolations\":[";
    for (std::size_t index = 0; index < snapshot.budgetViolations.size(); ++index) {
        if (index != 0) out << ',';
        json_budget_violation(out, snapshot.budgetViolations[index]);
    }
    out << "],\"budgetFaultCorrelation\":{\"correlated\":"
        << (snapshot.budgetFaultCorrelation.correlated ? "true" : "false")
        << ",\"violationFrameIndex\":" << snapshot.budgetFaultCorrelation.violationFrameIndex
        << ",\"violationTimestampNanoseconds\":"
        << snapshot.budgetFaultCorrelation.violationTimestampNanoseconds
        << ",\"overBytes\":" << snapshot.budgetFaultCorrelation.overBytes << ",\"reason\":";
    json_string(out, snapshot.budgetFaultCorrelation.reason);
    out << "},\"deviceFault\":";
    json_fault(out, snapshot.deviceFault);
    out << '}';
    return out.str();
}

std::string serialize_text(const RenderDiagnosticsSnapshot& snapshot) {
    std::ostringstream out;
    out << "render_diagnostics.version=1\n";
    out << "frameIndex=" << snapshot.frameIndex << '\n';
    out << "timestampNanoseconds=" << snapshot.timestampNanoseconds << '\n';
    out << "memory.api=" << to_string(snapshot.memory.api) << '\n';
    out << "memory.valid=" << (snapshot.memory.valid ? "true" : "false") << '\n';
    out << "memory.estimated=" << (snapshot.memory.estimated ? "true" : "false") << '\n';
    text_line(out, "memory.source", snapshot.memory.source);
    text_line(out, "memory.error", snapshot.memory.error);
    out << "memory.heapCount=" << snapshot.memory.heaps.size() << '\n';
    for (std::size_t index = 0; index < snapshot.memory.heaps.size(); ++index) {
        const auto& heap = snapshot.memory.heaps[index];
        out << "memory.heap[" << index << "].index=" << heap.index << '\n';
        text_line(out, "memory.heap[" + std::to_string(index) + "].name", heap.name);
        out << "memory.heap[" << index << "].budgetBytes=" << heap.budgetBytes << '\n';
        out << "memory.heap[" << index << "].usageBytes=" << heap.usageBytes << '\n';
    }
    out << "graph.compiled=" << (snapshot.graph.compiled ? "true" : "false") << '\n';
    out << "graph.passCount=" << snapshot.graph.diagnostics.passCount << '\n';
    out << "graph.resourceCount=" << snapshot.graph.diagnostics.resourceCount << '\n';
    out << "graph.lifetimeCount=" << snapshot.graph.lifetimes.size() << '\n';
    out << "graph.barrierCount=" << snapshot.graph.barriers.size() << '\n';
    out << "graph.aliasCount=" << snapshot.graph.aliases.size() << '\n';
    out << "graph.queueDependencyCount=" << snapshot.graph.queueDependencies.size() << '\n';
    out << "graph.timing.cpuNanoseconds=" << snapshot.graph.timing.cpuNanoseconds << '\n';
    out << "graph.timing.gpuNanoseconds=" << snapshot.graph.timing.gpuNanoseconds << '\n';
    out << "graph.timing.cpuTimingAvailable="
        << (snapshot.graph.timing.cpuTimingAvailable ? "true" : "false") << '\n';
    out << "graph.timing.gpuTimingAvailable="
        << (snapshot.graph.timing.gpuTimingAvailable ? "true" : "false") << '\n';
    out << "graph.timing.cpuTimingDelayed="
        << (snapshot.graph.timing.cpuTimingDelayed ? "true" : "false") << '\n';
    out << "graph.timing.gpuTimingDelayed="
        << (snapshot.graph.timing.gpuTimingDelayed ? "true" : "false") << '\n';
    out << "graph.passTimingCount=" << snapshot.graph.passTimings.size() << '\n';
    for (std::size_t index = 0; index < snapshot.graph.passTimings.size(); ++index) {
        const auto& timing = snapshot.graph.passTimings[index];
        text_line(out, "graph.passTiming[" + std::to_string(index) + "].name", timing.name);
        out << "graph.passTiming[" << index << "].cpuNanoseconds=" << timing.cpuNanoseconds << '\n';
        out << "graph.passTiming[" << index << "].gpuNanoseconds=" << timing.gpuNanoseconds << '\n';
    }
    out << "captureMarkerCount=" << snapshot.captureMarkers.size() << '\n';
    for (std::size_t index = 0; index < snapshot.captureMarkers.size(); ++index) {
        const auto& marker = snapshot.captureMarkers[index];
        out << "captureMarker[" << index << "].id=" << marker.id << '\n';
        text_line(out, "captureMarker[" + std::to_string(index) + "].name", marker.name);
        out << "captureMarker[" << index << "].phase=" << to_string(marker.phase) << '\n';
        out << "captureMarker[" << index << "].depth=" << marker.depth << '\n';
    }
    text_line(out, "captureMetadata.tool", snapshot.captureMetadata.tool);
    text_line(out, "captureMetadata.name", snapshot.captureMetadata.name);
    text_line(out, "captureMetadata.filePath", snapshot.captureMetadata.filePath);
    out << "captureMetadata.active=" << (snapshot.captureMetadata.active ? "true" : "false") << '\n';
    out << "captureMetadata.markerCount=" << snapshot.captureMetadata.markerCount << '\n';
    out << "budgetViolationCount=" << snapshot.budgetViolations.size() << '\n';
    for (std::size_t index = 0; index < snapshot.budgetViolations.size(); ++index) {
        const auto& violation = snapshot.budgetViolations[index];
        out << "budgetViolation[" << index << "].frameIndex=" << violation.frameIndex << '\n';
        text_line(out, "budgetViolation[" + std::to_string(index) + "].heapName", violation.heapName);
        out << "budgetViolation[" << index << "].budgetBytes=" << violation.budgetBytes << '\n';
        out << "budgetViolation[" << index << "].usageBytes=" << violation.usageBytes << '\n';
        out << "budgetViolation[" << index << "].overBytes=" << violation.overBytes << '\n';
        out << "budgetViolation[" << index << "].severity=" << to_string(violation.severity) << '\n';
    }
    out << "budgetFaultCorrelation.correlated="
        << (snapshot.budgetFaultCorrelation.correlated ? "true" : "false") << '\n';
    text_line(out, "budgetFaultCorrelation.reason", snapshot.budgetFaultCorrelation.reason);
    out << "deviceFault.api=" << to_string(snapshot.deviceFault.api) << '\n';
    out << "deviceFault.source=" << to_string(snapshot.deviceFault.source) << '\n';
    out << "deviceFault.deviceLost=" << (snapshot.deviceFault.deviceLost ? "true" : "false") << '\n';
    out << "deviceFault.recovered=" << (snapshot.deviceFault.recovered ? "true" : "false") << '\n';
    text_line(out, "deviceFault.reason", snapshot.deviceFault.reason);
    text_line(out, "deviceFault.description", snapshot.deviceFault.description);
    return out.str();
}

} // namespace shinkou::render

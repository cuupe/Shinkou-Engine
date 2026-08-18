#include "shinkou/render/RenderBudgetService.h"
#include "shinkou/render/RenderCapture.h"

#include <cassert>
#include <string>

using namespace shinkou::render;

namespace {
GpuMemoryBudgetSnapshot memory_sample(std::uint64_t timestamp, std::uint64_t usage) {
    GpuMemoryBudgetSnapshot snapshot;
    snapshot.api = BackendApi::Vulkan;
    snapshot.timestampNanoseconds = timestamp;
    snapshot.source = "test";
    snapshot.valid = true;
    snapshot.heaps.push_back({0, "local", 1000, usage, 1000 - usage, 0, true});
    return snapshot;
}
}

int main() {
    std::size_t externalBegins = 0;
    std::size_t externalEnds = 0;
    std::size_t externalMarkers = 0;
    RenderExternalCaptureHooks external;
    external.beginCapture = [&externalBegins](const RenderExternalCaptureRequest& request) {
        ++externalBegins;
        return request.tool == RenderCaptureTool::RenderDoc;
    };
    external.endCapture = [&externalEnds]() { ++externalEnds; };
    external.setMarker = [&externalMarkers](std::string_view) { ++externalMarkers; };

    RenderCaptureService capture({}, external);
    assert(capture.begin_external_capture({RenderCaptureTool::RenderDoc, "frame", {}, true}));
    {
        auto frame = capture.scope("frame", "test", 4);
        capture.marker("draw", "test", 4);
        auto nested = capture.scope("nested", "test", 4, RenderQueue::Compute);
        assert(nested.active());
    }
    capture.end_external_capture();
    assert(externalBegins == 1);
    assert(externalEnds == 1);
    assert(externalMarkers == 5);
    assert(capture.markers().size() == 5);
    const auto captureMetadata = capture.metadata();
    assert(captureMetadata.tool == "renderdoc");
    assert(captureMetadata.name == "frame");
    assert(captureMetadata.markerCount == 5);
    assert(!captureMetadata.active);
    assert(capture.markers()[0].phase == CaptureMarkerPhase::Begin);
    assert(capture.markers()[1].name == "draw");
    assert(capture.markers().back().phase == CaptureMarkerPhase::End);

    RenderGraph graph;
    const auto json = capture.graph_json(graph, BackendApi::Vulkan);
    const auto text = capture.graph_text(graph, BackendApi::Vulkan);
    assert(json.find("\"graph\"") != std::string::npos);
    assert(json.find("captureMarkers") != std::string::npos);
    assert(text.find("render_diagnostics.version=1") != std::string::npos);

    RenderBudgetService budget(2);
    budget.record(memory_sample(10, 400), 1);
    budget.record(memory_sample(20, 1200), 2);
    budget.record(memory_sample(30, 600), 3);
    const auto aggregate = budget.aggregate();
    assert(aggregate.sampleCount == 2);
    assert(aggregate.validSampleCount == 2);
    assert(aggregate.overBudgetSampleCount == 1);
    assert(aggregate.latestFrameIndex == 3);
    assert(aggregate.heaps.size() == 1);
    assert(aggregate.heaps[0].latestUsageBytes == 600);
    assert(aggregate.heaps[0].peakUsageBytes == 1200);
    assert(aggregate.heaps[0].averageUsageBytes == 900);

    std::size_t transitions = 0;
    DeviceRecoveryStateMachine recovery([&transitions](const DeviceRecoveryStatus&) { ++transitions; });
    DeviceFaultDiagnostic fault;
    fault.api = BackendApi::Vulkan;
    fault.source = DeviceFaultSource::VulkanDeviceFault;
    fault.deviceLost = true;
    fault.reason = "device reset";
    const auto correlation = correlate_budget_fault(aggregate, fault);
    assert(correlation.correlated);
    assert(correlation.violationFrameIndex == 2);
    assert(correlation.overBytes == 200);
    assert(recovery.notify_device_lost(fault));
    assert(recovery.state() == DeviceRecoveryState::Lost);
    assert(recovery.begin_recovery());
    assert(recovery.mark_recovered());
    assert(recovery.state() == DeviceRecoveryState::Recovered);
    assert(recovery.reset());
    assert(recovery.state() == DeviceRecoveryState::Ready);
    assert(transitions == 4);

    const auto converted = to_diagnostics_snapshot(graph, aggregate, recovery.status(), capture.markers(),
                                                   BackendApi::Vulkan);
    assert(converted.memory.valid);
    assert(converted.memory.heaps[0].usageBytes == 600);
    assert(converted.captureMarkers.size() == capture.markers().size());
    assert(!converted.deviceFault.deviceLost);
    return 0;
}

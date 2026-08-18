#include "shinkou/render/RendererDiagnosticsService.h"

#include <cassert>
#include <string>
#include <thread>

using namespace shinkou::render;

int main() {
    const auto mainThread = std::this_thread::get_id();
    std::thread::id callbackThread;

    RendererDiagnosticsServiceConfig config;
    config.pendingFrameCapacity = 2;
    config.budgetWindowCapacity = 4;
    config.completedFrameCapacity = 2;
    config.api = BackendApi::DirectX12;
    RendererDiagnosticsService service(config);

    assert(service.begin_frame(42));
    {
        auto frameScope = service.scope("frame", "test", 42);
        assert(frameScope.active());
        assert(service.marker("opaque", "test", 42));
    }

    RendererDiagnosticsFrameCallbacks callbacks;
    callbacks.graphReport = [&callbackThread]() {
        callbackThread = std::this_thread::get_id();
        RenderGraphReport report;
        report.compiled = true;
        report.diagnostics.passCount = 2;
        return report;
    };
    callbacks.memoryBudget = [] {
        GpuMemoryBudgetSnapshot snapshot;
        snapshot.api = BackendApi::DirectX12;
        snapshot.valid = true;
        snapshot.source = "test-provider";
        snapshot.heaps.push_back({0, "local", 4096, 5000, 0, 0, true});
        return snapshot;
    };
    callbacks.deviceFault = [] {
        DeviceFaultDiagnostic fault;
        fault.api = BackendApi::DirectX12;
        fault.source = DeviceFaultSource::D3D12Dred;
        fault.deviceLost = true;
        fault.reason = "test device lost";
        return fault;
    };
    assert(service.end_frame(std::move(callbacks)));
    service.wait_until_idle();

    const auto latest = service.latest();
    assert(latest);
    assert(latest->processed);
    assert(latest->frameIndex == 42);
    assert(latest->diagnostics.graph.diagnostics.passCount == 2);
    assert(latest->diagnostics.memory.heaps[0].usageBytes == 5000);
    assert(latest->diagnostics.captureMarkers.size() == 3);
    assert(latest->diagnostics.deviceFault.source == DeviceFaultSource::D3D12Dred);
    assert(latest->diagnostics.budgetFaultCorrelation.correlated);
    assert(latest->diagnostics.budgetFaultCorrelation.overBytes == 904);
    assert(latest->recovery.state == DeviceRecoveryState::Lost);
    assert(callbackThread != mainThread);
    assert(latest->json.find("captureMarkers") != std::string::npos);
    assert(latest->json.find("d3d12_dred") != std::string::npos);
    assert(latest->text.find("graph.passCount=2") != std::string::npos);

    assert(service.begin_recovery());
    assert(service.mark_recovered());
    const auto recovery = service.recovery_status();
    assert(recovery);
    assert(recovery->state == DeviceRecoveryState::Recovered);
    assert(!service.frame_open());
    return 0;
}

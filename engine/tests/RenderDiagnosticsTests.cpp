#include "shinkou/render/RenderDiagnostics.h"

#include <cassert>
#include <string>

using namespace shinkou::render;

int main() {
    RenderGraph graph;
    const auto first = graph.create_texture(TextureDesc{64, 64, 1, 1, "rgba8", true});
    const auto second = graph.create_texture(TextureDesc{64, 64, 1, 1, "rgba8", true});
    graph.add_pass("write_first", {{first, ResourceUsage::ColorAttachment}},
                   [](IRenderBackend&, const RenderPassContext&) {});
    graph.add_pass("write_second", {{second, ResourceUsage::ColorAttachment}},
                   [](IRenderBackend&, const RenderPassContext&) {});
    assert(graph.compile());

    const auto report = build_render_graph_report(graph);
    assert(report.compiled);
    assert(report.lifetimes.size() == 2);
    assert(report.lifetimes[0].firstPass == 0);
    assert(report.lifetimes[0].lastPass == 0);
    assert(report.lifetimes[0].estimatedBytes == 64u * 64u * 4u);
    assert(report.barriers.size() == 2);
    assert(report.aliases.size() == 1);

    std::size_t emitted = 0;
    RenderDiagnosticsHooks hooks;
    hooks.queryMemoryBudget = [](GpuMemoryBudgetSnapshot& snapshot) {
        snapshot.source = "test-backend";
        snapshot.heaps.push_back({0, "local", 1024, 512, 512, 0, true});
        return true;
    };
    hooks.queryDeviceFault = [](DeviceFaultDiagnostic& fault) {
        fault.source = DeviceFaultSource::D3D12Dred;
        fault.deviceLost = true;
        fault.reason = "test fault";
        fault.breadcrumbs.push_back("write_first");
        return true;
    };
    hooks.emitCaptureMarker = [&emitted](const RenderCaptureMarker&) { ++emitted; };

    RenderCaptureRecorder recorder(hooks);
    recorder.begin("frame", "test", 7);
    recorder.instant("draw", "test", 7);
    recorder.end({}, "test", 7);
    assert(recorder.markers().size() == 3);
    assert(emitted == 3);
    assert(recorder.markers()[0].phase == CaptureMarkerPhase::Begin);
    assert(recorder.markers()[2].name == "frame");

    const auto snapshot = collect_diagnostics(hooks, graph, recorder.markers(), BackendApi::DirectX12);
    assert(snapshot.memory.valid);
    assert(snapshot.memory.api == BackendApi::DirectX12);
    assert(snapshot.deviceFault.source == DeviceFaultSource::D3D12Dred);
    const std::string json = serialize_json(snapshot);
    assert(json.find("\"memory\"") != std::string::npos);
    assert(json.find("\"lifetimes\"") != std::string::npos);
    assert(json.find("d3d12_dred") != std::string::npos);
    const std::string text = serialize_text(snapshot);
    assert(text.find("render_diagnostics.version=1") != std::string::npos);
    assert(text.find("graph.aliasCount=1") != std::string::npos);
    return 0;
}

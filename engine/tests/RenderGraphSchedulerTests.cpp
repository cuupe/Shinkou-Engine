#include "shinkou/render/RenderGraphScheduler.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

using namespace shinkou::render;

namespace {
void update_peak(std::atomic<int>& peak, int value) {
    int observed = peak.load(std::memory_order_relaxed);
    while (observed < value && !peak.compare_exchange_weak(
        observed, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

RenderGraph independentGraph() {
    RenderGraph graph;
    graph.add_pass("graphics", {}, {}, [](auto&, const auto&) {}, RenderQueue::Graphics);
    graph.add_pass("compute", {}, {}, [](auto&, const auto&) {}, RenderQueue::Compute);
    graph.add_pass("copy", {}, {}, [](auto&, const auto&) {}, RenderQueue::Copy);
    return graph;
}
}

int main() {
    RenderGraph graph = independentGraph();
    std::string error;
    if (!graph.compile(&error)) {
        std::cerr << error << '\n';
        return 1;
    }
    RenderGraphScheduler scheduler;
    const auto logical = scheduler.build(graph, &error);
    if (!logical.valid || logical.levels.size() != 1 || logical.levels.front().passIndices.size() != 3 ||
        logical.batches.size() != 3 || logical.submissionOrder.size() != 3 ||
        logical.submissionOrder[0] != 0 || logical.submissionOrder[1] != 1 || logical.submissionOrder[2] != 2) {
        std::cerr << "independent dependency level or queue batch plan failed\n";
        return 2;
    }

    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    FunctionCommandRecordingExecutor parallelExecutor([&](const CommandRecordingTask& task) {
        if (task.pass_context().name.empty()) return CommandRecordingResult{CommandRecordingStatus::Failed, "empty name", {}};
        const auto current = active.fetch_add(1) + 1;
        update_peak(peak, current);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        active.fetch_sub(1);
        return CommandRecordingResult{CommandRecordingStatus::Recorded, {}, {}};
    });
    RenderGraphScheduler::Options parallelOptions;
    parallelOptions.maxConcurrency = 3;
    const auto prepared = scheduler.prepare(graph, parallelExecutor, parallelOptions);
    if (!prepared.valid || prepared.usedFallback || peak.load() < 2 ||
        prepared.fallbackPassIndices.size() != 0 ||
        prepared.result_for_pass(0)->status != CommandRecordingStatus::Recorded) {
        std::cerr << "independent tasks were not prepared concurrently\n";
        return 3;
    }

    RenderGraph resourceGraph;
    const auto buffer = resourceGraph.create_buffer({256, 16, false, false, {}, false, false, true});
    resourceGraph.add_pass("read_a", {{buffer, ResourceUsage::StorageRead}}, [](auto&, const auto&) {});
    resourceGraph.add_pass("read_b", {{buffer, ResourceUsage::StorageRead}}, [](auto&, const auto&) {});
    if (!resourceGraph.compile(&error)) return 4;
    std::atomic<int> exclusiveActive{0};
    std::atomic<int> exclusivePeak{0};
    FunctionCommandRecordingExecutor exclusiveExecutor([&](const CommandRecordingTask&) {
        const auto current = exclusiveActive.fetch_add(1) + 1;
        update_peak(exclusivePeak, current);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        exclusiveActive.fetch_sub(1);
        return CommandRecordingResult{CommandRecordingStatus::Recorded, {}, {}};
    });
    RenderGraphScheduler::Options exclusiveOptions;
    exclusiveOptions.maxConcurrency = 2;
    exclusiveOptions.resourcePolicy = [](ResourceHandle, ResourceUsage) {
        return CommandResourceAccessMode::Exclusive;
    };
    scheduler.prepare(resourceGraph, exclusiveExecutor, exclusiveOptions);
    if (exclusivePeak.load() != 1) {
        std::cerr << "exclusive resource recording was concurrent\n";
        return 5;
    }

    RenderGraph chain;
    TextureDesc chainTexture;
    chainTexture.width = 32;
    chainTexture.height = 32;
    chainTexture.renderTarget = true;
    const auto texture = chain.create_texture(chainTexture);
    chain.add_pass("producer", {}, {texture}, [](auto&, const auto&) {});
    chain.add_pass("consumer", {texture}, {}, [](auto&, const auto&) {});
    chain.add_pass("tail", {texture}, {}, [](auto&, const auto&) {});
    if (!chain.compile(&error)) return 6;
    FunctionCommandRecordingExecutor failingExecutor([](const CommandRecordingTask& task) {
        if (task.passIndex == 0) return CommandRecordingResult{CommandRecordingStatus::Failed, "worker unavailable", {}};
        return CommandRecordingResult{CommandRecordingStatus::Recorded, {}, {}};
    });
    const auto failed = scheduler.prepare(chain, failingExecutor, {});
    if (!failed.valid || !failed.usedFallback || failed.fallbackPassIndices.size() != 3 ||
        failed.fallbackPassIndices[0] != 0 || failed.fallbackPassIndices[1] != 1 || failed.fallbackPassIndices[2] != 2 ||
        failed.result_for_pass(0)->status != CommandRecordingStatus::Failed ||
        failed.result_for_pass(1)->status != CommandRecordingStatus::Fallback) {
        std::cerr << "recording failure did not preserve fallback order\n";
        return 7;
    }

    RenderGraph mainOnly;
    const auto mainOnlyBuffer = mainOnly.create_buffer({64, 16, false, false, {}, false, false, true});
    mainOnly.add_pass("main_only", {{mainOnlyBuffer, ResourceUsage::StorageRead}}, [](auto&, const auto&) {});
    if (!mainOnly.compile(&error)) return 8;
    std::atomic<int> dispatched{0};
    FunctionCommandRecordingExecutor shouldNotRun([&](const CommandRecordingTask&) {
        ++dispatched;
        return CommandRecordingResult{CommandRecordingStatus::Recorded, {}, {}};
    });
    RenderGraphScheduler::Options mainOnlyOptions;
    mainOnlyOptions.resourcePolicy = [](ResourceHandle, ResourceUsage) {
        return CommandResourceAccessMode::MainThreadOnly;
    };
    const auto mainOnlyPlan = scheduler.prepare(mainOnly, shouldNotRun, mainOnlyOptions);
    if (!mainOnlyPlan.usedFallback || dispatched.load() != 0 || mainOnlyPlan.fallbackPassIndices.size() != 1) {
        std::cerr << "main-thread resource constraint was ignored\n";
        return 9;
    }
    return 0;
}

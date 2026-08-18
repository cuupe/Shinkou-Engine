#include "shinkou/render/RenderBudgetService.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

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

std::uint64_t average(std::uint64_t sum, std::uint64_t count) noexcept {
    return count == 0 ? 0 : sum / count;
}

struct HeapAccumulator {
    GpuMemoryHeapAggregate value;
    std::uint64_t budgetSum{0};
    std::uint64_t usageSum{0};
    std::uint64_t availableSum{0};
};

} // namespace

RenderBudgetService::RenderBudgetService(std::size_t capacity) : capacity_(capacity) {}

void RenderBudgetService::set_capacity(std::size_t capacity) {
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_.store(capacity, std::memory_order_relaxed);
    while (samples_.size() > capacity) samples_.pop_front();
}

void RenderBudgetService::clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
}

void RenderBudgetService::record(GpuMemoryBudgetSnapshot snapshot, std::uint64_t frameIndex) {
    if (snapshot.timestampNanoseconds == 0) snapshot.timestampNanoseconds = now_nanoseconds();
    std::lock_guard<std::mutex> lock(mutex_);
    if (capacity_.load(std::memory_order_relaxed) == 0) return;
    samples_.push_back({frameIndex, std::move(snapshot)});
    while (samples_.size() > capacity_.load(std::memory_order_relaxed)) samples_.pop_front();
}

bool RenderBudgetService::sample(const RenderDiagnosticsHooks& hooks, BackendApi api,
                                 std::uint64_t frameIndex) {
    auto snapshot = query_gpu_memory_budget(hooks, api);
    const bool valid = snapshot.valid;
    record(std::move(snapshot), frameIndex);
    return valid;
}

void RenderBudgetService::ingest(const RenderDiagnosticsSnapshot& snapshot, std::uint64_t frameIndex) {
    record(snapshot.memory, frameIndex);
}

GpuMemoryBudgetAggregate RenderBudgetService::aggregate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    GpuMemoryBudgetAggregate result;
    result.sampleCount = samples_.size();
    std::vector<HeapAccumulator> heaps;
    heaps.reserve(samples_.empty() ? 0 : samples_.front().snapshot.heaps.size());

    for (const auto& sample : samples_) {
        const auto& memory = sample.snapshot;
        if (memory.api != BackendApi::Null) result.api = memory.api;
        result.estimated = result.estimated || memory.estimated;
        if (memory.valid) ++result.validSampleCount;
        if (memory.timestampNanoseconds != 0) {
            if (result.firstTimestampNanoseconds == 0) result.firstTimestampNanoseconds = memory.timestampNanoseconds;
            result.latestTimestampNanoseconds = memory.timestampNanoseconds;
        }
        result.latestFrameIndex = sample.frameIndex;

        bool sampleOverBudget = false;
        for (const auto& heap : memory.heaps) {
            const auto found = std::find_if(heaps.begin(), heaps.end(), [&heap](const HeapAccumulator& item) {
                return item.value.index == heap.index && item.value.name == heap.name;
            });
            auto* item = found == heaps.end() ? nullptr : &*found;
            if (item == nullptr) {
                heaps.push_back({});
                item = &heaps.back();
                item->value.index = heap.index;
                item->value.name = heap.name;
            }
            ++item->value.sampleCount;
            item->value.latestBudgetBytes = heap.budgetBytes;
            item->value.latestUsageBytes = heap.usageBytes;
            item->value.latestAvailableBytes = heap.availableBytes;
            item->value.peakUsageBytes = std::max(item->value.peakUsageBytes, heap.usageBytes);
            item->usageSum = saturating_add(item->usageSum, heap.usageBytes);
            item->availableSum = saturating_add(item->availableSum, heap.availableBytes);
            if (heap.budgetKnown) {
                ++item->value.budgetKnownSamples;
                item->value.budgetKnown = true;
                item->budgetSum = saturating_add(item->budgetSum, heap.budgetBytes);
                if (heap.usageBytes > heap.budgetBytes) {
                    ++item->value.overBudgetSamples;
                    sampleOverBudget = true;
                    result.violations.push_back({sample.frameIndex, memory.timestampNanoseconds,
                                                 heap.index, heap.name, heap.budgetBytes,
                                                 heap.usageBytes, heap.usageBytes - heap.budgetBytes});
                }
            }
        }
        if (sampleOverBudget) ++result.overBudgetSampleCount;
    }

    result.heaps.reserve(heaps.size());
    for (auto& item : heaps) {
        item.value.averageBudgetBytes = average(item.budgetSum, item.value.budgetKnownSamples);
        item.value.averageUsageBytes = average(item.usageSum, item.value.sampleCount);
        item.value.averageAvailableBytes = average(item.availableSum, item.value.sampleCount);
        result.heaps.push_back(std::move(item.value));
    }
    return result;
}

GpuMemoryBudgetSnapshot RenderBudgetService::latest_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.empty() ? GpuMemoryBudgetSnapshot{} : samples_.back().snapshot;
}

std::deque<GpuMemoryBudgetSample> RenderBudgetService::samples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_;
}

DeviceRecoveryStateMachine::DeviceRecoveryStateMachine(StateChangedCallback callback)
    : callback_(std::move(callback)) {
    status_.stateTimestampNanoseconds = now_nanoseconds();
}

bool DeviceRecoveryStateMachine::transition(DeviceRecoveryState next, std::string error) {
    if (status_.state == next && error == status_.lastError) return false;
    status_.state = next;
    status_.stateTimestampNanoseconds = now_nanoseconds();
    status_.lastError = std::move(error);
    if (callback_) {
        try {
            callback_(status_);
        } catch (...) {
            // State changes remain authoritative even if an observer fails.
        }
    }
    return true;
}

bool DeviceRecoveryStateMachine::notify_device_lost(DeviceFaultDiagnostic fault) {
    if (status_.state == DeviceRecoveryState::Lost) return false;
    status_.lastFault = std::move(fault);
    status_.lastError = status_.lastFault.reason;
    ++status_.generation;
    return transition(DeviceRecoveryState::Lost, status_.lastError);
}

bool DeviceRecoveryStateMachine::begin_recovery() {
    if (status_.state != DeviceRecoveryState::Lost) return false;
    return transition(DeviceRecoveryState::Recovering);
}

bool DeviceRecoveryStateMachine::mark_recovered() {
    if (status_.state != DeviceRecoveryState::Recovering) return false;
    return transition(DeviceRecoveryState::Recovered);
}

bool DeviceRecoveryStateMachine::mark_failed(std::string error) {
    if (status_.state != DeviceRecoveryState::Lost && status_.state != DeviceRecoveryState::Recovering) return false;
    return transition(DeviceRecoveryState::Failed, std::move(error));
}

bool DeviceRecoveryStateMachine::reset() {
    if (status_.state != DeviceRecoveryState::Recovered && status_.state != DeviceRecoveryState::Failed) return false;
    return transition(DeviceRecoveryState::Ready);
}

const char* to_string(DeviceRecoveryState state) noexcept {
    switch (state) {
    case DeviceRecoveryState::Ready: return "ready";
    case DeviceRecoveryState::Lost: return "lost";
    case DeviceRecoveryState::Recovering: return "recovering";
    case DeviceRecoveryState::Recovered: return "recovered";
    case DeviceRecoveryState::Failed: return "failed";
    }
    return "unknown";
}

GpuMemoryBudgetSnapshot to_memory_snapshot(const GpuMemoryBudgetAggregate& aggregate) {
    GpuMemoryBudgetSnapshot snapshot;
    snapshot.api = aggregate.api;
    snapshot.timestampNanoseconds = aggregate.latestTimestampNanoseconds;
    snapshot.source = "budget-service.aggregate";
    snapshot.valid = aggregate.validSampleCount != 0;
    snapshot.estimated = aggregate.estimated;
    snapshot.heaps.reserve(aggregate.heaps.size());
    for (const auto& heap : aggregate.heaps) {
        snapshot.heaps.push_back({heap.index, heap.name, heap.latestBudgetBytes, heap.latestUsageBytes,
                                  heap.latestAvailableBytes, 0, heap.budgetKnown});
    }
    return snapshot;
}

RenderBudgetFaultCorrelation correlate_budget_fault(const GpuMemoryBudgetAggregate& aggregate,
                                                    const DeviceFaultDiagnostic& fault) {
    RenderBudgetFaultCorrelation result;
    if (!fault.deviceLost || aggregate.violations.empty()) return result;

    const auto& violation = aggregate.violations.back();
    constexpr std::uint64_t kCorrelationWindowNanoseconds = 5'000'000'000ull;
    if (fault.timestampNanoseconds != 0 && violation.timestampNanoseconds != 0) {
        const auto delta = fault.timestampNanoseconds > violation.timestampNanoseconds
            ? fault.timestampNanoseconds - violation.timestampNanoseconds
            : violation.timestampNanoseconds - fault.timestampNanoseconds;
        if (delta > kCorrelationWindowNanoseconds) return result;
    }
    result.correlated = true;
    result.violationFrameIndex = violation.frameIndex;
    result.violationTimestampNanoseconds = violation.timestampNanoseconds;
    result.overBytes = violation.overBytes;
    result.reason = fault.reason.empty() ? "device fault near GPU memory budget violation" :
        "device fault near GPU memory budget violation: " + fault.reason;
    return result;
}

DeviceFaultDiagnostic to_device_fault(const DeviceRecoveryStatus& status, BackendApi api) {
    auto fault = status.lastFault;
    if (fault.api == BackendApi::Null) fault.api = api;
    if (fault.timestampNanoseconds == 0) fault.timestampNanoseconds = status.stateTimestampNanoseconds;
    fault.deviceLost = status.state == DeviceRecoveryState::Lost ||
        status.state == DeviceRecoveryState::Recovering || status.state == DeviceRecoveryState::Failed;
    fault.recovered = status.state == DeviceRecoveryState::Recovered;
    if (fault.source == DeviceFaultSource::None && fault.deviceLost) fault.source = DeviceFaultSource::Backend;
    if (fault.reason.empty()) fault.reason = status.lastError;
    return fault;
}

RenderDiagnosticsSnapshot to_diagnostics_snapshot(const RenderGraph& graph,
                                                  const GpuMemoryBudgetAggregate& aggregate,
                                                  const DeviceRecoveryStatus& recovery,
                                                  std::vector<RenderCaptureMarker> markers,
                                                  BackendApi api) {
    RenderDiagnosticsSnapshot snapshot;
    snapshot.timestampNanoseconds = now_nanoseconds();
    snapshot.memory = to_memory_snapshot(aggregate);
    snapshot.graph = build_render_graph_report(graph);
    snapshot.captureMarkers = std::move(markers);
    snapshot.budgetViolations = aggregate.violations;
    snapshot.deviceFault = to_device_fault(recovery, api);
    snapshot.budgetFaultCorrelation = correlate_budget_fault(aggregate, snapshot.deviceFault);
    return snapshot;
}

} // namespace shinkou::render

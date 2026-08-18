#pragma once

#include "shinkou/render/RenderDiagnostics.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace shinkou::render {

struct GpuMemoryBudgetSample {
    std::uint64_t frameIndex{0};
    GpuMemoryBudgetSnapshot snapshot;
};

struct GpuMemoryHeapAggregate {
    std::uint32_t index{0};
    std::string name;
    std::uint64_t sampleCount{0};
    std::uint64_t budgetKnownSamples{0};
    std::uint64_t overBudgetSamples{0};
    std::uint64_t latestBudgetBytes{0};
    std::uint64_t latestUsageBytes{0};
    std::uint64_t latestAvailableBytes{0};
    std::uint64_t averageBudgetBytes{0};
    std::uint64_t averageUsageBytes{0};
    std::uint64_t averageAvailableBytes{0};
    std::uint64_t peakUsageBytes{0};
    bool budgetKnown{false};
};

struct GpuMemoryBudgetAggregate {
    BackendApi api{BackendApi::Null};
    std::uint64_t sampleCount{0};
    std::uint64_t validSampleCount{0};
    std::uint64_t overBudgetSampleCount{0};
    std::uint64_t firstTimestampNanoseconds{0};
    std::uint64_t latestTimestampNanoseconds{0};
    std::uint64_t latestFrameIndex{0};
    std::uint64_t warningViolationCount{0};
    std::uint64_t criticalViolationCount{0};
    std::uint64_t peakOverBytes{0};
    bool estimated{false};
    std::vector<GpuMemoryHeapAggregate> heaps;
    std::vector<RenderBudgetViolation> violations;
};

class RenderBudgetService {
public:
    explicit RenderBudgetService(std::size_t capacity = 120);

    void set_capacity(std::size_t capacity);
    std::size_t capacity() const noexcept { return capacity_.load(std::memory_order_relaxed); }
    void clear() noexcept;

    // Records a copy, so platform-owned query buffers can be reused after the
    // call. Invalid samples remain in the window for observability.
    void record(GpuMemoryBudgetSnapshot snapshot, std::uint64_t frameIndex = 0);
    bool sample(const RenderDiagnosticsHooks& hooks, BackendApi api = BackendApi::Null,
                std::uint64_t frameIndex = 0);
    void ingest(const RenderDiagnosticsSnapshot& snapshot, std::uint64_t frameIndex = 0);

    GpuMemoryBudgetAggregate aggregate() const;
    GpuMemoryBudgetSnapshot latest_snapshot() const;
    std::deque<GpuMemoryBudgetSample> samples() const;

private:
    std::atomic<std::size_t> capacity_{120};
    std::deque<GpuMemoryBudgetSample> samples_;
    mutable std::mutex mutex_;
};

enum class DeviceRecoveryState { Ready, Lost, Recovering, Recovered, Failed };

struct DeviceRecoveryStatus {
    DeviceRecoveryState state{DeviceRecoveryState::Ready};
    std::uint64_t generation{0};
    std::uint64_t stateTimestampNanoseconds{0};
    std::string lastError;
    DeviceFaultDiagnostic lastFault;
};

class DeviceRecoveryStateMachine {
public:
    using StateChangedCallback = std::function<void(const DeviceRecoveryStatus&)>;

    explicit DeviceRecoveryStateMachine(StateChangedCallback callback = {});

    bool notify_device_lost(DeviceFaultDiagnostic fault);
    bool begin_recovery();
    bool mark_recovered();
    bool mark_failed(std::string error);
    bool reset();

    DeviceRecoveryState state() const noexcept { return status_.state; }
    const DeviceRecoveryStatus& status() const noexcept { return status_; }
    void set_callback(StateChangedCallback callback) { callback_ = std::move(callback); }

private:
    bool transition(DeviceRecoveryState next, std::string error = {});

    DeviceRecoveryStatus status_;
    StateChangedCallback callback_;
};

const char* to_string(DeviceRecoveryState state) noexcept;

GpuMemoryBudgetSnapshot to_memory_snapshot(const GpuMemoryBudgetAggregate& aggregate);
RenderBudgetFaultCorrelation correlate_budget_fault(const GpuMemoryBudgetAggregate& aggregate,
                                                    const DeviceFaultDiagnostic& fault);
DeviceFaultDiagnostic to_device_fault(const DeviceRecoveryStatus& status,
                                      BackendApi api = BackendApi::Null);
RenderDiagnosticsSnapshot to_diagnostics_snapshot(const RenderGraph& graph,
                                                  const GpuMemoryBudgetAggregate& aggregate,
                                                  const DeviceRecoveryStatus& recovery,
                                                  std::vector<RenderCaptureMarker> markers = {},
                                                  BackendApi api = BackendApi::Null);

} // namespace shinkou::render

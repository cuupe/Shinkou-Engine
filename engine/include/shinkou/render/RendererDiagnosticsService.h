#pragma once

#include "shinkou/render/RenderBudgetService.h"
#include "shinkou/render/RenderCapture.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace shinkou::render {

struct RendererDiagnosticsServiceConfig {
    std::size_t pendingFrameCapacity{3};
    std::size_t budgetWindowCapacity{120};
    std::size_t completedFrameCapacity{8};
    BackendApi api{BackendApi::Null};
    RenderDiagnosticsHooks diagnosticsHooks;
    RenderExternalCaptureHooks externalCaptureHooks;
};

// Providers are invoked by the diagnostics worker after end_frame(). They must
// return an owned snapshot or otherwise synchronize access to engine state.
struct RendererDiagnosticsFrameCallbacks {
    std::function<RenderGraphReport()> graphReport;
    std::function<GpuMemoryBudgetSnapshot()> memoryBudget;
    std::function<DeviceFaultDiagnostic()> deviceFault;
};

struct RendererDiagnosticsFrame {
    std::uint64_t sequence{0};
    std::uint64_t frameIndex{0};
    std::uint64_t beginTimestampNanoseconds{0};
    std::uint64_t endTimestampNanoseconds{0};
    BackendApi api{BackendApi::Null};
    bool processed{false};
    bool dropped{false};
    std::string processingError;
    GpuMemoryBudgetAggregate budget;
    DeviceRecoveryStatus recovery;
    RenderDiagnosticsSnapshot diagnostics;
    std::string json;
    std::string text;
};

class RendererDiagnosticsService;

class RendererDiagnosticsScope {
public:
    RendererDiagnosticsScope() = default;
    ~RendererDiagnosticsScope();

    RendererDiagnosticsScope(const RendererDiagnosticsScope&) = delete;
    RendererDiagnosticsScope& operator=(const RendererDiagnosticsScope&) = delete;
    RendererDiagnosticsScope(RendererDiagnosticsScope&& other) noexcept;
    RendererDiagnosticsScope& operator=(RendererDiagnosticsScope&& other) noexcept;

    void close() noexcept;
    bool active() const noexcept { return active_; }

private:
    friend class RendererDiagnosticsService;
    RendererDiagnosticsScope(RendererDiagnosticsService* owner, std::string name,
                             std::string category, std::uint64_t frameIndex,
                             RenderQueue queue) noexcept;

    RendererDiagnosticsService* owner_{nullptr};
    std::string name_;
    std::string category_;
    std::uint64_t frameIndex_{0};
    RenderQueue queue_{RenderQueue::Graphics};
    bool active_{false};
};

class RendererDiagnosticsService {
public:
    explicit RendererDiagnosticsService(RendererDiagnosticsServiceConfig config = {});
    ~RendererDiagnosticsService();

    RendererDiagnosticsService(const RendererDiagnosticsService&) = delete;
    RendererDiagnosticsService& operator=(const RendererDiagnosticsService&) = delete;

    // These calls are deliberately non-blocking. False means the frame was
    // already open, the service is shutting down, or the internal lock/queue
    // was unavailable; the drop counters expose that condition.
    bool begin_frame(std::uint64_t frameIndex, BackendApi api = BackendApi::Null);
    bool end_frame(RendererDiagnosticsFrameCallbacks callbacks = {});
    bool begin_scope(std::string_view name, std::string_view category = {},
                     std::uint64_t frameIndex = 0,
                     RenderQueue queue = RenderQueue::Graphics);
    bool end_scope(std::string_view name = {}, std::string_view category = {},
                   std::uint64_t frameIndex = 0,
                   RenderQueue queue = RenderQueue::Graphics);
    bool marker(std::string_view name, std::string_view category = {},
                std::uint64_t frameIndex = 0,
                RenderQueue queue = RenderQueue::Graphics);
    RendererDiagnosticsScope scope(std::string_view name, std::string_view category = {},
                                   std::uint64_t frameIndex = 0,
                                   RenderQueue queue = RenderQueue::Graphics);

    bool begin_external_capture(const RenderExternalCaptureRequest& request = {});
    bool end_external_capture();

    bool notify_device_lost(DeviceFaultDiagnostic fault);
    bool begin_recovery();
    bool mark_recovered();
    bool mark_recovery_failed(std::string error);
    bool reset_recovery();

    std::shared_ptr<const RendererDiagnosticsFrame> latest() const noexcept;
    std::shared_ptr<const DeviceRecoveryStatus> recovery_status() const noexcept;
    std::uint64_t dropped_frame_count() const noexcept { return droppedFrames_.load(); }
    std::uint64_t dropped_marker_count() const noexcept { return droppedMarkers_.load(); }
    bool frame_open() const noexcept;

    // Intended for shutdown/tests/tools, never required by the render loop.
    void wait_until_idle();

private:
    struct PendingFrame {
        std::uint64_t sequence{0};
        std::uint64_t frameIndex{0};
        std::uint64_t beginTimestampNanoseconds{0};
        std::uint64_t endTimestampNanoseconds{0};
        BackendApi api{BackendApi::Null};
        std::vector<RenderCaptureMarker> markers;
        RenderCaptureMetadata captureMetadata;
        RendererDiagnosticsFrameCallbacks callbacks;
    };

    void worker_loop();
    void process(PendingFrame pending);
    bool update_recovery(const std::function<bool(DeviceRecoveryStateMachine&)>& operation);
    void publish_recovery();

    RendererDiagnosticsServiceConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable idleCondition_;
    std::deque<PendingFrame> pendingFrames_;
    std::deque<std::shared_ptr<const RendererDiagnosticsFrame>> completedFrames_;
    bool frameOpen_{false};
    bool stopping_{false};
    bool workerBusy_{false};
    std::uint64_t openFrameIndex_{0};
    std::uint64_t openFrameBeginTimestamp_{0};
    BackendApi openFrameApi_{BackendApi::Null};
    std::uint64_t nextSequence_{1};

    RenderCaptureService capture_;
    RenderBudgetService budget_;
    DeviceRecoveryStateMachine recovery_;
    mutable std::mutex recoveryMutex_;
    std::thread worker_;

    std::shared_ptr<const RendererDiagnosticsFrame> latest_;
    std::shared_ptr<const DeviceRecoveryStatus> recoverySnapshot_;
    std::atomic<std::uint64_t> droppedFrames_{0};
    std::atomic<std::uint64_t> droppedMarkers_{0};
};

} // namespace shinkou::render

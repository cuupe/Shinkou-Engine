#include "shinkou/render/RendererDiagnosticsService.h"

#include <chrono>
#include <utility>

namespace shinkou::render {
namespace {

std::uint64_t now_nanoseconds() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

RendererDiagnosticsScope::RendererDiagnosticsScope(RendererDiagnosticsService* owner, std::string name,
                                                   std::string category, std::uint64_t frameIndex,
                                                   RenderQueue queue) noexcept
    : owner_(owner), name_(std::move(name)), category_(std::move(category)),
      frameIndex_(frameIndex), queue_(queue), active_(owner != nullptr) {}

RendererDiagnosticsScope::~RendererDiagnosticsScope() { close(); }

RendererDiagnosticsScope::RendererDiagnosticsScope(RendererDiagnosticsScope&& other) noexcept
    : owner_(other.owner_), name_(std::move(other.name_)), category_(std::move(other.category_)),
      frameIndex_(other.frameIndex_), queue_(other.queue_), active_(other.active_) {
    other.owner_ = nullptr;
    other.active_ = false;
}

RendererDiagnosticsScope& RendererDiagnosticsScope::operator=(RendererDiagnosticsScope&& other) noexcept {
    if (this == &other) return *this;
    close();
    owner_ = other.owner_;
    name_ = std::move(other.name_);
    category_ = std::move(other.category_);
    frameIndex_ = other.frameIndex_;
    queue_ = other.queue_;
    active_ = other.active_;
    other.owner_ = nullptr;
    other.active_ = false;
    return *this;
}

void RendererDiagnosticsScope::close() noexcept {
    if (!active_ || owner_ == nullptr) return;
    try {
        owner_->end_scope(name_, category_, frameIndex_, queue_);
    } catch (...) {
        // Diagnostics must never turn scope teardown into a render failure.
    }
    active_ = false;
    owner_ = nullptr;
}

RendererDiagnosticsService::RendererDiagnosticsService(RendererDiagnosticsServiceConfig config)
    : config_(std::move(config)),
      capture_(config_.diagnosticsHooks, config_.externalCaptureHooks),
      budget_(config_.budgetWindowCapacity),
      recovery_() {
    std::atomic_store_explicit(&latest_, std::shared_ptr<const RendererDiagnosticsFrame>{},
                               std::memory_order_release);
    publish_recovery();
    worker_ = std::thread(&RendererDiagnosticsService::worker_loop, this);
}

RendererDiagnosticsService::~RendererDiagnosticsService() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_one();
    if (worker_.joinable()) worker_.join();
}

bool RendererDiagnosticsService::begin_frame(std::uint64_t frameIndex, BackendApi api) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || stopping_ || frameOpen_) return false;
    frameOpen_ = true;
    openFrameIndex_ = frameIndex;
    openFrameBeginTimestamp_ = now_nanoseconds();
    openFrameApi_ = api == BackendApi::Null ? config_.api : api;
    capture_.clear_markers();
    return true;
}

bool RendererDiagnosticsService::end_frame(RendererDiagnosticsFrameCallbacks callbacks) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || stopping_ || !frameOpen_) return false;
    if (pendingFrames_.size() >= config_.pendingFrameCapacity) {
        frameOpen_ = false;
        capture_.clear_markers();
        droppedFrames_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    PendingFrame pending;
    pending.sequence = nextSequence_++;
    pending.frameIndex = openFrameIndex_;
    pending.beginTimestampNanoseconds = openFrameBeginTimestamp_;
    pending.endTimestampNanoseconds = now_nanoseconds();
    pending.api = openFrameApi_;
    pending.markers = capture_.markers();
    pending.captureMetadata = capture_.metadata();
    pending.callbacks = std::move(callbacks);
    capture_.clear_markers();
    pendingFrames_.push_back(std::move(pending));
    frameOpen_ = false;
    lock.unlock();
    condition_.notify_one();
    return true;
}

bool RendererDiagnosticsService::begin_scope(std::string_view name, std::string_view category,
                                             std::uint64_t frameIndex, RenderQueue queue) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || stopping_ || !frameOpen_) {
        droppedMarkers_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    capture_.begin_scope(name, category, frameIndex == 0 ? openFrameIndex_ : frameIndex, queue);
    return true;
}

bool RendererDiagnosticsService::end_scope(std::string_view name, std::string_view category,
                                           std::uint64_t frameIndex, RenderQueue queue) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || stopping_ || !frameOpen_) {
        droppedMarkers_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    capture_.end_scope(name, category, frameIndex == 0 ? openFrameIndex_ : frameIndex, queue);
    return true;
}

bool RendererDiagnosticsService::marker(std::string_view name, std::string_view category,
                                       std::uint64_t frameIndex, RenderQueue queue) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || stopping_ || !frameOpen_) {
        droppedMarkers_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    capture_.marker(name, category, frameIndex == 0 ? openFrameIndex_ : frameIndex, queue);
    return true;
}

RendererDiagnosticsScope RendererDiagnosticsService::scope(std::string_view name,
                                                           std::string_view category,
                                                           std::uint64_t frameIndex,
                                                           RenderQueue queue) {
    if (!begin_scope(name, category, frameIndex, queue)) return {};
    return RendererDiagnosticsScope(this, std::string(name), std::string(category), frameIndex, queue);
}

bool RendererDiagnosticsService::begin_external_capture(const RenderExternalCaptureRequest& request) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || stopping_) return false;
    return capture_.begin_external_capture(request);
}

bool RendererDiagnosticsService::end_external_capture() {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || stopping_) return false;
    capture_.end_external_capture();
    return true;
}

bool RendererDiagnosticsService::update_recovery(
    const std::function<bool(DeviceRecoveryStateMachine&)>& operation) {
    std::unique_lock<std::mutex> lock(recoveryMutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    const bool changed = operation(recovery_);
    if (changed) publish_recovery();
    return changed;
}

void RendererDiagnosticsService::publish_recovery() {
    std::atomic_store_explicit(&recoverySnapshot_,
                               std::make_shared<const DeviceRecoveryStatus>(recovery_.status()),
                               std::memory_order_release);
}

bool RendererDiagnosticsService::notify_device_lost(DeviceFaultDiagnostic fault) {
    return update_recovery([fault = std::move(fault)](DeviceRecoveryStateMachine& state) mutable {
        return state.notify_device_lost(std::move(fault));
    });
}

bool RendererDiagnosticsService::begin_recovery() {
    return update_recovery([](DeviceRecoveryStateMachine& state) { return state.begin_recovery(); });
}

bool RendererDiagnosticsService::mark_recovered() {
    return update_recovery([](DeviceRecoveryStateMachine& state) { return state.mark_recovered(); });
}

bool RendererDiagnosticsService::mark_recovery_failed(std::string error) {
    return update_recovery([error = std::move(error)](DeviceRecoveryStateMachine& state) mutable {
        return state.mark_failed(std::move(error));
    });
}

bool RendererDiagnosticsService::reset_recovery() {
    return update_recovery([](DeviceRecoveryStateMachine& state) { return state.reset(); });
}

std::shared_ptr<const RendererDiagnosticsFrame> RendererDiagnosticsService::latest() const noexcept {
    return std::atomic_load_explicit(&latest_, std::memory_order_acquire);
}

std::shared_ptr<const DeviceRecoveryStatus> RendererDiagnosticsService::recovery_status() const noexcept {
    return std::atomic_load_explicit(&recoverySnapshot_, std::memory_order_acquire);
}

bool RendererDiagnosticsService::frame_open() const noexcept {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    return !lock.owns_lock() || frameOpen_;
}

void RendererDiagnosticsService::wait_until_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idleCondition_.wait(lock, [this] { return pendingFrames_.empty() && !workerBusy_; });
}

void RendererDiagnosticsService::worker_loop() {
    for (;;) {
        PendingFrame pending;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !pendingFrames_.empty(); });
            if (pendingFrames_.empty() && stopping_) return;
            pending = std::move(pendingFrames_.front());
            pendingFrames_.pop_front();
            workerBusy_ = true;
        }
        process(std::move(pending));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            workerBusy_ = false;
        }
        idleCondition_.notify_all();
    }
}

void RendererDiagnosticsService::process(PendingFrame pending) {
    auto frame = std::make_shared<RendererDiagnosticsFrame>();
    frame->sequence = pending.sequence;
    frame->frameIndex = pending.frameIndex;
    frame->beginTimestampNanoseconds = pending.beginTimestampNanoseconds;
    frame->endTimestampNanoseconds = pending.endTimestampNanoseconds;
    frame->api = pending.api;
    frame->diagnostics.timestampNanoseconds = pending.endTimestampNanoseconds;
    frame->diagnostics.captureMarkers = pending.markers;
    frame->diagnostics.captureMetadata = pending.captureMetadata;

    RenderGraphReport graphReport;
    GpuMemoryBudgetSnapshot memorySnapshot;
    DeviceFaultDiagnostic fault;
    try {
        if (pending.callbacks.graphReport) graphReport = pending.callbacks.graphReport();
        if (pending.callbacks.memoryBudget) {
            memorySnapshot = pending.callbacks.memoryBudget();
        } else {
            memorySnapshot = query_gpu_memory_budget(config_.diagnosticsHooks, pending.api);
        }
        if (pending.callbacks.deviceFault) {
            fault = pending.callbacks.deviceFault();
        } else {
            fault = query_device_fault(config_.diagnosticsHooks, pending.api);
        }
        budget_.record(memorySnapshot, pending.frameIndex);
        frame->budget = budget_.aggregate();
        frame->diagnostics.memory = to_memory_snapshot(frame->budget);
        frame->diagnostics.graph = std::move(graphReport);
        frame->diagnostics.budgetViolations = frame->budget.violations;
        frame->diagnostics.deviceFault = fault;
        frame->diagnostics.budgetFaultCorrelation = correlate_budget_fault(frame->budget, fault);

        if (fault.deviceLost) {
            update_recovery([&fault](DeviceRecoveryStateMachine& state) {
                return state.notify_device_lost(fault);
            });
        }
        const auto recovery = recovery_status();
        if (recovery) frame->recovery = *recovery;
        if (frame->diagnostics.deviceFault.api == BackendApi::Null) {
            frame->diagnostics.deviceFault.api = pending.api;
        }
        frame->processed = true;
    } catch (const std::exception& error) {
        frame->processingError = error.what();
    } catch (...) {
        frame->processingError = "unknown diagnostics worker error";
    }

    frame->json = serialize_json(frame->diagnostics);
    frame->text = serialize_text(frame->diagnostics);
    auto immutableFrame = std::shared_ptr<const RendererDiagnosticsFrame>(std::move(frame));
    std::atomic_store_explicit(&latest_, immutableFrame, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completedFrames_.push_back(immutableFrame);
        while (completedFrames_.size() > config_.completedFrameCapacity) completedFrames_.pop_front();
    }
}

} // namespace shinkou::render

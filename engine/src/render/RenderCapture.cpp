#include "shinkou/render/RenderCapture.h"

#include <chrono>
#include <utility>

namespace shinkou::render {

namespace {
std::uint64_t now_nanoseconds() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

RenderCaptureScope::RenderCaptureScope(RenderCaptureService* owner, std::string name,
                                       std::string category, std::uint64_t frameIndex,
                                       RenderQueue queue) noexcept
    : owner_(owner), name_(std::move(name)), category_(std::move(category)),
      frameIndex_(frameIndex), queue_(queue), active_(owner != nullptr) {}

RenderCaptureScope::~RenderCaptureScope() { close(); }

RenderCaptureScope::RenderCaptureScope(RenderCaptureScope&& other) noexcept
    : owner_(other.owner_), name_(std::move(other.name_)), category_(std::move(other.category_)),
      frameIndex_(other.frameIndex_), queue_(other.queue_), active_(other.active_) {
    other.owner_ = nullptr;
    other.active_ = false;
}

RenderCaptureScope& RenderCaptureScope::operator=(RenderCaptureScope&& other) noexcept {
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

void RenderCaptureScope::close() noexcept {
    if (!active_ || owner_ == nullptr) return;
    try {
        owner_->end_scope(name_, category_, frameIndex_, queue_);
    } catch (...) {
        // A scope must not throw from a destructor. The marker already emitted
        // by the begin operation remains useful even if a custom sink fails.
    }
    active_ = false;
    owner_ = nullptr;
}

RenderCaptureService::RenderCaptureService(RenderDiagnosticsHooks diagnosticsHooks,
                                           RenderExternalCaptureHooks externalHooks)
    : diagnosticsHooks_(std::move(diagnosticsHooks)), externalHooks_(std::move(externalHooks)),
      recorder_(diagnosticsHooks_) {}

void RenderCaptureService::set_diagnostics_hooks(RenderDiagnosticsHooks hooks) {
    diagnosticsHooks_ = std::move(hooks);
    recorder_.set_hooks(diagnosticsHooks_);
}

void RenderCaptureService::set_external_hooks(RenderExternalCaptureHooks hooks) {
    externalHooks_ = std::move(hooks);
}

RenderCaptureScope RenderCaptureService::scope(std::string_view name, std::string_view category,
                                               std::uint64_t frameIndex, RenderQueue queue) {
    begin_scope(name, category, frameIndex, queue);
    return RenderCaptureScope(this, std::string(name), std::string(category), frameIndex, queue);
}

void RenderCaptureService::begin_scope(std::string_view name, std::string_view category,
                                       std::uint64_t frameIndex, RenderQueue queue) {
    recorder_.begin(name, category, frameIndex, queue);
    if (externalCaptureActive_ && externalHooks_.setMarker) externalHooks_.setMarker(name);
}

void RenderCaptureService::end_scope(std::string_view name, std::string_view category,
                                     std::uint64_t frameIndex, RenderQueue queue) {
    recorder_.end(name, category, frameIndex, queue);
    if (externalCaptureActive_ && externalHooks_.setMarker) {
        externalHooks_.setMarker(name.empty() ? "scope.end" : name);
    }
}

void RenderCaptureService::marker(std::string_view name, std::string_view category,
                                  std::uint64_t frameIndex, RenderQueue queue) {
    recorder_.instant(name, category, frameIndex, queue);
    if (externalCaptureActive_ && externalHooks_.setMarker) externalHooks_.setMarker(name);
}

bool RenderCaptureService::begin_external_capture(const RenderExternalCaptureRequest& request) {
    if (externalCaptureActive_ || !externalHooks_.beginCapture) return false;
    try {
        if (!externalHooks_.beginCapture(request)) return false;
    } catch (...) {
        return false;
    }
    externalCaptureActive_ = true;
    metadata_.tool = to_string(request.tool);
    metadata_.name = request.name;
    metadata_.filePath = request.filePath;
    metadata_.beginTimestampNanoseconds = now_nanoseconds();
    metadata_.endTimestampNanoseconds = 0;
    metadata_.active = true;
    metadata_.allowAsync = request.allowAsync;
    return true;
}

void RenderCaptureService::end_external_capture() {
    if (!externalCaptureActive_) return;
    externalCaptureActive_ = false;
    metadata_.active = false;
    metadata_.endTimestampNanoseconds = now_nanoseconds();
    if (externalHooks_.endCapture) {
        try {
            externalHooks_.endCapture();
        } catch (...) {
            // External tooling must not break the render loop.
        }
    }
}

RenderCaptureMetadata RenderCaptureService::metadata() const {
    auto result = metadata_;
    result.markerCount = recorder_.markers().size();
    return result;
}

RenderDiagnosticsSnapshot RenderCaptureService::snapshot(const RenderGraph& graph, BackendApi api) const {
    auto result = collect_diagnostics(diagnosticsHooks_, graph, recorder_.markers(), api);
    result.captureMetadata = metadata();
    return result;
}

std::string RenderCaptureService::graph_json(const RenderGraph& graph, BackendApi api) const {
    return serialize_json(snapshot(graph, api));
}

std::string RenderCaptureService::graph_text(const RenderGraph& graph, BackendApi api) const {
    return serialize_text(snapshot(graph, api));
}

void RenderCaptureService::clear_markers() { recorder_.clear(); }

const char* to_string(RenderCaptureTool tool) noexcept {
    switch (tool) {
    case RenderCaptureTool::None: return "none";
    case RenderCaptureTool::RenderDoc: return "renderdoc";
    case RenderCaptureTool::PIX: return "pix";
    case RenderCaptureTool::Custom: return "custom";
    }
    return "unknown";
}

RenderDiagnosticsSnapshot capture_render_graph_snapshot(const RenderGraph& graph, BackendApi api) {
    return collect_diagnostics({}, graph, {}, api);
}

std::string capture_render_graph_json(const RenderGraph& graph, BackendApi api) {
    return serialize_json(capture_render_graph_snapshot(graph, api));
}

std::string capture_render_graph_text(const RenderGraph& graph, BackendApi api) {
    return serialize_text(capture_render_graph_snapshot(graph, api));
}

} // namespace shinkou::render

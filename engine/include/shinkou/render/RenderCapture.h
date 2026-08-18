#pragma once

#include "shinkou/render/RenderDiagnostics.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::render {

enum class RenderCaptureTool { None, RenderDoc, PIX, Custom };

struct RenderExternalCaptureRequest {
    RenderCaptureTool tool{RenderCaptureTool::None};
    std::string name;
    std::string filePath;
    bool allowAsync{true};
};

struct RenderExternalCaptureHooks {
    // The hook owns all platform/tool SDK details. Returning false rejects the
    // capture and leaves the service inactive.
    std::function<bool(const RenderExternalCaptureRequest&)> beginCapture;
    std::function<void()> endCapture;
    std::function<void(std::string_view)> setMarker;
};

class RenderCaptureService;

class RenderCaptureScope {
public:
    RenderCaptureScope() = default;
    ~RenderCaptureScope();

    RenderCaptureScope(const RenderCaptureScope&) = delete;
    RenderCaptureScope& operator=(const RenderCaptureScope&) = delete;
    RenderCaptureScope(RenderCaptureScope&& other) noexcept;
    RenderCaptureScope& operator=(RenderCaptureScope&& other) noexcept;

    void close() noexcept;
    bool active() const noexcept { return active_; }

private:
    friend class RenderCaptureService;
    RenderCaptureScope(RenderCaptureService* owner, std::string name, std::string category,
                       std::uint64_t frameIndex, RenderQueue queue) noexcept;

    RenderCaptureService* owner_{nullptr};
    std::string name_;
    std::string category_;
    std::uint64_t frameIndex_{0};
    RenderQueue queue_{RenderQueue::Graphics};
    bool active_{false};
};

class RenderCaptureService {
public:
    explicit RenderCaptureService(RenderDiagnosticsHooks diagnosticsHooks = {},
                                  RenderExternalCaptureHooks externalHooks = {});

    void set_diagnostics_hooks(RenderDiagnosticsHooks hooks);
    void set_external_hooks(RenderExternalCaptureHooks hooks);

    RenderCaptureScope scope(std::string_view name, std::string_view category = {},
                             std::uint64_t frameIndex = 0,
                             RenderQueue queue = RenderQueue::Graphics);
    void begin_scope(std::string_view name, std::string_view category = {},
                     std::uint64_t frameIndex = 0,
                     RenderQueue queue = RenderQueue::Graphics);
    void end_scope(std::string_view name = {}, std::string_view category = {},
                   std::uint64_t frameIndex = 0,
                   RenderQueue queue = RenderQueue::Graphics);
    void marker(std::string_view name, std::string_view category = {},
                std::uint64_t frameIndex = 0,
                RenderQueue queue = RenderQueue::Graphics);

    bool begin_external_capture(const RenderExternalCaptureRequest& request = {});
    void end_external_capture();
    bool external_capture_active() const noexcept { return externalCaptureActive_; }
    RenderCaptureMetadata metadata() const;

    RenderDiagnosticsSnapshot snapshot(const RenderGraph& graph,
                                       BackendApi api = BackendApi::Null) const;
    std::string graph_json(const RenderGraph& graph, BackendApi api = BackendApi::Null) const;
    std::string graph_text(const RenderGraph& graph, BackendApi api = BackendApi::Null) const;

    const std::vector<RenderCaptureMarker>& markers() const noexcept { return recorder_.markers(); }
    void clear_markers();

private:
    RenderDiagnosticsHooks diagnosticsHooks_;
    RenderExternalCaptureHooks externalHooks_;
    RenderCaptureRecorder recorder_;
    RenderCaptureMetadata metadata_;
    bool externalCaptureActive_{false};
};

const char* to_string(RenderCaptureTool tool) noexcept;

RenderDiagnosticsSnapshot capture_render_graph_snapshot(const RenderGraph& graph,
                                                         BackendApi api = BackendApi::Null);
std::string capture_render_graph_json(const RenderGraph& graph,
                                      BackendApi api = BackendApi::Null);
std::string capture_render_graph_text(const RenderGraph& graph,
                                      BackendApi api = BackendApi::Null);

} // namespace shinkou::render

#include "shinkou/renderlib/Renderer.h"

namespace shinkou::renderlib {
namespace {
class NullBackend final : public IDisplayBackend {
public:
    bool initialize(const RendererConfig&) override { m_capabilities.initialized = true; return true; }
    void shutdown() override { m_capabilities.initialized = false; }
    bool resize(Size) override { return true; }
    void begin_frame(Color) override {}
    void submit(const DisplayList& list) override { m_stats.commands += list.size(); m_stats.drawCalls += list.size(); }
    bool end_frame() override { ++m_stats.frames; return true; }
    RendererCapabilities capabilities() const override { return m_capabilities; }
    RendererStats stats() const override { return m_stats; }
    std::string last_error() const override { return {}; }
private:
    RendererCapabilities m_capabilities{};
    RendererStats m_stats{};
};
}

DisplayRenderer::DisplayRenderer(std::unique_ptr<IDisplayBackend> backend) : m_backend(std::move(backend)) {}
bool DisplayRenderer::initialize(const RendererConfig& config) { if (!m_backend) m_backend = create_null_backend(); m_config = config; m_initialized = m_backend->initialize(config); return m_initialized; }
void DisplayRenderer::shutdown() { if (m_backend && m_initialized) m_backend->shutdown(); m_initialized = false; }
bool DisplayRenderer::resize(Size size) { m_config.size = size; return m_backend && m_backend->resize(size); }
bool DisplayRenderer::render(const DisplayList& list, Color clear) { if (!m_backend || !m_initialized) return false; m_backend->begin_frame(clear); m_backend->submit(list); return m_backend->end_frame(); }
void DisplayRenderer::set_backend(std::unique_ptr<IDisplayBackend> backend) { if (m_initialized) shutdown(); m_backend = std::move(backend); }
RendererCapabilities DisplayRenderer::capabilities() const { return m_backend ? m_backend->capabilities() : RendererCapabilities{}; }
RendererStats DisplayRenderer::stats() const { return m_backend ? m_backend->stats() : RendererStats{}; }
std::string DisplayRenderer::last_error() const { return m_backend ? m_backend->last_error() : "display backend is not set"; }
std::unique_ptr<IDisplayBackend> create_null_backend() { return std::make_unique<NullBackend>(); }

} // namespace shinkou::renderlib


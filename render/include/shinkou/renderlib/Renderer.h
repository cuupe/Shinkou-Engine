#pragma once

#include "DisplayList.h"
#include <cstdint>
#include <memory>
#include <string>

namespace shinkou::renderlib {

struct RendererConfig { Size size{1280, 720}; float dpiScale = 1.0f; bool vsync = true; };
struct RendererCapabilities { bool initialized = false; bool software = false; bool present = false; PixelFormat format = PixelFormat::RGBA8Unorm; };
struct RendererStats { std::uint64_t frames = 0; std::uint64_t commands = 0; std::uint64_t drawCalls = 0; std::uint64_t presents = 0; };

class IDisplayBackend {
public:
    virtual ~IDisplayBackend() = default;
    virtual bool initialize(const RendererConfig& config) = 0;
    virtual void shutdown() = 0;
    virtual bool resize(Size size) = 0;
    virtual void begin_frame(Color clear) = 0;
    virtual void submit(const DisplayList& list) = 0;
    virtual bool end_frame() = 0;
    virtual RendererCapabilities capabilities() const = 0;
    virtual RendererStats stats() const = 0;
    virtual std::string last_error() const = 0;
};

class DisplayRenderer {
public:
    explicit DisplayRenderer(std::unique_ptr<IDisplayBackend> backend = {});
    bool initialize(const RendererConfig& config = {});
    void shutdown();
    bool resize(Size size);
    bool render(const DisplayList& list, Color clear = {0, 0, 0, 1});
    void set_backend(std::unique_ptr<IDisplayBackend> backend);
    IDisplayBackend* backend() { return m_backend.get(); }
    const IDisplayBackend* backend() const { return m_backend.get(); }
    RendererCapabilities capabilities() const;
    RendererStats stats() const;
    std::string last_error() const;

private:
    std::unique_ptr<IDisplayBackend> m_backend;
    RendererConfig m_config{};
    bool m_initialized = false;
};

std::unique_ptr<IDisplayBackend> create_null_backend();

} // namespace shinkou::renderlib


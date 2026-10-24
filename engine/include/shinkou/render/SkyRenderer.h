#pragma once

#include "shinkou/render/RenderScene.h"
#include <string>

namespace shinkou::render {

struct SkySettings {
    bool enabled{false};
    // Caller-owned, equirectangular 2D environment. Empty uses a gradient.
    ResourceHandle environment{};
    float intensity{1.0f};
    float rotation{0.0f};
    math::Vec3 zenith{0.03f,0.08f,0.18f};
    math::Vec3 horizon{0.4f,0.45f,0.5f};
    math::Vec3 ground{0.03f,0.025f,0.02f};
};

bool validate_sky_settings(const SkySettings& settings,std::string& error);

// Draws at the far plane without clearing color or writing scene depth.
class SkyRenderer {
    ResourceHandle vertex_{},fragment_{},pipeline_{},sampler_{},fallback_{};
    std::string error_;
public:
    bool initialize(Renderer& renderer,std::string colorFormat="rgba16f");
    bool record(Renderer& renderer,RenderGraph& graph,const RenderView& view,
                const SkySettings& settings,ResourceHandle color,ResourceHandle depth);
    void release(Renderer& renderer);
    const std::string& last_error() const noexcept {return error_;}
};
}

#pragma once

#include "shinkou/render/RenderBackend.h"
#include <string>
#include <vector>

namespace shinkou::render {

enum class RenderFeature {
    DepthPrepass,
    CascadedShadows,
    ScreenSpaceAmbientOcclusion,
    TemporalAntiAliasing,
    Bloom,
    Exposure,
    ScreenSpaceReflections,
    OcclusionCulling,
    LodSelection,
    MeshletRasterization,
    RayTracing,
    VariableRateShading
};

struct RenderFeatureRequest {
    RenderFeature feature{RenderFeature::DepthPrepass};
    bool required{false};
    bool enabled{true};
};

struct RenderFeatureDecision {
    RenderFeature feature{RenderFeature::DepthPrepass};
    bool enabled{false};
    bool supported{false};
    bool fallback{false};
    std::string reason;
};

struct RenderFeaturePlan {
    bool valid{false};
    std::string error;
    std::vector<RenderFeatureDecision> decisions;
    std::vector<RenderFeature> executionOrder;

    const RenderFeatureDecision* decision(RenderFeature feature) const noexcept;
};

const char* to_string(RenderFeature feature) noexcept;
RenderFeaturePlan build_render_feature_plan(const RenderCapabilities& capabilities,
                                            const std::vector<RenderFeatureRequest>& requests);

} // namespace shinkou::render

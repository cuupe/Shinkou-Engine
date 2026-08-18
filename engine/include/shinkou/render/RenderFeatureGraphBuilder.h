#pragma once

#include "shinkou/render/AdvancedRenderFeatures.h"
#include "shinkou/render/LightingPipeline.h"
#include "shinkou/render/RenderScene.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace shinkou::render {

enum class RenderFeatureGraphStage {
    DepthPrepass,
    ShadowCascades,
    ClusterCulling,
    Opaque,
    SSAO,
    SSR,
    TAA,
    Bloom,
    Exposure,
    ToneMap,
    Present
};

const char* to_string(RenderFeatureGraphStage stage) noexcept;

struct RenderFeatureGraphDesc {
    RenderCapabilities capabilities{};
    std::uint32_t width{1280};
    std::uint32_t height{720};
    std::size_t maxLights{256};
    std::vector<RenderFeatureRequest> featureRequests;
    ShadowSettings shadows{};
    LightingPipelineDesc lighting{};
    std::vector<PostProcessStageDesc> postProcessStages;
    ResourceHandle depthTarget{};
    TextureDesc depthDescription{1280, 720, 1, 1, "d24s8", false, false, {}};
    ResourceHandle presentTarget{};
    TextureDesc presentDescription{1280, 720, 1, 1, "rgba8", true, false, {}};
};

struct RenderFeatureGraphResource {
    std::string name;
    ResourceHandle handle{};
    ResourceDesc description{};
    bool external{false};
};

struct RenderFeatureGraphResourceContract {
    std::string name;
    ResourceHandle handle{};
    ResourceDesc description{};
    ResourceUsage usage{ResourceUsage::Unknown};
};

struct RenderFeatureGraphStageDesc {
    RenderFeatureGraphStage stage{RenderFeatureGraphStage::Opaque};
    std::string name;
    RenderQueue queue{RenderQueue::Graphics};
    std::vector<std::string> dependsOn;
    std::vector<RenderFeatureGraphResourceContract> inputs;
    std::vector<RenderFeatureGraphResourceContract> outputs;
    bool enabled{false};
    bool fallback{false};
    std::string reason;
};

struct RenderFeatureGraphPass {
    RenderFeatureGraphStage stage{RenderFeatureGraphStage::Opaque};
    std::string name;
    std::size_t passIndex{static_cast<std::size_t>(-1)};
    RenderQueue queue{RenderQueue::Graphics};
    std::vector<ResourceAccess> accesses;
    PostProcessStageDesc postProcess{};
    bool enabled{false};
    bool fallback{false};
    std::string reason;
    // The descriptive contract is additive to the legacy pass fields. It
    // makes stage ordering and resource ownership inspectable by extensions.
    RenderFeatureGraphStageDesc description;
};

struct RenderFeatureGraphPlan {
    bool valid{false};
    std::string error;
    std::vector<std::string> diagnostics;
    RenderFeaturePlan featurePlan;
    LightingPipelinePlan lightingPlan;
    std::vector<RenderFeatureGraphResource> resources;
    std::vector<RenderFeatureGraphPass> passes;
};

class RenderFeatureGraphBuilder {
public:
    static RenderFeatureGraphPlan build(RenderGraph& graph, const RenderFeatureGraphDesc& description);
};

} // namespace shinkou::render

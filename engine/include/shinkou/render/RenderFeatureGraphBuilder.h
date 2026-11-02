#pragma once

#include "shinkou/render/AdvancedRenderFeatures.h"
#include "shinkou/render/LightingPipeline.h"
#include "shinkou/render/RenderScene.h"
#include "shinkou/render/SkyRenderer.h"
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
    Present,
    Custom,
    MotionVectors,
    MotionBlur,
    Sky
};

const char *to_string(RenderFeatureGraphStage stage) noexcept;

enum class RenderImageDomain { Inherit, LinearHDR, DisplaySRGB, Data };

struct RenderStageInput {
    std::string name;
    // A preceding stage's name, or a named frame resource such as depth.
    std::string resource;
    ResourceUsage usage{ResourceUsage::ShaderRead};
};

struct RenderPipelineStage {
    std::string name;
    RenderFeatureGraphStage stage{RenderFeatureGraphStage::Custom};
    // $color follows the last stage that replaced the color stream.
    std::string source{"$color"};
    std::vector<RenderStageInput> inputs;
    std::vector<std::string> dependsOn;
    bool enabled{true};
    // Custom stages may produce an auxiliary image or have only side effects.
    bool replacesColor{true};
    bool producesImage{true};
    RenderImageDomain outputDomain{RenderImageDomain::Inherit};
    std::optional<TextureDesc> output;
    RenderQueue queue{RenderQueue::Graphics};
};

struct RenderPipelineExternalResource {
    std::string name;
    ResourceHandle handle{};
    ResourceDesc description{};
    RenderImageDomain domain{RenderImageDomain::Data};
};

struct RenderFeatureGraphStageDesc;
struct RenderFeatureGraphCommands {
    RenderGraph::PassCallback record;
    std::vector<ResourceAccess> accesses;
    std::string error;
    bool clearAttachments{true};
    RenderGraph::RecordCallback recordCommands;
    // prepare() may register intermediate or complete native passes. Their
    // indices are checked and exposed in the resulting stage plan.
    std::vector<std::size_t> expandedPasses;
};

struct RenderFeatureGraphExecutor {
    RenderFeatureGraphStage stage{RenderFeatureGraphStage::Opaque};
    RenderGraph::PassCallback record;
    // Additional geometry, material and parameter accesses owned by this
    // implementation. Import persistent resources before building the graph.
    std::vector<ResourceAccess> accesses;
    std::function<RenderFeatureGraphCommands(RenderGraph &, const RenderFeatureGraphStageDesc &)>
        prepare;
    // Empty names are defaults by kind; a named executor overrides one node.
    std::string name;
};

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
    std::vector<RenderFeatureGraphExecutor> executors;
    RenderView view{};
    math::Mat4 previousViewProjection{math::Mat4::Identity()};
    bool historyValid{false};
    std::vector<LightRenderItem> lights;
    bool preferComputePostProcess{true};
    // An existing scene pass may feed the standard image stages. In this
    // mode sceneColor/depthTarget must already be produced or imported.
    ResourceHandle sceneColor{};
    TextureDesc sceneColorDescription{};
    std::vector<RenderPipelineStage> stages;
    bool recordSceneColor{false};
    ResourceHandle shadowTarget{};
    TextureDesc shadowDescription{};
    ResourceHandle motionVectorTarget{};
    TextureDesc motionVectorDescription{};
    float deltaSeconds{1.0f / 60.0f};
    bool exposureHistoryValid{false};
    std::vector<RenderPipelineExternalResource> resources;
    SkySettings sky;
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
    PostProcessStageDesc postProcess;
    RenderImageDomain inputDomain{RenderImageDomain::LinearHDR};
    RenderImageDomain outputDomain{RenderImageDomain::LinearHDR};
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
    std::vector<std::size_t> passIndices;
};

struct RenderFeatureGraphPlan {
    bool valid{false};
    bool executable{false};
    std::string error;
    std::vector<std::string> diagnostics;
    RenderFeaturePlan featurePlan;
    LightingPipelinePlan lightingPlan;
    std::vector<RenderFeatureGraphResource> resources;
    std::vector<RenderFeatureGraphPass> passes;
};

class RenderFeatureGraphBuilder {
  public:
    // Descriptive planning does not register executable passes or claim that
    // the selected algorithms have an implementation.
    static RenderFeatureGraphPlan plan(RenderGraph &graph,
                                       const RenderFeatureGraphDesc &description);
    // Transactional: a missing implementation or invalid dependency leaves
    // the supplied graph unchanged.
    static RenderFeatureGraphPlan build(RenderGraph &graph,
                                        const RenderFeatureGraphDesc &description);
    static std::vector<RenderPipelineStage> default_stages();
};

} // namespace shinkou::render

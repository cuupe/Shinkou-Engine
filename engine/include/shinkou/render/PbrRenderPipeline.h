#pragma once

#include "shinkou/render/PostProcess.h"
#include "shinkou/render/RenderScene.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace shinkou::render {

struct RenderDescriptorValidationResult {
    std::vector<std::string> errors;

    bool valid() const noexcept { return errors.empty(); }
};

struct IblParameters {
    ResourceHandle irradianceTexture{};
    ResourceHandle prefilteredEnvironment{};
    ResourceHandle brdfLut{};
    float intensity{1.0f};
    float rotation{0.0f};
};

// The PBR model deliberately contains only shader-facing values and resource
// handles. It does not depend on a backend material object or a game scene.
struct PbrMaterialDesc {
    std::string name;
    math::Vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic{0.0f};
    float roughness{1.0f};
    float normalScale{1.0f};
    float occlusionStrength{1.0f};
    math::Vec3 emissiveFactor{};
    float emissiveStrength{1.0f};
    float alphaCutoff{0.5f};
    bool alphaTest{false};
    bool doubleSided{false};
    ResourceHandle baseColorTexture{};
    ResourceHandle metallicRoughnessTexture{};
    ResourceHandle normalTexture{};
    ResourceHandle occlusionTexture{};
    ResourceHandle emissiveTexture{};
    IblParameters ibl{};
};

RenderDescriptorValidationResult validate_pbr_material(const PbrMaterialDesc& material);
RenderDescriptorValidationResult validate_light_component(const LightComponent& light);

struct LightClusterDesc {
    std::uint32_t viewportWidth{1};
    std::uint32_t viewportHeight{1};
    std::uint32_t tileSize{16};
    std::uint32_t depthSlices{24};
    std::uint32_t maxLightsPerCluster{64};
    float nearPlane{0.05f};
    float farPlane{1000.0f};
    bool logarithmicDepth{true};
};

struct LightClusterGrid {
    std::uint32_t tilesX{0};
    std::uint32_t tilesY{0};
    std::uint32_t slices{0};
    std::uint64_t clusterCount{0};
};

RenderDescriptorValidationResult validate_light_cluster_desc(const LightClusterDesc& description);
LightClusterGrid make_light_cluster_grid(const LightClusterDesc& description);

struct LightClusterBuildResult {
    LightClusterGrid grid{};
    std::vector<std::uint32_t> directionalLightIndices;
    std::vector<std::uint32_t> clusterLightCounts;
    std::vector<std::uint32_t> clusterLightIndices;
    std::size_t pointLightCount{0};
    std::size_t spotLightCount{0};
    std::size_t rejectedLightCount{0};
    std::size_t overflowLightCount{0};
    RenderDescriptorValidationResult validation;

    bool valid() const noexcept { return validation.valid(); }
};

// Produces a deterministic CPU-side cluster layout for validation, tooling,
// and upload preparation. A backend may replace the actual culling work with
// the scheduled compute pass without changing this data contract.
LightClusterBuildResult build_light_clusters(const LightClusterDesc& description,
    const RenderView& view, const std::vector<LightRenderItem>& lights);

struct ClusteredLightingDesc {
    LightClusterDesc grid{};
    ResourceHandle lightBuffer{};
    BufferDesc lightBufferDescription{};
    ResourceHandle depthTexture{};
    TextureDesc depthDescription{};
    ResourceHandle cullingPipeline{};
    PipelineDesc cullingPipelineDescription{};
    std::uint32_t lightSlot{0};
    std::uint32_t depthSlot{1};
    std::uint32_t clusterSlot{2};
    std::uint32_t indexSlot{3};
    std::uint32_t parameterSlot{4};
};

struct ClusteredLightingPlan {
    bool scheduled{false};
    std::size_t passIndex{static_cast<std::size_t>(-1)};
    ResourceHandle clusterBuffer{};
    ResourceHandle clusterIndexBuffer{};
    ResourceHandle parameterBuffer{};
    LightClusterBuildResult build;
    RenderDescriptorValidationResult validation;

    bool valid() const noexcept { return scheduled && validation.valid() && build.valid(); }
};

class ClusteredForwardScheduler {
public:
    static ClusteredLightingPlan schedule(RenderGraph& graph, const RenderScene& scene,
        const ClusteredLightingDesc& description);
};

}

#pragma once

#include "shinkou/render/PostProcess.h"
#include "shinkou/render/RenderScene.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace shinkou::render {

struct PbrLightingParameters {
    math::Vec3 baseColor{1.0f, 1.0f, 1.0f};
    float metallic{0.0f};
    float roughness{0.5f};
    float normalScale{1.0f};
    float occlusionStrength{1.0f};
    math::Vec3 emissiveColor{};
    float emissiveStrength{0.0f};
    float clearCoat{0.0f};
    float clearCoatRoughness{0.1f};
};

struct IblLightingParameters {
    ResourceHandle environmentMap{};
    ResourceHandle irradianceMap{};
    ResourceHandle prefilteredMap{};
    ResourceHandle brdfLut{};
    ResourceHandle sampler{};
    float diffuseIntensity{1.0f};
    float specularIntensity{1.0f};
    float maxSpecularLod{5.0f};
    float environmentRotation{0.0f};
};

// A backend-neutral, frame-local light record. Direction is the light's
// forward vector in world space; point and spot lights also use position.
struct LightingLight {
    Entity entity{};
    LightType type{LightType::Directional};
    math::Vec3 position{};
    math::Vec3 direction{0.0f, 0.0f, 1.0f};
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity{1.0f};
    float range{10.0f};
    float innerCone{0.5f};
    float outerCone{0.8f};
    bool castsShadow{false};
    bool enabled{true};
};

std::vector<LightingLight> make_lighting_lights(const std::vector<LightRenderItem>& lights);

enum class LightBinningMode { Tiles, Clusters };

struct LightBinningDesc {
    LightBinningMode mode{LightBinningMode::Clusters};
    std::uint32_t viewportWidth{1};
    std::uint32_t viewportHeight{1};
    std::uint32_t tileSize{16};
    std::uint32_t depthSlices{24};
    std::uint32_t maxLightsPerBin{128};
    float nearPlane{0.05f};
    float farPlane{1000.0f};
};

struct LightBinRange {
    std::uint32_t offset{0};
    std::uint32_t count{0};
};

struct LightBinningPlan {
    LightBinningDesc description{};
    std::uint32_t tileCountX{0};
    std::uint32_t tileCountY{0};
    std::uint32_t sliceCount{0};
    std::vector<LightBinRange> bins;
    std::vector<std::uint32_t> lightIndices;
    std::size_t droppedLightCount{0};
    std::size_t invalidLightCount{0};
    std::size_t directionalLightCount{0};
    bool valid{false};

    std::size_t bin_index(std::uint32_t tileX, std::uint32_t tileY,
                          std::uint32_t slice = 0) const noexcept;
    const LightBinRange* range(std::uint32_t tileX, std::uint32_t tileY,
                               std::uint32_t slice = 0) const noexcept;
};

LightBinningPlan build_light_binning(const RenderView& view,
                                     const std::vector<LightingLight>& lights,
                                     const LightBinningDesc& description,
                                     std::string* error = nullptr);

struct LightingStageDesc {
    // This embeds the existing post-process resource/parameter description so
    // the sorted result can be handed to PostProcessPipeline after a backend
    // selects pipelines and targets.
    PostProcessStageDesc postProcess{};
    std::vector<std::string> dependsOn;
    RenderQueue queue{RenderQueue::Graphics};
    bool enabled{true};
    bool optional{false};
};

LightingStageDesc make_lighting_stage(std::string name, PostProcessStageKind kind);

struct LightingPipelineDesc {
    PbrLightingParameters pbr{};
    IblLightingParameters ibl{};
    LightBinningDesc binning{};
    std::vector<LightingStageDesc> stages;
};

struct LightingValidationResult {
    std::vector<std::string> errors;

    bool valid() const noexcept { return errors.empty(); }
};

LightingValidationResult validate_lighting_pipeline(const LightingPipelineDesc& description);
bool topologically_sort_lighting_stages(const std::vector<LightingStageDesc>& stages,
                                        std::vector<std::size_t>& order,
                                        std::string* error = nullptr);

struct LightingPipelinePlan {
    std::vector<std::size_t> stageOrder;
    LightBinningPlan binning;
    bool valid{false};
    std::string error;
};

class LightingPipeline {
    LightingPipelineDesc description_{};

public:
    const LightingPipelineDesc& description() const noexcept { return description_; }
    LightingPipelineDesc& description() noexcept { return description_; }

    std::size_t add_stage(LightingStageDesc stage);
    void clear_stages() noexcept { description_.stages.clear(); }

    LightingValidationResult validate() const;
    LightingPipelinePlan build_plan(const RenderView& view,
                                    const std::vector<LightingLight>& lights) const;
    LightingPipelinePlan build_plan(const RenderView& view,
                                    const std::vector<LightRenderItem>& lights) const;
};

} // namespace shinkou::render

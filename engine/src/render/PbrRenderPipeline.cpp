#include "shinkou/render/PbrRenderPipeline.h"
#include "shinkou/render/RenderBackend.h"
#include "shinkou/render/LightingPipeline.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace shinkou::render {
namespace {
bool finite(float value) noexcept { return std::isfinite(value) != 0; }

void add_error(RenderDescriptorValidationResult& result, const std::string& message) {
    result.errors.emplace_back(message);
}

void validate_texture_handle(RenderDescriptorValidationResult& result, ResourceHandle handle, const char* name) {
    if (handle && handle.kind != ResourceKind::Texture2D) {
        result.errors.emplace_back(std::string(name) + " must be a 2D texture resource");
    }
}

bool is_power_of_two(std::uint32_t value) noexcept {
    return value != 0 && (value & (value - 1u)) == 0;
}

void merge(RenderDescriptorValidationResult& target, const RenderDescriptorValidationResult& source) {
    target.errors.insert(target.errors.end(), source.errors.begin(), source.errors.end());
}
}

RenderDescriptorValidationResult validate_pbr_material(const PbrMaterialDesc& material) {
    RenderDescriptorValidationResult result;
    const auto non_negative = [&result](float value, const char* name) {
        if (!finite(value) || value < 0.0f) result.errors.emplace_back(std::string(name) + " must be finite and non-negative");
    };
    non_negative(material.baseColorFactor.x, "baseColorFactor.r");
    non_negative(material.baseColorFactor.y, "baseColorFactor.g");
    non_negative(material.baseColorFactor.z, "baseColorFactor.b");
    non_negative(material.baseColorFactor.w, "baseColorFactor.a");
    non_negative(material.emissiveFactor.x, "emissiveFactor.r");
    non_negative(material.emissiveFactor.y, "emissiveFactor.g");
    non_negative(material.emissiveFactor.z, "emissiveFactor.b");
    if (!finite(material.metallic) || material.metallic < 0.0f || material.metallic > 1.0f) add_error(result, "metallic must be in [0, 1]");
    if (!finite(material.roughness) || material.roughness < 0.0f || material.roughness > 1.0f) add_error(result, "roughness must be in [0, 1]");
    if (!finite(material.normalScale) || material.normalScale < 0.0f) add_error(result, "normalScale must be finite and non-negative");
    if (!finite(material.occlusionStrength) || material.occlusionStrength < 0.0f || material.occlusionStrength > 1.0f) add_error(result, "occlusionStrength must be in [0, 1]");
    if (!finite(material.emissiveStrength) || material.emissiveStrength < 0.0f) add_error(result, "emissiveStrength must be finite and non-negative");
    if (!finite(material.alphaCutoff) || material.alphaCutoff < 0.0f || material.alphaCutoff > 1.0f) add_error(result, "alphaCutoff must be in [0, 1]");
    validate_texture_handle(result, material.baseColorTexture, "baseColorTexture");
    validate_texture_handle(result, material.metallicRoughnessTexture, "metallicRoughnessTexture");
    validate_texture_handle(result, material.normalTexture, "normalTexture");
    validate_texture_handle(result, material.occlusionTexture, "occlusionTexture");
    validate_texture_handle(result, material.emissiveTexture, "emissiveTexture");
    validate_texture_handle(result, material.ibl.irradianceTexture, "irradianceTexture");
    validate_texture_handle(result, material.ibl.prefilteredEnvironment, "prefilteredEnvironment");
    validate_texture_handle(result, material.ibl.brdfLut, "brdfLut");
    const auto hasIbl = material.ibl.irradianceTexture || material.ibl.prefilteredEnvironment || material.ibl.brdfLut;
    const auto completeIbl = material.ibl.irradianceTexture && material.ibl.prefilteredEnvironment && material.ibl.brdfLut;
    if (hasIbl && !completeIbl) add_error(result, "IBL requires irradiance, prefiltered environment, and BRDF LUT textures together");
    if (!finite(material.ibl.intensity) || material.ibl.intensity < 0.0f) add_error(result, "IBL intensity must be finite and non-negative");
    if (!finite(material.ibl.rotation)) add_error(result, "IBL rotation must be finite");
    return result;
}

RenderDescriptorValidationResult validate_light_component(const LightComponent& light) {
    RenderDescriptorValidationResult result;
    const auto non_negative = [&result](float value, const char* name) {
        if (!finite(value) || value < 0.0f) result.errors.emplace_back(std::string(name) + " must be finite and non-negative");
    };
    non_negative(light.color.x, "light.color.r");
    non_negative(light.color.y, "light.color.g");
    non_negative(light.color.z, "light.color.b");
    non_negative(light.intensity, "light.intensity");
    if (light.type != LightType::Directional && (!finite(light.range) || light.range <= 0.0f)) add_error(result, "local light range must be greater than zero");
    if (light.type == LightType::Spot) {
        if (!finite(light.innerCone) || !finite(light.outerCone) || light.innerCone < 0.0f || light.outerCone > 1.0f || light.innerCone > light.outerCone) {
            add_error(result, "spot cone values must satisfy 0 <= innerCone <= outerCone <= 1");
        }
    }
    return result;
}

RenderDescriptorValidationResult validate_light_cluster_desc(const LightClusterDesc& description) {
    RenderDescriptorValidationResult result;
    if (description.viewportWidth == 0 || description.viewportHeight == 0) add_error(result, "cluster viewport dimensions must be non-zero");
    if (!is_power_of_two(description.tileSize) || description.tileSize > 512) add_error(result, "cluster tileSize must be a power of two in [1, 512]");
    if (description.depthSlices == 0 || description.depthSlices > 256) add_error(result, "cluster depthSlices must be in [1, 256]");
    if (description.maxLightsPerCluster == 0 || description.maxLightsPerCluster > 1024) add_error(result, "maxLightsPerCluster must be in [1, 1024]");
    if (!finite(description.nearPlane) || !finite(description.farPlane) || description.nearPlane <= 0.0f || description.farPlane <= description.nearPlane) add_error(result, "cluster depth range must satisfy 0 < nearPlane < farPlane");
    return result;
}

LightClusterGrid make_light_cluster_grid(const LightClusterDesc& description) {
    if (!validate_light_cluster_desc(description).valid()) return {};
    LightClusterGrid grid;
    grid.tilesX = (description.viewportWidth + description.tileSize - 1u) / description.tileSize;
    grid.tilesY = (description.viewportHeight + description.tileSize - 1u) / description.tileSize;
    grid.slices = description.depthSlices;
    grid.clusterCount = static_cast<std::uint64_t>(grid.tilesX) * grid.tilesY * grid.slices;
    return grid;
}

LightClusterBuildResult build_light_clusters(const LightClusterDesc& description,
    const RenderView& view, const std::vector<LightRenderItem>& lights) {
    LightClusterBuildResult result;
    result.validation = validate_light_cluster_desc(description);
    if (!result.validation.valid()) return result;
    result.grid = make_light_cluster_grid(description);
    LightBinningDesc binning;
    binning.viewportWidth = description.viewportWidth;
    binning.viewportHeight = description.viewportHeight;
    binning.tileSize = description.tileSize;
    binning.depthSlices = description.depthSlices;
    binning.maxLightsPerBin = description.maxLightsPerCluster;
    binning.nearPlane = description.nearPlane;
    binning.farPlane = description.farPlane;
    binning.logarithmicDepth = description.logarithmicDepth;
    binning.includeDirectionalLights = false;
    std::string binningError;
    const auto built = build_light_binning(view,make_lighting_lights(lights),binning,&binningError);
    if (!built.valid) { add_error(result.validation,"light binning failed: "+binningError); return result; }
    result.clusterLightIndices = built.lightIndices;
    result.clusterLightCounts.reserve(built.bins.size());
    for (const auto& bin : built.bins) result.clusterLightCounts.push_back(bin.count);
    result.overflowLightCount = built.droppedLightCount;
    std::vector<bool> included(lights.size(),false);
    for (const auto index : built.lightIndices) if (index < included.size()) included[index] = true;
    for (std::size_t index = 0; index < lights.size(); ++index) {
        const auto& light = lights[index].light;
        const auto validation = validate_light_component(light);
        merge(result.validation,validation);
        if (light.type == LightType::Directional && light.visible && validation.valid())
            result.directionalLightIndices.push_back(static_cast<std::uint32_t>(index));
        else {
            if (light.type == LightType::Point) ++result.pointLightCount;
            if (light.type == LightType::Spot) ++result.spotLightCount;
            if (!included[index]) ++result.rejectedLightCount;
        }
    }
    return result;
}

ClusteredLightingPlan ClusteredForwardScheduler::schedule(RenderGraph& graph, const RenderScene& scene,
    const ClusteredLightingDesc& description) {
    ClusteredLightingPlan plan;
    plan.build = build_light_clusters(description.grid, scene.view(), scene.lights());
    merge(plan.validation, plan.build.validation);
    if (!description.lightBuffer || description.lightBuffer.kind != ResourceKind::Buffer) add_error(plan.validation, "cluster scheduler requires a light buffer");
    if (description.lightBuffer && !graph.contains_resource(description.lightBuffer)) {
        if (description.lightBufferDescription.size == 0) add_error(plan.validation, "light buffer description is required when importing a light buffer");
        else graph.import_resource(description.lightBuffer, description.lightBufferDescription);
    }
    if (!description.cullingPipeline || description.cullingPipeline.kind != ResourceKind::Pipeline) add_error(plan.validation, "cluster scheduler requires a culling pipeline");
    if (description.cullingPipeline && !graph.contains_resource(description.cullingPipeline)) graph.import_resource(description.cullingPipeline, description.cullingPipelineDescription);
    if (description.depthTexture && description.depthTexture.kind != ResourceKind::Texture2D && description.depthTexture.kind != ResourceKind::DepthStencil) add_error(plan.validation, "cluster depth input must be a texture or depth-stencil resource");
    if (description.depthTexture && !graph.contains_resource(description.depthTexture)) graph.import_resource(description.depthTexture, description.depthDescription);
    if (!plan.validation.valid()) return plan;

    const auto clusterCount = static_cast<std::size_t>(plan.build.grid.clusterCount);
    constexpr std::size_t maxGpuIndexBytes=256u*1024u*1024u;
    if (clusterCount>maxGpuIndexBytes/sizeof(std::uint32_t)/description.grid.maxLightsPerCluster) {
        add_error(plan.validation,"cluster GPU index buffer exceeds the 256 MiB budget");return plan;
    }
    plan.clusterBuffer = graph.create_buffer({clusterCount * sizeof(std::uint32_t) * 2u, sizeof(std::uint32_t) * 2u,
        false, false, {}, false, true, true});
    plan.clusterIndexBuffer = graph.create_buffer({std::max<std::size_t>(sizeof(std::uint32_t),
        clusterCount * description.grid.maxLightsPerCluster * sizeof(std::uint32_t)), sizeof(std::uint32_t), false, false,
        {}, false, true, true});
    struct GridParameters {
        std::uint32_t tilesX;
        std::uint32_t tilesY;
        std::uint32_t slices;
        std::uint32_t maxLights;
        float nearPlane;
        float farPlane;
        std::uint32_t logarithmicDepth;
        std::uint32_t lightCount;
    } parameters{plan.build.grid.tilesX, plan.build.grid.tilesY, plan.build.grid.slices,
        description.grid.maxLightsPerCluster, description.grid.nearPlane, description.grid.farPlane,
        description.grid.logarithmicDepth ? 1u : 0u, static_cast<std::uint32_t>(scene.lights().size())};
    std::vector<std::uint8_t> parameterBytes(sizeof(parameters));
    std::memcpy(parameterBytes.data(), &parameters, sizeof(parameters));
    BufferDesc parameterDescription{sizeof(parameters), sizeof(float) * 4u, false, false,
        std::move(parameterBytes)};
    parameterDescription.uniformBuffer = true;
    plan.parameterBuffer = graph.create_buffer(parameterDescription);
    if (!plan.clusterBuffer || !plan.clusterIndexBuffer || !plan.parameterBuffer) {
        add_error(plan.validation, "cluster scheduler could not allocate graph resources");
        return plan;
    }

    std::vector<DescriptorBinding> bindings{
        {"lights", description.lightBuffer, DescriptorType::StructuredBuffer, description.lightSlot, 0, 1, false, {}},
        {"clusters", plan.clusterBuffer, DescriptorType::StorageBuffer, description.clusterSlot, 0, 1, false, {}},
        {"clusterLightIndices", plan.clusterIndexBuffer, DescriptorType::StorageBuffer, description.indexSlot, 0, 1, false, {}},
        {"clusterParameters", plan.parameterBuffer, DescriptorType::UniformBuffer, description.parameterSlot, 0, 1, false, {}}
    };
    if (description.depthTexture) bindings.push_back({"depth", description.depthTexture, DescriptorType::Texture, description.depthSlot, 0, 1, false, {}});
    const auto material = graph.create_material({"clustered_light_culling_material", description.cullingPipeline, std::move(bindings), false});
    if (!material) {
        add_error(plan.validation, "cluster scheduler could not create its graph material");
        return plan;
    }
    std::vector<ResourceAccess> accesses{
        {description.lightBuffer, ResourceUsage::ShaderRead},
        {material, ResourceUsage::ShaderRead},
        {plan.clusterBuffer, ResourceUsage::StorageWrite},
        {plan.clusterIndexBuffer, ResourceUsage::StorageWrite},
        {plan.parameterBuffer, ResourceUsage::UniformBuffer}
    };
    if (description.depthTexture) accesses.push_back({description.depthTexture, ResourceUsage::ShaderRead});
    const auto dispatchCount = static_cast<std::uint32_t>((plan.build.grid.clusterCount + 63u) / 64u);
    plan.passIndex = graph.add_pass("clustered_light_culling", std::move(accesses),
        [material, parameters = plan.parameterBuffer, slot = description.parameterSlot, dispatchCount](auto& backend, const auto&) {
            backend.bind_material(material);
            backend.bind_uniform_buffer(parameters, slot, 0);
            backend.dispatch({std::max(1u, dispatchCount), 1, 1});
        }, RenderQueue::Compute);
    plan.scheduled = true;
    return plan;
}
}

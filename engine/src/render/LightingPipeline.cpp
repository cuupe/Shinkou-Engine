#include "shinkou/render/LightingPipeline.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace shinkou::render {
namespace {

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

bool finite_vec3(math::Vec3 value) {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

bool valid_handle(ResourceHandle handle, ResourceKind kind) {
    return !handle || handle.kind == kind;
}

bool valid_queue(RenderQueue queue) {
    switch (queue) {
    case RenderQueue::Graphics:
    case RenderQueue::Compute:
    case RenderQueue::Copy:
        return true;
    }
    return false;
}

bool valid_stage_kind(PostProcessStageKind kind) {
    switch (kind) {
    case PostProcessStageKind::Custom:
    case PostProcessStageKind::Exposure:
    case PostProcessStageKind::Bloom:
    case PostProcessStageKind::SSAO:
    case PostProcessStageKind::TAA:
    case PostProcessStageKind::SSR:
    case PostProcessStageKind::ToneMap:
        return true;
    }
    return false;
}

bool finite_non_negative(float value) {
    return math::IsFinite(value) && value >= 0.0f;
}

bool valid_texture_description(const TextureDesc& description) {
    return description.width != 0 && description.height != 0 &&
        description.layers != 0 && description.mipLevels != 0;
}

void append_error(LightingValidationResult& result, const std::string& message) {
    result.errors.push_back(message);
}

bool validate_stage_parameters(const PostProcessStageDesc& stage, std::string& error) {
    if (!math::IsFinite(stage.exposure) || !math::IsFinite(stage.blendFactor) || stage.blendFactor < 0.0f) {
        error = "post-process stage has invalid exposure or blend factor";
        return false;
    }
    if (!valid_handle(stage.pipeline, ResourceKind::Pipeline) ||
        !valid_handle(stage.sampler, ResourceKind::Sampler) ||
        !valid_handle(stage.historyTexture, ResourceKind::Texture2D) ||
        !valid_handle(stage.historyOutput, ResourceKind::Texture2D) ||
        !valid_handle(stage.auxiliaryTexture, ResourceKind::Texture2D)) {
        error = "post-process stage has a resource with the wrong kind";
        return false;
    }
    if ((stage.historyTexture && !valid_texture_description(stage.historyDescription)) ||
        (stage.historyOutput && !valid_texture_description(stage.historyOutputDescription)) ||
        (stage.auxiliaryTexture && !valid_texture_description(stage.auxiliaryDescription)) ||
        !valid_texture_description(stage.output)) {
        error = "post-process stage has an invalid texture description";
        return false;
    }

    const auto valid_variant = [&error](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ExposureStageParameters>) {
            if (!math::IsFinite(value.exposure) || !finite_non_negative(value.adaptationRate)) {
                error = "exposure parameters must be finite and non-negative where applicable";
                return false;
            }
        } else if constexpr (std::is_same_v<T, BloomStageParameters>) {
            if (!finite_non_negative(value.threshold) || value.knee < 0.0f || value.knee > 1.0f ||
                !finite_non_negative(value.intensity) || value.mipLevels == 0) {
                error = "bloom parameters are outside their supported ranges";
                return false;
            }
        } else if constexpr (std::is_same_v<T, SsaoStageParameters>) {
            if (!(value.radius > 0.0f) || !math::IsFinite(value.radius) ||
                !finite_non_negative(value.power) || !finite_non_negative(value.bias) || value.sampleCount == 0) {
                error = "SSAO parameters are outside their supported ranges";
                return false;
            }
        } else if constexpr (std::is_same_v<T, TaaStageParameters>) {
            if (value.feedback < 0.0f || value.feedback > 1.0f || !math::IsFinite(value.feedback) ||
                !finite_non_negative(value.sharpness) || value.motionRejection < 0.0f ||
                value.motionRejection > 1.0f || !math::IsFinite(value.motionRejection)) {
                error = "TAA parameters are outside their supported ranges";
                return false;
            }
        } else if constexpr (std::is_same_v<T, SsrStageParameters>) {
            if (!(value.maxDistance > 0.0f) || !math::IsFinite(value.maxDistance) ||
                !finite_non_negative(value.thickness) || !(value.stride > 0.0f) ||
                !math::IsFinite(value.stride) || value.maxSteps == 0) {
                error = "SSR parameters are outside their supported ranges";
                return false;
            }
        }
        return true;
    };
    return std::visit(valid_variant, stage.parameters);
}

bool validate_light(const LightingLight& light, std::string& error) {
    if (!finite_vec3(light.position) || !finite_vec3(light.direction) || !finite_vec3(light.color) ||
        !finite_non_negative(light.intensity) || !finite_non_negative(light.range) ||
        !math::IsFinite(light.innerCone) || !math::IsFinite(light.outerCone) ||
        light.innerCone < 0.0f || light.innerCone > 1.0f || light.outerCone < 0.0f ||
        light.outerCone > 1.0f || light.innerCone > light.outerCone) {
        error = "light contains a non-finite value or an invalid range";
        return false;
    }
    if (light.type != LightType::Directional && !(light.range > 0.0f)) {
        error = "point and spot lights require a positive range";
        return false;
    }
    if ((light.type == LightType::Directional || light.type == LightType::Spot) &&
        math::LengthSquared(light.direction) <= math::Epsilon) {
        error = "directional and spot lights require a non-zero direction";
        return false;
    }
    return true;
}

bool validate_binning(const LightBinningDesc& description, std::string& error) {
    if (description.viewportWidth == 0 || description.viewportHeight == 0 || description.tileSize == 0 ||
        description.maxLightsPerBin == 0 || description.depthSlices == 0 ||
        !(description.nearPlane > 0.0f) || !math::IsFinite(description.nearPlane) ||
        !(description.farPlane > description.nearPlane) || !math::IsFinite(description.farPlane)) {
        error = "light binning requires positive viewport, tile, capacity, and depth ranges";
        return false;
    }
    return true;
}

std::uint32_t clamp_u32(int value, std::uint32_t maximum) {
    if (maximum == 0) return 0;
    return static_cast<std::uint32_t>(std::clamp(value, 0, static_cast<int>(maximum - 1u)));
}

struct ScreenBounds {
    std::uint32_t minX{0};
    std::uint32_t maxX{0};
    std::uint32_t minY{0};
    std::uint32_t maxY{0};
    float minDepth{0.0f};
    float maxDepth{0.0f};
    bool visible{false};
};

ScreenBounds screen_bounds(const RenderView& view, const LightingLight& light,
                           const LightBinningDesc& description) {
    ScreenBounds result;
    const auto cameraPosition = math::TransformPoint(view.viewMatrix, light.position);
    const float radius = light.range;
    const float depth = cameraPosition.z;
    result.minDepth = std::max(description.nearPlane, depth - radius);
    result.maxDepth = std::min(description.farPlane, depth + radius);
    if (result.maxDepth < description.nearPlane || result.minDepth > description.farPlane) return result;

    const auto clip = math::TransformVector4(view.viewProjection,
        {light.position.x, light.position.y, light.position.z, 1.0f});
    if (!math::IsFinite(clip.x) || !math::IsFinite(clip.y) || !math::IsFinite(clip.w) ||
        std::abs(clip.w) <= math::Epsilon) return result;
    const float centerX = clip.x / clip.w;
    const float centerY = clip.y / clip.w;
    const auto radiusClipX = math::TransformVector4(view.viewProjection,
        {light.position.x + radius, light.position.y, light.position.z, 1.0f});
    const auto radiusClipY = math::TransformVector4(view.viewProjection,
        {light.position.x, light.position.y + radius, light.position.z, 1.0f});
    float extentX = 1.0f;
    float extentY = 1.0f;
    if (std::abs(radiusClipX.w) > math::Epsilon && math::IsFinite(radiusClipX.x)) {
        extentX = std::abs(radiusClipX.x / radiusClipX.w - centerX);
    }
    if (std::abs(radiusClipY.w) > math::Epsilon && math::IsFinite(radiusClipY.y)) {
        extentY = std::abs(radiusClipY.y / radiusClipY.w - centerY);
    }
    if (cameraPosition.z <= 0.0f) {
        extentX = 1.0f;
        extentY = 1.0f;
    }
    const float minX = std::clamp(centerX - extentX, -1.0f, 1.0f);
    const float maxX = std::clamp(centerX + extentX, -1.0f, 1.0f);
    const float minY = std::clamp(centerY - extentY, -1.0f, 1.0f);
    const float maxY = std::clamp(centerY + extentY, -1.0f, 1.0f);
    result.minX = clamp_u32(static_cast<int>(std::floor((minX * 0.5f + 0.5f) * description.viewportWidth)), description.viewportWidth);
    result.maxX = clamp_u32(static_cast<int>(std::ceil((maxX * 0.5f + 0.5f) * description.viewportWidth)), description.viewportWidth);
    result.minY = clamp_u32(static_cast<int>(std::floor((minY * 0.5f + 0.5f) * description.viewportHeight)), description.viewportHeight);
    result.maxY = clamp_u32(static_cast<int>(std::ceil((maxY * 0.5f + 0.5f) * description.viewportHeight)), description.viewportHeight);
    result.visible = result.minX <= result.maxX && result.minY <= result.maxY;
    return result;
}

std::uint32_t depth_slice(float depth, const LightBinningDesc& description, std::uint32_t sliceCount) {
    if (sliceCount <= 1) return 0;
    const float logRange = std::log(description.farPlane / description.nearPlane);
    if (!(logRange > 0.0f) || !math::IsFinite(logRange)) return 0;
    const float t = std::clamp(std::log(std::max(description.nearPlane, depth) / description.nearPlane) / logRange,
                               0.0f, 0.99999994f);
    return std::min(sliceCount - 1u, static_cast<std::uint32_t>(t * static_cast<float>(sliceCount)));
}

} // namespace

std::vector<LightingLight> make_lighting_lights(const std::vector<LightRenderItem>& lights) {
    std::vector<LightingLight> result;
    result.reserve(lights.size());
    for (const auto& item : lights) {
        LightingLight light;
        light.entity = item.entity;
        light.type = item.light.type;
        light.position = item.transform.position;
        light.direction = math::Normalize(math::TransformVector(math::TransformMatrix(item.transform), {0.0f, 0.0f, 1.0f}));
        light.color = item.light.color;
        light.intensity = item.light.intensity;
        light.range = item.light.range;
        light.innerCone = item.light.innerCone;
        light.outerCone = item.light.outerCone;
        light.castsShadow = item.light.castsShadow;
        light.enabled = item.light.visible;
        result.push_back(light);
    }
    return result;
}

std::size_t LightBinningPlan::bin_index(std::uint32_t tileX, std::uint32_t tileY,
                                        std::uint32_t slice) const noexcept {
    if (tileX >= tileCountX || tileY >= tileCountY || slice >= sliceCount) return static_cast<std::size_t>(-1);
    return (static_cast<std::size_t>(slice) * tileCountY + tileY) * tileCountX + tileX;
}

const LightBinRange* LightBinningPlan::range(std::uint32_t tileX, std::uint32_t tileY,
                                             std::uint32_t slice) const noexcept {
    const auto index = bin_index(tileX, tileY, slice);
    return index < bins.size() ? &bins[index] : nullptr;
}

LightBinningPlan build_light_binning(const RenderView& view,
                                     const std::vector<LightingLight>& lights,
                                     const LightBinningDesc& description,
                                     std::string* error) {
    LightBinningPlan result;
    result.description = description;
    std::string validationError;
    if (!validate_binning(description, validationError)) {
        fail(error, validationError);
        return result;
    }
    result.tileCountX = (description.viewportWidth + description.tileSize - 1u) / description.tileSize;
    result.tileCountY = (description.viewportHeight + description.tileSize - 1u) / description.tileSize;
    result.sliceCount = description.mode == LightBinningMode::Clusters ? description.depthSlices : 1u;
    const auto binCount = static_cast<std::size_t>(result.tileCountX) * result.tileCountY * result.sliceCount;
    result.bins.resize(binCount);
    std::vector<std::vector<std::uint32_t>> perBin(binCount);
    for (auto& bin : perBin) bin.reserve(description.maxLightsPerBin);

    for (std::size_t lightIndex = 0; lightIndex < lights.size(); ++lightIndex) {
        const auto& light = lights[lightIndex];
        std::string lightError;
        if (!light.enabled) continue;
        if (!validate_light(light, lightError)) {
            ++result.invalidLightCount;
            continue;
        }
        if (light.type == LightType::Directional) {
            ++result.directionalLightCount;
            for (auto& bin : perBin) {
                if (bin.size() < description.maxLightsPerBin) bin.push_back(static_cast<std::uint32_t>(lightIndex));
                else ++result.droppedLightCount;
            }
            continue;
        }
        const auto bounds = screen_bounds(view, light, description);
        if (!bounds.visible) continue;
        const auto minTileX = bounds.minX / description.tileSize;
        const auto maxTileX = std::min(result.tileCountX - 1u, bounds.maxX / description.tileSize);
        const auto minTileY = bounds.minY / description.tileSize;
        const auto maxTileY = std::min(result.tileCountY - 1u, bounds.maxY / description.tileSize);
        const auto minSlice = depth_slice(bounds.minDepth, description, result.sliceCount);
        const auto maxSlice = depth_slice(bounds.maxDepth, description, result.sliceCount);
        for (std::uint32_t slice = minSlice; slice <= maxSlice; ++slice) {
            for (std::uint32_t tileY = minTileY; tileY <= maxTileY; ++tileY) {
                for (std::uint32_t tileX = minTileX; tileX <= maxTileX; ++tileX) {
                    auto& bin = perBin[result.bin_index(tileX, tileY, slice)];
                    if (bin.size() < description.maxLightsPerBin) bin.push_back(static_cast<std::uint32_t>(lightIndex));
                    else ++result.droppedLightCount;
                }
            }
        }
    }

    std::size_t totalIndices = 0;
    for (const auto& bin : perBin) totalIndices += bin.size();
    result.lightIndices.reserve(totalIndices);
    for (std::size_t index = 0; index < perBin.size(); ++index) {
        result.bins[index].offset = static_cast<std::uint32_t>(result.lightIndices.size());
        result.bins[index].count = static_cast<std::uint32_t>(perBin[index].size());
        result.lightIndices.insert(result.lightIndices.end(), perBin[index].begin(), perBin[index].end());
    }
    result.valid = true;
    return result;
}

LightingStageDesc make_lighting_stage(std::string name, PostProcessStageKind kind) {
    LightingStageDesc result;
    result.postProcess.name = std::move(name);
    result.postProcess.kind = kind;
    switch (kind) {
    case PostProcessStageKind::Exposure:
        result.postProcess.parameters = ExposureStageParameters{};
        break;
    case PostProcessStageKind::Bloom:
        result.postProcess.parameters = BloomStageParameters{};
        break;
    case PostProcessStageKind::SSAO:
        result.postProcess.parameters = SsaoStageParameters{};
        break;
    case PostProcessStageKind::TAA:
        result.postProcess.parameters = TaaStageParameters{};
        break;
    case PostProcessStageKind::SSR:
        result.postProcess.parameters = SsrStageParameters{};
        break;
    case PostProcessStageKind::Custom:
    case PostProcessStageKind::ToneMap:
        break;
    }
    return result;
}

bool topologically_sort_lighting_stages(const std::vector<LightingStageDesc>& stages,
                                        std::vector<std::size_t>& order,
                                        std::string* error) {
    order.clear();
    std::vector<std::string> names;
    names.reserve(stages.size());
    for (std::size_t index = 0; index < stages.size(); ++index) {
        const auto& stage = stages[index];
        if (!stage.enabled) continue;
        if (stage.postProcess.name.empty()) return fail(error, "lighting stage has an empty name");
        if (std::find(names.begin(), names.end(), stage.postProcess.name) != names.end()) {
            return fail(error, "lighting stages must have unique names");
        }
        names.push_back(stage.postProcess.name);
    }
    std::vector<std::size_t> indegree(stages.size(), 0);
    std::vector<std::vector<std::size_t>> outgoing(stages.size());
    for (std::size_t index = 0; index < stages.size(); ++index) {
        if (!stages[index].enabled) continue;
        for (const auto& dependencyName : stages[index].dependsOn) {
            const auto dependency = std::find_if(stages.begin(), stages.end(), [&](const auto& candidate) {
                return candidate.enabled && candidate.postProcess.name == dependencyName;
            });
            if (dependency == stages.end()) return fail(error, "lighting stage depends on an unknown or disabled stage");
            if (dependency->postProcess.name == stages[index].postProcess.name) return fail(error, "lighting stage cannot depend on itself");
            const auto dependencyIndex = static_cast<std::size_t>(std::distance(stages.begin(), dependency));
            outgoing[dependencyIndex].push_back(index);
            ++indegree[index];
        }
    }
    for (;;) {
        std::size_t next = static_cast<std::size_t>(-1);
        for (std::size_t index = 0; index < stages.size(); ++index) {
            if (stages[index].enabled && indegree[index] == 0 &&
                std::find(order.begin(), order.end(), index) == order.end()) {
                next = index;
                break;
            }
        }
        if (next == static_cast<std::size_t>(-1)) break;
        order.push_back(next);
        for (const auto dependent : outgoing[next]) --indegree[dependent];
    }
    std::size_t enabledCount = 0;
    for (const auto& stage : stages) if (stage.enabled) ++enabledCount;
    if (order.size() != enabledCount) return fail(error, "lighting stage dependency graph contains a cycle");
    return true;
}

LightingValidationResult validate_lighting_pipeline(const LightingPipelineDesc& description) {
    LightingValidationResult result;
    const auto non_negative_vec3 = [](math::Vec3 value) {
        return finite_vec3(value) && value.x >= 0.0f && value.y >= 0.0f && value.z >= 0.0f;
    };
    if (!non_negative_vec3(description.pbr.baseColor) || !finite_non_negative(description.pbr.metallic) ||
        description.pbr.metallic > 1.0f || !finite_non_negative(description.pbr.roughness) ||
        description.pbr.roughness > 1.0f || !finite_non_negative(description.pbr.normalScale) ||
        !finite_non_negative(description.pbr.occlusionStrength) || description.pbr.occlusionStrength > 1.0f ||
        !non_negative_vec3(description.pbr.emissiveColor) || !finite_non_negative(description.pbr.emissiveStrength) ||
        !finite_non_negative(description.pbr.clearCoat) || description.pbr.clearCoat > 1.0f ||
        !finite_non_negative(description.pbr.clearCoatRoughness) || description.pbr.clearCoatRoughness > 1.0f) {
        append_error(result, "PBR parameters are outside their supported ranges");
    }
    if (!valid_handle(description.ibl.environmentMap, ResourceKind::Texture2D) ||
        !valid_handle(description.ibl.irradianceMap, ResourceKind::Texture2D) ||
        !valid_handle(description.ibl.prefilteredMap, ResourceKind::Texture2D) ||
        !valid_handle(description.ibl.brdfLut, ResourceKind::Texture2D) ||
        !valid_handle(description.ibl.sampler, ResourceKind::Sampler) ||
        !finite_non_negative(description.ibl.diffuseIntensity) || !finite_non_negative(description.ibl.specularIntensity) ||
        !finite_non_negative(description.ibl.maxSpecularLod) || !math::IsFinite(description.ibl.environmentRotation)) {
        append_error(result, "IBL parameters or resources are invalid");
    }
    std::string error;
    if (!validate_binning(description.binning, error)) append_error(result, error);
    for (std::size_t index = 0; index < description.stages.size(); ++index) {
        const auto& stage = description.stages[index];
        if (!stage.enabled) continue;
        if (!valid_queue(stage.queue)) append_error(result, "lighting stage has an invalid render queue");
        if (!valid_stage_kind(stage.postProcess.kind)) append_error(result, "lighting stage has an invalid kind");
        if (stage.postProcess.name.empty()) append_error(result, "lighting stage has an empty name");
        if (!validate_stage_parameters(stage.postProcess, error)) append_error(result, error);
        for (const auto& dependency : stage.dependsOn) {
            if (dependency.empty()) append_error(result, "lighting stage has an empty dependency name");
        }
    }
    std::vector<std::size_t> order;
    if (!topologically_sort_lighting_stages(description.stages, order, &error)) append_error(result, error);
    return result;
}

std::size_t LightingPipeline::add_stage(LightingStageDesc stage) {
    description_.stages.push_back(std::move(stage));
    return description_.stages.size() - 1;
}

LightingValidationResult LightingPipeline::validate() const {
    return validate_lighting_pipeline(description_);
}

LightingPipelinePlan LightingPipeline::build_plan(const RenderView& view,
                                                   const std::vector<LightingLight>& lights) const {
    LightingPipelinePlan plan;
    const auto validation = validate();
    if (!validation.valid()) {
        plan.error = validation.errors.front();
        return plan;
    }
    if (!topologically_sort_lighting_stages(description_.stages, plan.stageOrder, &plan.error)) return plan;
    plan.binning = build_light_binning(view, lights, description_.binning, &plan.error);
    plan.valid = plan.binning.valid;
    return plan;
}

LightingPipelinePlan LightingPipeline::build_plan(const RenderView& view,
                                                   const std::vector<LightRenderItem>& lights) const {
    return build_plan(view, make_lighting_lights(lights));
}

} // namespace shinkou::render

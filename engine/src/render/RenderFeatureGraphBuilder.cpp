#include "shinkou/render/RenderFeatureGraphBuilder.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

namespace shinkou::render {
namespace {

constexpr std::size_t InvalidPass = static_cast<std::size_t>(-1);

std::vector<RenderFeatureRequest> default_feature_requests() {
    return {
        {RenderFeature::DepthPrepass, false, true},
        {RenderFeature::CascadedShadows, false, true},
        {RenderFeature::ScreenSpaceAmbientOcclusion, false, true},
        {RenderFeature::ScreenSpaceReflections, false, true},
        {RenderFeature::TemporalAntiAliasing, false, true},
        {RenderFeature::Bloom, false, true},
        {RenderFeature::Exposure, false, true}
    };
}

bool enabled(const RenderFeaturePlan& plan, RenderFeature feature) {
    const auto* decision = plan.decision(feature);
    return decision && decision->enabled;
}

std::string skipped_feature_reason(const RenderFeaturePlan& plan, RenderFeature feature) {
    const auto* decision = plan.decision(feature);
    if (!decision) return "feature was not requested";
    if (decision->fallback) return "fallback: " + decision->reason;
    return decision->reason;
}

bool writes_resource(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::ShaderWrite:
    case ResourceUsage::ColorAttachment:
    case ResourceUsage::ColorAttachment1:
    case ResourceUsage::ColorAttachment2:
    case ResourceUsage::ColorAttachment3:
    case ResourceUsage::ColorAttachment4:
    case ResourceUsage::ColorAttachment5:
    case ResourceUsage::ColorAttachment6:
    case ResourceUsage::ColorAttachment7:
    case ResourceUsage::DepthStencil:
    case ResourceUsage::CopyDestination:
    case ResourceUsage::StorageWrite:
    case ResourceUsage::Present:
        return true;
    default:
        return false;
    }
}

void append_unique(std::vector<std::string>& values, std::string_view value) {
    if (value.empty() || std::find(values.begin(), values.end(), value) != values.end()) return;
    values.emplace_back(value);
}

bool valid_texture(const TextureDesc& description) {
    return description.width != 0 && description.height != 0 &&
        description.layers != 0 && description.mipLevels != 0;
}

bool valid_shadow_settings(const ShadowSettings& settings, std::string& error) {
    if (!settings.enabled) return true;
    if (settings.atlasSize == 0 || settings.cascadeCount == 0 || settings.cascadeCount > 8 ||
        !(settings.maxDistance > 0.0f) || !math::IsFinite(settings.maxDistance) ||
        !math::IsFinite(settings.bias) || settings.bias < 0.0f) {
        error = "shadow settings require a valid atlas, cascade count, distance, and bias";
        return false;
    }
    return true;
}

TextureDesc color_description(std::uint32_t width, std::uint32_t height, bool hdr,
                              bool storage, std::uint32_t mipLevels = 1) {
    TextureDesc description;
    description.width = width;
    description.height = height;
    description.layers = 1;
    description.mipLevels = std::max(1u, mipLevels);
    description.format = hdr ? "rgba16f" : "rgba8";
    description.renderTarget = true;
    description.hdr = hdr;
    description.colorSpace = "linear";
    description.storage = storage;
    return description;
}

PostProcessStageDesc default_post_process_stage(const std::string& name, PostProcessStageKind kind) {
    PostProcessStageDesc stage;
    stage.name = name;
    stage.kind = kind;
    stage.useParameters = true;
    switch (kind) {
    case PostProcessStageKind::Exposure:
        stage.parameters = ExposureStageParameters{};
        break;
    case PostProcessStageKind::Bloom:
        stage.parameters = BloomStageParameters{};
        break;
    case PostProcessStageKind::SSAO:
        stage.parameters = SsaoStageParameters{};
        break;
    case PostProcessStageKind::TAA:
        stage.parameters = TaaStageParameters{};
        break;
    case PostProcessStageKind::SSR:
        stage.parameters = SsrStageParameters{};
        break;
    case PostProcessStageKind::Custom:
    case PostProcessStageKind::ToneMap:
        break;
    }
    return stage;
}

PostProcessStageDesc resolve_post_process_stage(const RenderFeatureGraphDesc& description,
                                                const std::string& name, PostProcessStageKind kind,
                                                const TextureDesc& output, bool compute) {
    auto stage = default_post_process_stage(name, kind);
    const auto found = std::find_if(description.postProcessStages.begin(), description.postProcessStages.end(),
        [kind](const auto& candidate) { return candidate.kind == kind; });
    if (found != description.postProcessStages.end()) stage = *found;
    stage.name = name;
    stage.output = output;
    stage.compute = compute;
    return stage;
}

} // namespace

const char* to_string(RenderFeatureGraphStage stage) noexcept {
    switch (stage) {
    case RenderFeatureGraphStage::DepthPrepass: return "depth_prepass";
    case RenderFeatureGraphStage::ShadowCascades: return "shadow_cascades";
    case RenderFeatureGraphStage::ClusterCulling: return "cluster_culling";
    case RenderFeatureGraphStage::Opaque: return "opaque";
    case RenderFeatureGraphStage::SSAO: return "ssao";
    case RenderFeatureGraphStage::SSR: return "ssr";
    case RenderFeatureGraphStage::TAA: return "taa";
    case RenderFeatureGraphStage::Bloom: return "bloom";
    case RenderFeatureGraphStage::Exposure: return "exposure";
    case RenderFeatureGraphStage::ToneMap: return "tone_map";
    case RenderFeatureGraphStage::Present: return "present";
    }
    return "unknown";
}

RenderFeatureGraphPlan RenderFeatureGraphBuilder::build(RenderGraph& graph,
                                                        const RenderFeatureGraphDesc& description) {
    RenderFeatureGraphPlan plan;
    if (description.width == 0 || description.height == 0 || description.maxLights == 0) {
        plan.error = "render feature graph requires positive dimensions and light capacity";
        return plan;
    }
    std::string error;
    if (!valid_shadow_settings(description.shadows, error)) {
        plan.error = error;
        return plan;
    }
    if (!valid_texture(description.depthDescription) || !valid_texture(description.presentDescription)) {
        plan.error = "render feature graph target descriptions are incomplete";
        return plan;
    }
    if (description.depthTarget && description.depthTarget.kind != ResourceKind::DepthStencil) {
        plan.error = "depth target must be a depth-stencil resource";
        return plan;
    }
    if (description.presentTarget && description.presentTarget.kind != ResourceKind::Texture2D) {
        plan.error = "present target must be a 2D texture resource";
        return plan;
    }

    auto requests = description.featureRequests;
    if (requests.empty()) requests = default_feature_requests();
    plan.featurePlan = build_render_feature_plan(description.capabilities, requests);
    if (!plan.featurePlan.valid) {
        plan.error = plan.featurePlan.error;
        return plan;
    }

    LightingPipeline lighting;
    lighting.description() = description.lighting;
    lighting.description().binning.viewportWidth = description.width;
    lighting.description().binning.viewportHeight = description.height;
    plan.lightingPlan = lighting.build_plan(RenderView{}, std::vector<LightingLight>{});
    if (!plan.lightingPlan.valid) {
        plan.error = "lighting pipeline plan is invalid: " + plan.lightingPlan.error;
        return plan;
    }

    auto add_resource = [&](const std::string& name, ResourceHandle handle,
                            const ResourceDesc& resourceDescription, bool external) {
        if (!handle) {
            plan.error = "failed to create render feature graph resource: " + name;
            return ResourceHandle{};
        }
        plan.resources.push_back({name, handle, resourceDescription, external});
        return handle;
    };
    auto create_texture = [&](const std::string& name, TextureDesc resourceDescription) {
        return add_resource(name, graph.create_texture(resourceDescription), resourceDescription, false);
    };
    auto create_depth = [&](const std::string& name, TextureDesc resourceDescription) {
        return add_resource(name, graph.create_depth_stencil(resourceDescription), resourceDescription, false);
    };
    auto create_buffer = [&](const std::string& name, BufferDesc resourceDescription) {
        return add_resource(name, graph.create_buffer(resourceDescription), resourceDescription, false);
    };
    auto import_resource = [&](const std::string& name, ResourceHandle handle, const ResourceDesc& resourceDescription) {
        graph.import_resource(handle, resourceDescription);
        if (!graph.contains_resource(handle)) {
            plan.error = "failed to import render feature graph resource: " + name;
            return ResourceHandle{};
        }
        return add_resource(name, handle, resourceDescription, true);
    };

    auto depthDescription = description.depthDescription;
    depthDescription.width = description.width;
    depthDescription.height = description.height;
    const auto depth = description.depthTarget
        ? import_resource("depth", description.depthTarget, depthDescription)
        : create_depth("depth", depthDescription);
    auto presentDescription = description.presentDescription;
    presentDescription.width = description.width;
    presentDescription.height = description.height;
    presentDescription.renderTarget = true;
    const auto present = description.presentTarget
        ? import_resource("present", description.presentTarget, presentDescription)
        : create_texture("present", presentDescription);
    const auto hdr = create_texture("hdr_color", color_description(description.width, description.height, true, true));
    if (!depth || !present || !hdr) return plan;

    ResourceHandle shadowAtlas{};
    ResourceHandle lightBuffer{};
    ResourceHandle clusterBuffer{};
    ResourceHandle clusterIndexBuffer{};
    ResourceHandle aoTexture{};
    ResourceHandle ssrTexture{};
    ResourceHandle taaHistory{};
    ResourceHandle taaTexture{};
    ResourceHandle bloomTexture{};
    ResourceHandle exposureTexture{};

    const bool depthPrepass = enabled(plan.featurePlan, RenderFeature::DepthPrepass);
    const bool shadows = enabled(plan.featurePlan, RenderFeature::CascadedShadows) && description.shadows.enabled;
    const bool requestedClusters = description.lighting.binning.mode == LightBinningMode::Clusters;
    bool clusterCulling = requestedClusters && plan.lightingPlan.valid;
    bool clusterFallback = false;
    if (clusterCulling && !description.capabilities.supportsCompute) {
        clusterFallback = true;
    }

    if (shadows) {
        auto shadowDescription = description.presentDescription;
        shadowDescription.width = description.shadows.atlasSize;
        shadowDescription.height = description.shadows.atlasSize;
        shadowDescription.format = "d32f";
        shadowDescription.renderTarget = false;
        shadowDescription.storage = false;
        shadowAtlas = create_depth("shadow_atlas", shadowDescription);
        if (!shadowAtlas) return plan;
    }
    if (clusterCulling) {
        const auto binCount = std::max<std::size_t>(1, plan.lightingPlan.binning.bins.size());
        const auto maxLights = std::max<std::size_t>(1, description.lighting.binning.maxLightsPerBin);
        lightBuffer = create_buffer("light_buffer", {description.maxLights * 64u, 64u, false, false, {}, false, true, false});
        clusterBuffer = create_buffer("cluster_buffer", {binCount * sizeof(std::uint32_t) * 2u,
            sizeof(std::uint32_t) * 2u, false, false, {}, false, true, true});
        clusterIndexBuffer = create_buffer("cluster_light_indices", {binCount * maxLights * sizeof(std::uint32_t),
            sizeof(std::uint32_t), false, false, {}, false, true, true});
        if (!lightBuffer || !clusterBuffer || !clusterIndexBuffer) return plan;
    }

    auto contract_for = [&](const ResourceAccess& access) {
        RenderFeatureGraphResourceContract contract;
        contract.handle = access.resource;
        contract.usage = access.usage;
        const auto found = std::find_if(plan.resources.begin(), plan.resources.end(),
            [&access](const auto& resource) { return resource.handle.id == access.resource.id; });
        if (found != plan.resources.end()) {
            contract.name = found->name;
            contract.description = found->description;
        }
        return contract;
    };
    auto add_pass = [&](RenderFeatureGraphStage stage, std::string name,
                        std::vector<ResourceAccess> accesses, RenderQueue queue,
                        bool fallback, std::string reason, PostProcessStageDesc postProcess = {},
                        std::vector<std::string> dependsOn = {}) {
        RenderFeatureGraphPass node;
        node.stage = stage;
        node.name = std::move(name);
        node.queue = queue;
        node.accesses = accesses;
        node.postProcess = std::move(postProcess);
        node.enabled = true;
        node.fallback = fallback;
        node.reason = std::move(reason);
        node.passIndex = graph.add_pass(node.name, std::move(accesses), [](auto&, const auto&) {}, queue);
        node.description.stage = node.stage;
        node.description.name = node.name;
        node.description.queue = node.queue;
        node.description.dependsOn = std::move(dependsOn);
        node.description.enabled = node.enabled;
        node.description.fallback = node.fallback;
        node.description.reason = node.reason;
        for (const auto& access : node.accesses) {
            auto contract = contract_for(access);
            (writes_resource(access.usage) ? node.description.outputs : node.description.inputs)
                .push_back(std::move(contract));
        }
        if (node.fallback) plan.diagnostics.push_back(node.name + ": fallback selected; " + node.reason);
        plan.passes.push_back(std::move(node));
    };
    auto add_skipped = [&](RenderFeatureGraphStage stage, const char* name,
                           std::string reason, bool fallback = false,
                           std::vector<std::string> dependsOn = {}) {
        RenderFeatureGraphPass node;
        node.stage = stage;
        node.name = name;
        node.enabled = false;
        node.fallback = fallback;
        node.reason = std::move(reason);
        node.description.stage = node.stage;
        node.description.name = node.name;
        node.description.dependsOn = std::move(dependsOn);
        node.description.enabled = node.enabled;
        node.description.fallback = node.fallback;
        node.description.reason = node.reason;
        if (node.fallback) plan.diagnostics.push_back(node.name + ": fallback selected; " + node.reason);
        plan.passes.push_back(std::move(node));
    };

    if (depthPrepass) {
        add_pass(RenderFeatureGraphStage::DepthPrepass, "depth_prepass", {{depth, ResourceUsage::DepthStencil}},
            RenderQueue::Graphics, false, "feature enabled");
    } else {
        add_skipped(RenderFeatureGraphStage::DepthPrepass, "depth_prepass",
            skipped_feature_reason(plan.featurePlan, RenderFeature::DepthPrepass),
            plan.featurePlan.decision(RenderFeature::DepthPrepass) &&
                plan.featurePlan.decision(RenderFeature::DepthPrepass)->fallback);
    }
    if (shadows) {
        add_pass(RenderFeatureGraphStage::ShadowCascades, "shadow_cascades", {{shadowAtlas, ResourceUsage::DepthStencil}},
            RenderQueue::Graphics, false, "feature enabled", {},
            depthPrepass ? std::vector<std::string>{"depth_prepass"} : std::vector<std::string>{});
    } else {
        add_skipped(RenderFeatureGraphStage::ShadowCascades, "shadow_cascades",
            description.shadows.enabled ? skipped_feature_reason(plan.featurePlan, RenderFeature::CascadedShadows) : "shadow settings disabled",
            description.shadows.enabled && plan.featurePlan.decision(RenderFeature::CascadedShadows) &&
                plan.featurePlan.decision(RenderFeature::CascadedShadows)->fallback);
    }
    if (clusterCulling) {
        std::vector<ResourceAccess> accesses{{lightBuffer, ResourceUsage::ShaderRead},
            {clusterBuffer, ResourceUsage::StorageWrite}, {clusterIndexBuffer, ResourceUsage::StorageWrite}};
        if (depthPrepass) accesses.push_back({depth, ResourceUsage::ShaderRead});
        add_pass(RenderFeatureGraphStage::ClusterCulling, "cluster_culling", std::move(accesses),
            clusterFallback ? RenderQueue::Graphics : RenderQueue::Compute, clusterFallback,
            clusterFallback ? "compute unavailable; graphics fallback selected" : "compute cluster culling", {},
            depthPrepass ? std::vector<std::string>{"depth_prepass"} : std::vector<std::string>{});
    } else {
        add_skipped(RenderFeatureGraphStage::ClusterCulling, "cluster_culling",
            requestedClusters ? "lighting cluster plan unavailable" : "tile lighting selected");
    }

    std::vector<ResourceAccess> opaque{{depth, depthPrepass ? ResourceUsage::ShaderRead : ResourceUsage::DepthStencil},
        {hdr, ResourceUsage::ColorAttachment}};
    if (shadowAtlas) opaque.push_back({shadowAtlas, ResourceUsage::ShaderRead});
    if (clusterCulling) {
        opaque.push_back({lightBuffer, ResourceUsage::ShaderRead});
        opaque.push_back({clusterBuffer, ResourceUsage::ShaderRead});
        opaque.push_back({clusterIndexBuffer, ResourceUsage::ShaderRead});
    }
    std::vector<std::string> opaqueDependencies;
    if (depthPrepass) opaqueDependencies.emplace_back("depth_prepass");
    if (shadows) opaqueDependencies.emplace_back("shadow_cascades");
    if (clusterCulling) opaqueDependencies.emplace_back("cluster_culling");
    add_pass(RenderFeatureGraphStage::Opaque, "opaque", std::move(opaque), RenderQueue::Graphics, false,
        depthPrepass ? "depth prepass provides read-only depth" : "opaque pass owns depth production", {},
        std::move(opaqueDependencies));

    const bool ssao = enabled(plan.featurePlan, RenderFeature::ScreenSpaceAmbientOcclusion);
    if (ssao) {
        aoTexture = create_texture("ssao", color_description(description.width, description.height, false, true));
        if (!aoTexture) return plan;
        const bool compute = description.capabilities.supportsCompute;
        auto stage = resolve_post_process_stage(description, "ssao", PostProcessStageKind::SSAO,
            color_description(description.width, description.height, false, true), compute);
        auto dependencies = std::vector<std::string>{"opaque"};
        for (const auto& dependency : stage.dependsOn) append_unique(dependencies, dependency);
        add_pass(RenderFeatureGraphStage::SSAO, "ssao", {{depth, ResourceUsage::ShaderRead},
            {aoTexture, compute ? ResourceUsage::StorageWrite : ResourceUsage::ColorAttachment}},
            compute ? RenderQueue::Compute : RenderQueue::Graphics, !compute,
            compute ? "compute SSAO" : "compute unavailable; graphics SSAO fallback", std::move(stage),
            std::move(dependencies));
    } else {
        add_skipped(RenderFeatureGraphStage::SSAO, "ssao",
            skipped_feature_reason(plan.featurePlan, RenderFeature::ScreenSpaceAmbientOcclusion),
            plan.featurePlan.decision(RenderFeature::ScreenSpaceAmbientOcclusion) &&
                plan.featurePlan.decision(RenderFeature::ScreenSpaceAmbientOcclusion)->fallback);
    }

    ResourceHandle currentColor = hdr;
    std::string colorProducer = "opaque";
    const bool ssr = enabled(plan.featurePlan, RenderFeature::ScreenSpaceReflections);
    if (ssr) {
        ssrTexture = create_texture("ssr", color_description(description.width, description.height, true, true));
        if (!ssrTexture) return plan;
        const bool compute = description.capabilities.supportsCompute;
        std::vector<ResourceAccess> accesses{{currentColor, ResourceUsage::ShaderRead},
            {depth, ResourceUsage::ShaderRead},
            {ssrTexture, compute ? ResourceUsage::StorageWrite : ResourceUsage::ColorAttachment}};
        if (aoTexture) accesses.push_back({aoTexture, ResourceUsage::ShaderRead});
        auto stage = resolve_post_process_stage(description, "ssr", PostProcessStageKind::SSR,
            color_description(description.width, description.height, true, true), compute);
        auto dependencies = std::vector<std::string>{colorProducer};
        if (aoTexture) append_unique(dependencies, "ssao");
        for (const auto& dependency : stage.dependsOn) append_unique(dependencies, dependency);
        add_pass(RenderFeatureGraphStage::SSR, "ssr", std::move(accesses),
            compute ? RenderQueue::Compute : RenderQueue::Graphics, !compute,
            compute ? "compute SSR" : "compute unavailable; graphics SSR fallback", std::move(stage),
            std::move(dependencies));
        currentColor = ssrTexture;
        colorProducer = "ssr";
    } else {
        add_skipped(RenderFeatureGraphStage::SSR, "ssr",
            skipped_feature_reason(plan.featurePlan, RenderFeature::ScreenSpaceReflections),
            plan.featurePlan.decision(RenderFeature::ScreenSpaceReflections) &&
                plan.featurePlan.decision(RenderFeature::ScreenSpaceReflections)->fallback);
    }

    const bool taa = enabled(plan.featurePlan, RenderFeature::TemporalAntiAliasing);
    if (taa) {
        taaHistory = create_texture("taa_history", color_description(description.width, description.height, true, true));
        taaTexture = create_texture("taa", color_description(description.width, description.height, true, true));
        if (!taaHistory || !taaTexture) return plan;
        const bool compute = description.capabilities.supportsCompute;
        auto stage = resolve_post_process_stage(description, "taa", PostProcessStageKind::TAA,
            color_description(description.width, description.height, true, true), compute);
        auto dependencies = std::vector<std::string>{colorProducer};
        for (const auto& dependency : stage.dependsOn) append_unique(dependencies, dependency);
        add_pass(RenderFeatureGraphStage::TAA, "taa", {{currentColor, ResourceUsage::ShaderRead},
            {taaHistory, ResourceUsage::ShaderRead},
            {taaTexture, compute ? ResourceUsage::StorageWrite : ResourceUsage::ColorAttachment}},
            compute ? RenderQueue::Compute : RenderQueue::Graphics, !compute,
            compute ? "compute TAA" : "compute unavailable; graphics TAA fallback", std::move(stage),
            std::move(dependencies));
        currentColor = taaTexture;
        colorProducer = "taa";
    } else {
        add_skipped(RenderFeatureGraphStage::TAA, "taa",
            skipped_feature_reason(plan.featurePlan, RenderFeature::TemporalAntiAliasing),
            plan.featurePlan.decision(RenderFeature::TemporalAntiAliasing) &&
                plan.featurePlan.decision(RenderFeature::TemporalAntiAliasing)->fallback);
    }

    const bool bloom = enabled(plan.featurePlan, RenderFeature::Bloom);
    if (bloom) {
        const auto mipLevels = [&description] {
            const auto found = std::find_if(description.postProcessStages.begin(), description.postProcessStages.end(),
                [](const auto& stage) { return stage.kind == PostProcessStageKind::Bloom; });
            if (found == description.postProcessStages.end()) return 5u;
            if (const auto* parameters = std::get_if<BloomStageParameters>(&found->parameters)) return parameters->mipLevels;
            return 5u;
        }();
        bloomTexture = create_texture("bloom", color_description(description.width, description.height, true, true, mipLevels));
        if (!bloomTexture) return plan;
        const bool compute = description.capabilities.supportsCompute;
        auto stage = resolve_post_process_stage(description, "bloom", PostProcessStageKind::Bloom,
            color_description(description.width, description.height, true, true, mipLevels), compute);
        auto dependencies = std::vector<std::string>{colorProducer};
        for (const auto& dependency : stage.dependsOn) append_unique(dependencies, dependency);
        add_pass(RenderFeatureGraphStage::Bloom, "bloom", {{currentColor, ResourceUsage::ShaderRead},
            {bloomTexture, compute ? ResourceUsage::StorageWrite : ResourceUsage::ColorAttachment}},
            compute ? RenderQueue::Compute : RenderQueue::Graphics, !compute,
            compute ? "compute bloom" : "compute unavailable; graphics bloom fallback", std::move(stage),
            std::move(dependencies));
    } else {
        add_skipped(RenderFeatureGraphStage::Bloom, "bloom",
            skipped_feature_reason(plan.featurePlan, RenderFeature::Bloom),
            plan.featurePlan.decision(RenderFeature::Bloom) &&
                plan.featurePlan.decision(RenderFeature::Bloom)->fallback);
    }

    const bool exposure = enabled(plan.featurePlan, RenderFeature::Exposure);
    if (exposure) {
        exposureTexture = create_texture("exposure", color_description(1, 1, true, true));
        if (!exposureTexture) return plan;
        const bool compute = description.capabilities.supportsCompute;
        auto stage = resolve_post_process_stage(description, "exposure", PostProcessStageKind::Exposure,
            color_description(1, 1, true, true), compute);
        auto dependencies = std::vector<std::string>{colorProducer};
        for (const auto& dependency : stage.dependsOn) append_unique(dependencies, dependency);
        add_pass(RenderFeatureGraphStage::Exposure, "exposure", {{currentColor, ResourceUsage::ShaderRead},
            {exposureTexture, compute ? ResourceUsage::StorageWrite : ResourceUsage::ColorAttachment}},
            compute ? RenderQueue::Compute : RenderQueue::Graphics, !compute,
            compute ? "compute exposure" : "compute unavailable; graphics exposure fallback", std::move(stage),
            std::move(dependencies));
    } else {
        add_skipped(RenderFeatureGraphStage::Exposure, "exposure",
            skipped_feature_reason(plan.featurePlan, RenderFeature::Exposure),
            plan.featurePlan.decision(RenderFeature::Exposure) &&
                plan.featurePlan.decision(RenderFeature::Exposure)->fallback);
    }

    const auto toneStage = resolve_post_process_stage(description, "tone_map", PostProcessStageKind::ToneMap,
        color_description(description.width, description.height, false, false), false);
    const auto tone = create_texture("tone_mapped", color_description(description.width, description.height, false, false));
    if (!tone) return plan;
    std::vector<ResourceAccess> toneAccesses{{currentColor, ResourceUsage::ShaderRead},
        {tone, ResourceUsage::ColorAttachment}};
    if (bloomTexture) toneAccesses.push_back({bloomTexture, ResourceUsage::ShaderRead});
    if (exposureTexture) toneAccesses.push_back({exposureTexture, ResourceUsage::ShaderRead});
    add_pass(RenderFeatureGraphStage::ToneMap, "tone_map", std::move(toneAccesses), RenderQueue::Graphics, false,
        "tone mapping is the required HDR-to-display boundary", toneStage,
        [&] {
            std::vector<std::string> dependencies{colorProducer};
            if (bloomTexture) dependencies.emplace_back("bloom");
            if (exposureTexture) dependencies.emplace_back("exposure");
            for (const auto& dependency : toneStage.dependsOn) append_unique(dependencies, dependency);
            return dependencies;
        }());

    add_pass(RenderFeatureGraphStage::Present, "present", {{tone, ResourceUsage::ShaderRead},
        {present, ResourceUsage::Present}}, RenderQueue::Graphics, false, "terminal presentation side effect", {},
        {"tone_map"});
    plan.valid = true;
    return plan;
}

} // namespace shinkou::render

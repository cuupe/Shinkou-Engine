#include "shinkou/render/RenderFeatureGraphBuilder.h"

#include <cassert>
#include <string>

using namespace shinkou::render;

namespace {
const RenderFeatureGraphPass* find_pass(const RenderFeatureGraphPlan& plan, RenderFeatureGraphStage stage) {
    for (const auto& pass : plan.passes) if (pass.stage == stage) return &pass;
    return nullptr;
}
}

int main() {
    RenderFeatureGraphDesc description;
    description.capabilities.deviceReady = true;
    description.capabilities.supportsCompute = true;
    description.width = 320;
    description.height = 180;
    description.shadows.atlasSize = 512;
    description.lighting.binning.viewportWidth = description.width;
    description.lighting.binning.viewportHeight = description.height;

    RenderGraph graph;
    const auto plan = RenderFeatureGraphBuilder::build(graph, description);
    assert(plan.valid);
    assert(plan.featurePlan.valid);
    assert(graph.passes().size() == 11);
    assert(plan.passes.size() == 11);
    assert(find_pass(plan, RenderFeatureGraphStage::DepthPrepass)->enabled);
    assert(find_pass(plan, RenderFeatureGraphStage::ClusterCulling)->enabled);
    assert(find_pass(plan, RenderFeatureGraphStage::ClusterCulling)->queue == RenderQueue::Compute);
    assert(find_pass(plan, RenderFeatureGraphStage::Present)->enabled);
    const auto* opaque = find_pass(plan, RenderFeatureGraphStage::Opaque);
    assert(opaque && opaque->description.outputs.size() == 1);
    assert(opaque->description.outputs.front().name == "hdr_color");
    assert(find_pass(plan, RenderFeatureGraphStage::SSAO)->description.dependsOn.front() == "opaque");
    assert(find_pass(plan, RenderFeatureGraphStage::Present)->description.dependsOn.front() == "tone_map");
    std::string error;
    assert(graph.compile(&error));
    assert(graph.execution_order().size() == graph.passes().size());

    RenderFeatureGraphDesc fallback = description;
    fallback.capabilities.supportsCompute = false;
    RenderGraph fallbackGraph;
    const auto fallbackPlan = RenderFeatureGraphBuilder::build(fallbackGraph, fallback);
    assert(fallbackPlan.valid);
    const auto* cluster = find_pass(fallbackPlan, RenderFeatureGraphStage::ClusterCulling);
    assert(cluster && cluster->enabled && cluster->fallback && cluster->queue == RenderQueue::Graphics);
    assert(cluster->reason.find("compute unavailable") != std::string::npos);
    assert(!fallbackPlan.diagnostics.empty());
    assert(fallbackGraph.compile(&error));

    RenderFeatureGraphDesc skipped = description;
    skipped.featureRequests = {
        {RenderFeature::DepthPrepass, false, true},
        {RenderFeature::CascadedShadows, false, false},
        {RenderFeature::ScreenSpaceAmbientOcclusion, false, false},
        {RenderFeature::ScreenSpaceReflections, false, false},
        {RenderFeature::TemporalAntiAliasing, false, false},
        {RenderFeature::Bloom, false, false},
        {RenderFeature::Exposure, false, false}
    };
    RenderGraph skippedGraph;
    const auto skippedPlan = RenderFeatureGraphBuilder::build(skippedGraph, skipped);
    assert(skippedPlan.valid);
    assert(!find_pass(skippedPlan, RenderFeatureGraphStage::ShadowCascades)->enabled);
    assert(!find_pass(skippedPlan, RenderFeatureGraphStage::SSAO)->enabled);
    assert(!find_pass(skippedPlan, RenderFeatureGraphStage::SSR)->enabled);
    assert(!find_pass(skippedPlan, RenderFeatureGraphStage::Bloom)->enabled);
    assert(skippedGraph.compile(&error));
    // Depth prepass and clustered lighting remain enabled; the optional
    // post/deferred features are retained in the plan for diagnostics but do
    // not materialize executable graph passes.
    assert(skippedGraph.passes().size() == 5);

    RenderFeatureGraphDesc invalid = description;
    invalid.depthTarget = {17, ResourceKind::Texture2D};
    RenderGraph invalidGraph;
    const auto invalidPlan = RenderFeatureGraphBuilder::build(invalidGraph, invalid);
    assert(!invalidPlan.valid && !invalidPlan.error.empty());
    assert(invalidGraph.passes().empty());

    RenderFeatureGraphDesc customStages = description;
    customStages.postProcessStages.push_back({"custom_ssao", {}, {}, {}, 0, 1, false, 1.0f, 1.0f, {}, {}, 2, 3,
        false, 4, 0, {}, {}, 5, {}, {}, 6, PostProcessStageKind::SSAO, {}, {"opaque"}});
    RenderGraph customGraph;
    const auto customPlan = RenderFeatureGraphBuilder::build(customGraph, customStages);
    assert(customPlan.valid);
    const auto* customSsao = find_pass(customPlan, RenderFeatureGraphStage::SSAO);
    assert(customSsao && customSsao->description.dependsOn.size() == 1 &&
        customSsao->description.dependsOn.front() == "opaque");
    return 0;
}

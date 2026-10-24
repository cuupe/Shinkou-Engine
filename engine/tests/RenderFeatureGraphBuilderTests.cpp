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
    PostProcessStageDesc temporal;
    temporal.kind = PostProcessStageKind::TAA;
    temporal.historyTexture = {70,ResourceKind::Texture2D};
    temporal.historyOutput = {71,ResourceKind::Texture2D};
    temporal.historyDescription.width = temporal.historyOutputDescription.width = description.width;
    temporal.historyDescription.height = temporal.historyOutputDescription.height = description.height;
    temporal.historyDescription.format = temporal.historyOutputDescription.format = "rgba16f";
    temporal.historyDescription.renderTarget = temporal.historyOutputDescription.renderTarget = true;
    temporal.historyDescription.storage = temporal.historyOutputDescription.storage = true;
    description.postProcessStages.push_back(temporal);

    std::size_t recorded = 0;
    for (unsigned i = 0; i <= static_cast<unsigned>(RenderFeatureGraphStage::Present); ++i) {
        description.executors.push_back({static_cast<RenderFeatureGraphStage>(i),
            [&](auto& backend, const auto& context) {
                ++recorded;
                if (context.queue == RenderQueue::Compute) assert(backend.dispatch({1,1,1}));
                else backend.draw_sprite({});
            }, {}});
    }
    RenderGraph graph;
    const auto plan = RenderFeatureGraphBuilder::build(graph, description);
    assert(plan.valid && plan.executable);
    assert(plan.featurePlan.valid);
    assert(graph.passes().size() == 11);
    assert(plan.passes.size() == 11);
    assert(find_pass(plan, RenderFeatureGraphStage::DepthPrepass)->enabled);
    assert(find_pass(plan, RenderFeatureGraphStage::ClusterCulling)->enabled);
    assert(find_pass(plan, RenderFeatureGraphStage::ClusterCulling)->queue == RenderQueue::Compute);
    assert(find_pass(plan, RenderFeatureGraphStage::Present)->enabled);
    const auto* opaque = find_pass(plan, RenderFeatureGraphStage::Opaque);
    assert(opaque && opaque->description.outputs.size() == 2);
    assert(opaque->description.outputs.back().name == "hdr_color");
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
    auto backend = create_backend(BackendApi::Null);
    assert(backend->initialize({}));
    assert(backend->create_resource(temporal.historyTexture,temporal.historyDescription));
    assert(backend->create_resource(temporal.historyOutput,temporal.historyOutputDescription));
    graph.execute(*backend, &error);
    assert(error.empty() && recorded == 11);
    assert(backend->stats().drawCalls > 0 && backend->stats().dispatchCalls > 0);

    auto missing = description;
    missing.executors.clear();
    RenderGraph untouched;
    const auto sentinel = untouched.create_buffer({16,4});
    const auto missingPlan = RenderFeatureGraphBuilder::build(untouched,missing);
    assert(!missingPlan.valid && !missingPlan.executable);
    assert(missingPlan.error.find("no executable implementation") != std::string::npos);
    assert(untouched.passes().empty() && untouched.resources().size() == 1 && untouched.contains_resource(sentinel));
    RenderGraph planning;
    const auto descriptive = RenderFeatureGraphBuilder::plan(planning,missing);
    assert(descriptive.valid && !descriptive.executable && planning.passes().empty());

    auto duplicate = description;
    duplicate.executors.push_back(duplicate.executors.front());
    assert(!RenderFeatureGraphBuilder::build(untouched,duplicate).valid);
    auto duplicateSettings=description;
    duplicateSettings.postProcessStages.push_back(duplicateSettings.postProcessStages.front());
    assert(!RenderFeatureGraphBuilder::build(untouched,duplicateSettings).valid);
    auto mismatchedSettings=description;
    mismatchedSettings.postProcessStages.front().parameters=BloomStageParameters{};
    assert(!RenderFeatureGraphBuilder::build(untouched,mismatchedSettings).valid);
    auto badDependency = description;
    PostProcessStageDesc invalidOrder;
    invalidOrder.kind = PostProcessStageKind::SSAO;
    invalidOrder.dependsOn = {"missing_effect"};
    badDependency.postProcessStages.push_back(invalidOrder);
    assert(!RenderFeatureGraphBuilder::build(untouched,badDependency).valid);
    assert(untouched.resources().size() == 1);
    return 0;
}

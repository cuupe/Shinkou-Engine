#include "shinkou/render/AdvancedRenderFeatures.h"

#include <cassert>
#include <algorithm>

using namespace shinkou::render;

int main() {
    RenderCapabilities capabilities;
    capabilities.deviceReady = true;
    capabilities.supportsRayTracing = false;
    capabilities.supportsMeshShaders = false;
    capabilities.supportsVariableRateShading = false;
    const auto plan = build_render_feature_plan(capabilities, {
        {RenderFeature::ScreenSpaceReflections, false, true},
        {RenderFeature::DepthPrepass, false, true},
        {RenderFeature::ScreenSpaceAmbientOcclusion, false, true},
        {RenderFeature::RayTracing, false, true}
    });
    assert(plan.valid);
    assert(plan.decision(RenderFeature::RayTracing)->fallback);
    assert(plan.executionOrder.size() == 3);
    const auto required = build_render_feature_plan(capabilities,
        {{RenderFeature::RayTracing, true, true}});
    assert(!required.valid);

    const auto autoDependencies = build_render_feature_plan(capabilities,
        {{RenderFeature::ScreenSpaceReflections, false, true}});
    assert(autoDependencies.valid);
    assert(autoDependencies.decision(RenderFeature::DepthPrepass)->enabled);
    assert(autoDependencies.decision(RenderFeature::ScreenSpaceAmbientOcclusion)->enabled);
    assert(autoDependencies.decision(RenderFeature::ScreenSpaceReflections)->enabled);
    const auto depthIt = std::find(autoDependencies.executionOrder.begin(), autoDependencies.executionOrder.end(),
        RenderFeature::DepthPrepass);
    const auto ssaoIt = std::find(autoDependencies.executionOrder.begin(), autoDependencies.executionOrder.end(),
        RenderFeature::ScreenSpaceAmbientOcclusion);
    const auto ssrIt = std::find(autoDependencies.executionOrder.begin(), autoDependencies.executionOrder.end(),
        RenderFeature::ScreenSpaceReflections);
    assert(depthIt < ssaoIt && ssaoIt < ssrIt);

    const auto disabledDependency = build_render_feature_plan(capabilities, {
        {RenderFeature::ScreenSpaceReflections, false, true},
        {RenderFeature::ScreenSpaceAmbientOcclusion, false, false}
    });
    assert(disabledDependency.valid);
    assert(!disabledDependency.decision(RenderFeature::ScreenSpaceReflections)->enabled);
    assert(disabledDependency.decision(RenderFeature::ScreenSpaceReflections)->fallback);

    const auto requiredDisabledDependency = build_render_feature_plan(capabilities, {
        {RenderFeature::ScreenSpaceReflections, true, true},
        {RenderFeature::ScreenSpaceAmbientOcclusion, false, false}
    });
    assert(!requiredDisabledDependency.valid);
    return 0;
}

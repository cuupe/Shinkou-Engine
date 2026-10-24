#include "shinkou/render/LightingPipeline.h"

#include <cassert>
#include <limits>

using namespace shinkou::render;

int main() {
    RenderView view;
    LightBinningDesc description;
    description.viewportWidth = 1280;
    description.viewportHeight = 720;
    description.tileSize = 16;
    const auto plan = build_light_binning(view, {}, description);
    assert(plan.valid);
    assert(plan.tileCountX == 80);
    assert(plan.tileCountY == 45);
    assert(plan.bins.size() == 80u * 45u * 24u);

    LightingPipeline pipeline;
    pipeline.description().binning = description;
    pipeline.add_stage(make_lighting_stage("tone_map", PostProcessStageKind::ToneMap));
    assert(pipeline.validate().valid());
    const auto frame = pipeline.build_plan(view, std::vector<LightRenderItem>{});
    assert(frame.valid);
    assert(frame.stageOrder.size() == 1);
    auto oversized=description;
    oversized.viewportWidth=oversized.viewportHeight=std::numeric_limits<std::uint32_t>::max();
    std::string error;
    assert(!build_light_binning(view,{},oversized,&error).valid && !error.empty());
    oversized=description;oversized.maxBinCount=1;
    assert(!build_light_binning(view,{},oversized,&error).valid);
    oversized=description;oversized.maxLightIndexCount=1;
    assert(!build_light_binning(view,{LightingLight{}},oversized,&error).valid);
    auto stage=make_lighting_stage("ao",PostProcessStageKind::SSAO).postProcess;
    stage.parameters=BloomStageParameters{};
    assert(!validate_post_process_parameters(stage,error));
    stage.kind=PostProcessStageKind::Bloom;
    auto bloom=BloomStageParameters{};bloom.knee=std::numeric_limits<float>::quiet_NaN();stage.parameters=bloom;
    assert(!validate_post_process_parameters(stage,error));
    return 0;
}

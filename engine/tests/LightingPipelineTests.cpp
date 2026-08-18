#include "shinkou/render/LightingPipeline.h"

#include <cassert>

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
    return 0;
}

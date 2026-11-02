#pragma once

#include "shinkou/render/BuiltinRenderFeatures.h"
#include "shinkou/render/MotionVectorRenderer.h"
#include "shinkou/render/PbrRenderPipeline.h"

namespace shinkou::render {
struct StandardRenderPipelineDesc {
    std::uint32_t width{1280}, height{720};
    std::string presentationFormat{"bgra8"};
    ShadowSettings shadows{};
    bool ssao{true}, ssr{true}, taa{true}, bloom{true}, exposure{true};
    std::vector<PostProcessStageDesc> imageParameters;
    bool depthPrepass{true};
    bool motionVectors{true};
    bool temporalJitter{true};
    bool motionBlur{false};
    ExposureStageParameters exposureParameters{1.0f, 1.5f};
    std::vector<RenderPipelineStage> stages;
    std::vector<RenderFeatureGraphExecutor> executors;
    SkySettings sky;
};

// A complete forward/HDR/image/presentation path. Custom passes can instead
// compose the same libraries directly through RenderFeatureGraphBuilder.
class StandardRenderPipeline {
    StandardRenderPipelineDesc description_;
    PbrMaterialLibrary materials_;
    BuiltinRenderFeatures images_;
    TemporalRenderHistory history_;
    TemporalRenderHistory exposureHistory_;
    ForwardRenderer forward_;
    MotionVectorRenderer motion_;
    SkyRenderer sky_;
    ResourceHandle color_{}, depth_{}, shadow_{}, motionVectors_{};
    TextureDesc colorDescription_{}, depthDescription_{}, shadowDescription_{},
        motionDescription_{};
    math::Mat4 previousViewProjection_{math::Mat4::Identity()};
    bool pending_{false}, initialized_{false};
    math::Mat4 pendingViewProjection_{math::Mat4::Identity()};
    PreviousObjectTransforms previousObjects_, pendingObjects_;
    std::uint32_t jitterFrame_{0};
    RenderFeatureGraphPlan plan_;
    std::string error_;

  public:
    StandardRenderPipeline() = default;
    StandardRenderPipeline(const StandardRenderPipeline &) = delete;
    StandardRenderPipeline &operator=(const StandardRenderPipeline &) = delete;
    bool initialize(Renderer &renderer, const StandardRenderPipelineDesc &description = {});
    bool resize(Renderer &renderer, std::uint32_t width, std::uint32_t height);
    ResourceHandle create_material(Renderer &renderer, const PbrMaterialDesc &material);
    bool build(Renderer &renderer, const RenderScene &scene, ResourceHandle presentationTarget = {},
               float deltaSeconds = 1.0f / 60.0f);
    // Call once after submitting the built graph. Failed frames never enter
    // temporal feedback. A camera cut can be signaled by invalidate_history().
    void complete_frame(bool submitted);
    void invalidate_history();
    bool set_stages(std::vector<RenderPipelineStage> stages,
                    std::vector<RenderFeatureGraphExecutor> executors = {});
    bool set_sky(const SkySettings &settings);
    void release(Renderer &renderer);
    ResourceHandle scene_color() const noexcept {
        return color_;
    }
    ResourceHandle scene_depth() const noexcept {
        return depth_;
    }
    ResourceHandle motion_vectors() const noexcept {
        return motionVectors_;
    }
    const RenderFeatureGraphPlan &last_plan() const noexcept {
        return plan_;
    }
    const std::string &last_error() const noexcept {
        return error_;
    }
};
} // namespace shinkou::render

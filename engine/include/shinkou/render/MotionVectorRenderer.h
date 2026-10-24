#pragma once

#include "shinkou/render/RenderScene.h"
#include "shinkou/render/RenderGraph.h"

namespace shinkou::render {
using PreviousObjectTransforms=std::unordered_map<Entity,math::Mat4,EntityHash>;

// Rigid-mesh motion in normalized top-left UV coordinates: previous-current.
// History is supplied by the caller and committed only after frame success.
class MotionVectorRenderer {
    ResourceHandle vertex_{},fragment_{},pipeline_{};
    ResourceHandle backgroundVertex_{},backgroundFragment_{},backgroundPipeline_{},sampler_{};
    std::string error_;
public:
    bool initialize(Renderer& renderer);
    bool record(Renderer& renderer,RenderGraph& graph,const RenderScene& scene,
                const RenderView& currentView,const math::Mat4& previousViewProjection,
                const PreviousObjectTransforms& previousObjects,bool historyValid,
                ResourceHandle target,ResourceHandle depth);
    void release(Renderer& renderer);
    const std::string& last_error() const noexcept { return error_; }
};
}

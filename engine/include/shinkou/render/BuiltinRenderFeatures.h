#pragma once
#include "shinkou/render/RenderFeatureGraphBuilder.h"
#include "shinkou/render/Renderer.h"
#include "shinkou/render/BuiltinShaders.h"
#include <array>
#include <string_view>

namespace shinkou::render {

// Renderer-owned native programs for the standard image-processing stages.
// Scene geometry and clustered culling retain their explicit stage executors.
class BuiltinRenderFeatures {
    std::vector<ResourceHandle> owned_;
    std::array<ResourceHandle,8> pipelines_{};
    std::array<PipelineDesc,8> pipelineDescriptions_{};
    std::array<ResourceHandle,6> expandedPipelines_{};
    std::array<PipelineDesc,6> expandedDescriptions_{};
    std::array<ResourceHandle,2> histogramPipelines_{};
    std::array<PipelineDesc,2> histogramDescriptions_{};
    ResourceHandle sampler_{},black_{},white_{};
    SamplerDesc samplerDescription_{};
    TextureDesc blackDescription_{},whiteDescription_{};
    std::string error_;
public:
    bool initialize(Renderer& renderer,std::string presentationFormat="rgba8");
    void release(Renderer& renderer);
    bool install(RenderFeatureGraphDesc& description) const;
    const std::string& last_error() const noexcept { return error_; }
};

// Persistent ping-pong history. Advance only after a successful submission.
// Resize and invalidate reset feedback, so no uninitialized history is read.
class TemporalRenderHistory {
    std::array<ResourceHandle,2> textures_{};
    TextureDesc description_{};
    std::uint32_t readIndex_{0};
    bool valid_{false};
public:
    bool resize(Renderer& renderer,std::uint32_t width,std::uint32_t height);
    void release(Renderer& renderer);
    void invalidate() noexcept { valid_ = false; }
    void advance() noexcept { if (textures_[0] && textures_[1]) { readIndex_ ^= 1; valid_ = true; } }
    bool valid() const noexcept { return valid_; }
    ResourceHandle input() const noexcept { return textures_[readIndex_]; }
    ResourceHandle output() const noexcept { return textures_[readIndex_^1]; }
    const TextureDesc& description() const noexcept { return description_; }
    void configure(PostProcessStageDesc& stage,PostProcessStageKind kind=PostProcessStageKind::TAA) const;
};
}

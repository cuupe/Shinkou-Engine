#pragma once

#include "shinkou/render/RenderGraph.h"
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace shinkou::render {

enum class PostProcessStageKind {
    Custom,
    Exposure,
    Bloom,
    SSAO,
    TAA,
    SSR,
    ToneMap
};

struct ExposureStageParameters {
    float exposure{1.0f};
    float adaptationRate{0.0f};
};

struct BloomStageParameters {
    float threshold{1.0f};
    float knee{0.5f};
    float intensity{0.8f};
    std::uint32_t mipLevels{5};
};

struct SsaoStageParameters {
    float radius{0.5f};
    float power{1.0f};
    float bias{0.025f};
    std::uint32_t sampleCount{16};
};

struct TaaStageParameters {
    float feedback{0.9f};
    float sharpness{0.0f};
    float motionRejection{1.0f};
};

struct SsrStageParameters {
    float maxDistance{50.0f};
    float thickness{0.1f};
    float stride{1.0f};
    std::uint32_t maxSteps{64};
};

using PostProcessStageParameters = std::variant<std::monostate, ExposureStageParameters,
    BloomStageParameters, SsaoStageParameters, TaaStageParameters, SsrStageParameters>;

struct PostProcessValidationResult {
    std::vector<std::string> errors;

    bool valid() const noexcept { return errors.empty(); }
};

struct PostProcessStageDesc {
    std::string name;
    ResourceHandle pipeline{};
    ResourceHandle sampler{};
    TextureDesc output{};
    std::uint32_t textureSlot{0};
    std::uint32_t samplerSlot{1};
    bool compute{false};
    float exposure{1.0f};
    float blendFactor{1.0f};
    ResourceHandle historyTexture{};
    TextureDesc historyDescription{};
    std::uint32_t historySlot{2};
    std::uint32_t outputSlot{3};
    bool useParameters{false};
    std::uint32_t parameterSlot{4};
    std::uint32_t parameterSpace{0};
    // Optional persistent history destination. Graphics stages expose it as
    // MRT attachment 1; compute stages expose it as a second storage image.
    ResourceHandle historyOutput{};
    TextureDesc historyOutputDescription{};
    std::uint32_t historyOutputSlot{5};
    // Optional auxiliary input used by depth/normal driven stages such as
    // SSAO and SSR. It remains a plain graph resource handle so backends do
    // not leak into the stage description.
    ResourceHandle auxiliaryTexture{};
    TextureDesc auxiliaryDescription{};
    std::uint32_t auxiliarySlot{6};
    PostProcessStageKind kind{PostProcessStageKind::Custom};
    PostProcessStageParameters parameters{};
    // Optional ordering constraints within a post-process chain.  The chain
    // remains explicitly ordered; dependencies may only name an earlier
    // stage, so they cannot silently change the image-processing order.
    std::vector<std::string> dependsOn;
};

struct PostProcessChainDesc {
    std::string name;
    ResourceHandle source{};
    TextureDesc sourceDescription{};
    ResourceHandle finalTarget{};
    TextureDesc finalDescription{};
    std::vector<PostProcessStageDesc> stages;
};

class PostProcessPipeline {
    std::vector<PostProcessStageDesc> stages_;

public:
    std::size_t add_stage(PostProcessStageDesc stage);
    void clear() noexcept { stages_.clear(); }
    std::size_t stage_count() const noexcept { return stages_.size(); }
    const std::vector<PostProcessStageDesc>& stages() const noexcept { return stages_; }

    ResourceHandle build(RenderGraph& graph,
                          ResourceHandle source,
                          const TextureDesc& sourceDescription,
                          ResourceHandle finalTarget = {},
                          const TextureDesc& finalDescription = {} ) const;
    ResourceHandle build(RenderGraph& graph, const PostProcessChainDesc& chain) const;
};

PostProcessValidationResult validate_post_process_stage(const PostProcessStageDesc& stage);
PostProcessValidationResult validate_post_process_chain(const PostProcessChainDesc& chain);
}

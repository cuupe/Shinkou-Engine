#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace shinkou::render {
enum class CompareOp { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class StencilOp {
    Keep,
    Zero,
    Replace,
    IncrementClamp,
    DecrementClamp,
    Invert,
    IncrementWrap,
    DecrementWrap
};
enum class BlendOp { Add, Subtract, ReverseSubtract, Min, Max };
enum class BlendFactor {
    Zero,
    One,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    ConstantColor,
    OneMinusConstantColor,
    ConstantAlpha,
    OneMinusConstantAlpha,
    SrcAlphaSaturate,
    Src1Color,
    OneMinusSrc1Color,
    Src1Alpha,
    OneMinusSrc1Alpha
};
enum class VertexFormat {
    Float,
    Float2,
    Float3,
    Float4,
    UInt,
    UInt2,
    UInt3,
    UInt4,
    Int,
    Int2,
    Int3,
    Int4,
    UNorm8x4,
    SNorm8x4,
    UInt8x4,
    UNorm16x2,
    UNorm16x4,
    Half2,
    Half4
};

struct VertexBindingDesc {
    std::uint32_t binding{0};
    std::uint32_t stride{0};
    bool perInstance{false};
    std::uint32_t stepRate{1};
};
struct VertexAttributeDesc {
    std::string semantic{"POSITION"};
    std::uint32_t semanticIndex{0};
    std::uint32_t location{0};
    std::uint32_t binding{0};
    VertexFormat format{VertexFormat::Float3};
    std::uint32_t offset{0};
};
struct VertexLayout {
    std::vector<VertexBindingDesc> bindings;
    std::vector<VertexAttributeDesc> attributes;
};
struct StencilFaceState {
    StencilOp fail{StencilOp::Keep};
    StencilOp depthFail{StencilOp::Keep};
    StencilOp pass{StencilOp::Keep};
    CompareOp compare{CompareOp::Always};
};
struct DepthStencilState {
    bool depthTest{true};
    bool depthWrite{true};
    CompareOp depthCompare{CompareOp::LessEqual};
    bool stencilTest{false};
    std::uint8_t stencilReadMask{0xff};
    std::uint8_t stencilWriteMask{0xff};
    std::uint32_t stencilReference{0};
    StencilFaceState front{};
    StencilFaceState back{};
};
struct BlendAttachmentState {
    bool enabled{false};
    BlendFactor srcColor{BlendFactor::SrcAlpha};
    BlendFactor dstColor{BlendFactor::OneMinusSrcAlpha};
    BlendOp colorOp{BlendOp::Add};
    BlendFactor srcAlpha{BlendFactor::One};
    BlendFactor dstAlpha{BlendFactor::Zero};
    BlendOp alphaOp{BlendOp::Add};
    std::uint8_t writeMask{0xf};
};
struct RasterizerState {
    bool frontCounterClockwise{false};
    bool depthClip{true};
    std::int32_t depthBias{0};
    float depthBiasClamp{0.0f};
    float slopeScaledDepthBias{0.0f};
};

struct PipelineDesc;
std::uint32_t vertex_format_size(VertexFormat format) noexcept;
VertexLayout resolve_vertex_layout(const PipelineDesc &description);
DepthStencilState resolve_depth_stencil(const PipelineDesc &description);
std::vector<BlendAttachmentState> resolve_blend_attachments(const PipelineDesc &description);
bool validate_pipeline_state(const PipelineDesc &description, std::string &error);
// Shared by logical pipeline caches and native PSO caches.
std::string pipeline_state_key(const PipelineDesc &description);
} // namespace shinkou::render

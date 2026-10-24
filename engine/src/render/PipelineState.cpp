#include "shinkou/render/RenderTypes.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <unordered_set>

namespace shinkou::render {
std::uint32_t vertex_format_size(VertexFormat format) noexcept {
    constexpr std::uint32_t sizes[]{4,8,12,16,4,8,12,16,4,8,12,16,4,4,4,4,8,4,8};
    const auto index = static_cast<std::size_t>(format);
    return index < sizeof(sizes)/sizeof(sizes[0]) ? sizes[index] : 0;
}

VertexLayout resolve_vertex_layout(const PipelineDesc& description) {
    if (!description.vertexLayout.bindings.empty() || !description.vertexLayout.attributes.empty())
        return description.vertexLayout;
    VertexLayout layout;
    if (!description.vertexInput) return layout;
    std::uint32_t stride = 12;
    layout.attributes.push_back({"POSITION",0,0,0,VertexFormat::Float3,0});
    if (description.vertexTextureCoordinates) {
        layout.attributes.push_back({"TEXCOORD",0,1,0,VertexFormat::Float2,stride});
        stride += 8;
    }
    if (description.vertexNormals) {
        layout.attributes.push_back({"NORMAL",0,static_cast<std::uint32_t>(layout.attributes.size()),
            0,VertexFormat::Float3,stride});
        stride += 12;
    }
    layout.bindings.push_back({0,stride,false,1});
    return layout;
}

DepthStencilState resolve_depth_stencil(const PipelineDesc& description) {
    auto state = description.depthStencilState.value_or(DepthStencilState{});
    if (!description.depthStencilState) {
        state.depthTest = description.depthTest;
        state.depthWrite = description.depthWrite;
    }
    if (description.depthFormat == "none") state.depthTest = state.depthWrite = false;
    return state;
}

std::vector<BlendAttachmentState> resolve_blend_attachments(const PipelineDesc& description) {
    if (!description.blendAttachments.empty()) return description.blendAttachments;
    BlendAttachmentState state;
    state.enabled = description.alphaBlend;
    return std::vector<BlendAttachmentState>(std::max<std::size_t>(1,description.colorFormats.size()), state);
}

bool validate_pipeline_state(const PipelineDesc& description, std::string& error) {
    const auto fail = [&](const char* message) { error = message; return false; };
    error.clear();
    const auto layout = resolve_vertex_layout(description);
    if (layout.bindings.size() > 16 || layout.attributes.size() > 16)
        return fail("vertex layout exceeds the portable 16 binding/attribute limit");
    if (layout.bindings.empty() != layout.attributes.empty())
        return fail("vertex bindings and attributes must both be present");
    std::unordered_set<std::uint32_t> slots, locations;
    std::unordered_set<std::string> semantics;
    for (const auto& binding : layout.bindings) {
        if (binding.binding >= 16 || !slots.insert(binding.binding).second ||
            binding.stride == 0 || binding.stride > 2048 || binding.stepRate != 1)
            return fail("vertex binding has an invalid slot, stride, duplicate slot or unsupported step rate");
    }
    for (const auto& attribute : layout.attributes) {
        const auto binding = std::find_if(layout.bindings.begin(),layout.bindings.end(),
            [&](const auto& value) { return value.binding == attribute.binding; });
        const auto size = vertex_format_size(attribute.format);
        if (binding == layout.bindings.end() || !size || attribute.offset > binding->stride ||
            size > binding->stride - attribute.offset || attribute.location >= 16 ||
            attribute.semantic.empty() || attribute.semantic.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789") != std::string::npos ||
            !locations.insert(attribute.location).second ||
            !semantics.insert(attribute.semantic+":"+std::to_string(attribute.semanticIndex)).second)
            return fail("vertex attribute has an invalid format, offset, binding, location or duplicate semantic");
    }
    const auto state = resolve_depth_stencil(description);
    const auto validCompare = [](CompareOp op) { return static_cast<unsigned>(op) <= static_cast<unsigned>(CompareOp::Always); };
    const auto validFace = [&](const StencilFaceState& face) {
        return validCompare(face.compare) && static_cast<unsigned>(face.fail) <= 7 &&
            static_cast<unsigned>(face.depthFail) <= 7 && static_cast<unsigned>(face.pass) <= 7;
    };
    if (!validCompare(state.depthCompare) || !validFace(state.front) || !validFace(state.back) ||
        state.stencilReference > 255 || (state.stencilTest && description.depthFormat != "d24s8"))
        return fail("invalid depth/stencil state or stencil format");
    const auto blends = resolve_blend_attachments(description);
    if (blends.size() != std::max<std::size_t>(1,description.colorFormats.size()) || blends.size() > 8)
        return fail("blend attachment count must match color attachments");
    for (const auto& blend : blends) {
        if (static_cast<unsigned>(blend.srcColor) > 18 || static_cast<unsigned>(blend.dstColor) > 18 ||
            static_cast<unsigned>(blend.srcAlpha) > 18 || static_cast<unsigned>(blend.dstAlpha) > 18 ||
            static_cast<unsigned>(blend.colorOp) > 4 || static_cast<unsigned>(blend.alphaOp) > 4 || blend.writeMask > 15)
            return fail("invalid blend attachment state");
        if (blend.srcColor==BlendFactor::ConstantAlpha || blend.srcColor==BlendFactor::OneMinusConstantAlpha ||
            blend.dstColor==BlendFactor::ConstantAlpha || blend.dstColor==BlendFactor::OneMinusConstantAlpha)
            return fail("constant-alpha RGB blending is outside the portable pipeline contract; use ConstantColor with equal components");
    }
    const auto& raster = description.rasterizerState;
    const auto& constants = description.blendConstants;
    if (!std::isfinite(raster.depthBiasClamp) || !std::isfinite(raster.slopeScaledDepthBias) ||
        !std::isfinite(constants.x) || !std::isfinite(constants.y) || !std::isfinite(constants.z) || !std::isfinite(constants.w))
        return fail("rasterizer and blend constants must be finite");
    return true;
}

std::string pipeline_state_key(const PipelineDesc& description) {
    std::ostringstream out;
    const auto append = [&](auto value) { out << static_cast<long long>(value) << ','; };
    const auto floating = [&](float value) { std::uint32_t bits; std::memcpy(&bits,&value,sizeof(bits)); append(bits); };
    const auto layout = resolve_vertex_layout(description);
    append(layout.bindings.size());
    for (const auto& b : layout.bindings) { append(b.binding); append(b.stride); append(b.perInstance); append(b.stepRate); }
    append(layout.attributes.size());
    for (const auto& a : layout.attributes) {
        out << a.semantic.size() << ':' << a.semantic << ',';
        append(a.semanticIndex); append(a.location); append(a.binding); append(a.format); append(a.offset);
    }
    const auto d = resolve_depth_stencil(description);
    append(d.depthTest); append(d.depthWrite); append(d.depthCompare); append(d.stencilTest);
    append(d.stencilReadMask); append(d.stencilWriteMask); append(d.stencilReference);
    for (const auto& f : {d.front,d.back}) { append(f.fail); append(f.depthFail); append(f.pass); append(f.compare); }
    for (const auto& b : resolve_blend_attachments(description)) {
        append(b.enabled); append(b.srcColor); append(b.dstColor); append(b.colorOp);
        append(b.srcAlpha); append(b.dstAlpha); append(b.alphaOp); append(b.writeMask);
    }
    const auto r = description.rasterizerState;
    append(r.frontCounterClockwise); append(r.depthClip); append(r.depthBias);
    floating(r.depthBiasClamp); floating(r.slopeScaledDepthBias);
    floating(description.blendConstants.x); floating(description.blendConstants.y);
    floating(description.blendConstants.z); floating(description.blendConstants.w);
    return out.str();
}
}

#pragma once
#include "DxgiAdapter.h"

#if defined(SHINKOU_PLATFORM_WINDOWS)
namespace shinkou::render {
inline DXGI_FORMAT dxgi_vertex_format(VertexFormat format) {
    constexpr DXGI_FORMAT values[]{DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_R32G32_FLOAT,
                                   DXGI_FORMAT_R32G32B32_FLOAT,    DXGI_FORMAT_R32G32B32A32_FLOAT,
                                   DXGI_FORMAT_R32_UINT,           DXGI_FORMAT_R32G32_UINT,
                                   DXGI_FORMAT_R32G32B32_UINT,     DXGI_FORMAT_R32G32B32A32_UINT,
                                   DXGI_FORMAT_R32_SINT,           DXGI_FORMAT_R32G32_SINT,
                                   DXGI_FORMAT_R32G32B32_SINT,     DXGI_FORMAT_R32G32B32A32_SINT,
                                   DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R8G8B8A8_SNORM,
                                   DXGI_FORMAT_R8G8B8A8_UINT,      DXGI_FORMAT_R16G16_UNORM,
                                   DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16_FLOAT,
                                   DXGI_FORMAT_R16G16B16A16_FLOAT};
    return values[static_cast<unsigned>(format)];
}
inline unsigned dxgi_blend_factor(BlendFactor factor) {
    // D3D11 and D3D12 use identical numeric blend constants.
    constexpr unsigned values[]{1, 2, 3, 4, 9, 10, 5, 6, 7, 8, 14, 15, 14, 15, 11, 16, 17, 18, 19};
    return values[static_cast<unsigned>(factor)];
}
inline unsigned dxgi_alpha_blend_factor(BlendFactor factor) {
    // D3D rejects color factors in the alpha equation; the equivalent
    // alpha factors preserve the component-wise blend semantics.
    if (factor == BlendFactor::SrcColor)
        factor = BlendFactor::SrcAlpha;
    if (factor == BlendFactor::OneMinusSrcColor)
        factor = BlendFactor::OneMinusSrcAlpha;
    if (factor == BlendFactor::DstColor)
        factor = BlendFactor::DstAlpha;
    if (factor == BlendFactor::OneMinusDstColor)
        factor = BlendFactor::OneMinusDstAlpha;
    if (factor == BlendFactor::Src1Color)
        factor = BlendFactor::Src1Alpha;
    if (factor == BlendFactor::OneMinusSrc1Color)
        factor = BlendFactor::OneMinusSrc1Alpha;
    return dxgi_blend_factor(factor);
}
template <class NativeFace>
void fill_dxgi_stencil(NativeFace &output, const StencilFaceState &face) {
    output.StencilFailOp =
        static_cast<decltype(output.StencilFailOp)>(static_cast<unsigned>(face.fail) + 1);
    output.StencilDepthFailOp =
        static_cast<decltype(output.StencilDepthFailOp)>(static_cast<unsigned>(face.depthFail) + 1);
    output.StencilPassOp =
        static_cast<decltype(output.StencilPassOp)>(static_cast<unsigned>(face.pass) + 1);
    output.StencilFunc =
        static_cast<decltype(output.StencilFunc)>(static_cast<unsigned>(face.compare) + 1);
}
template <class NativeDepth>
void fill_dxgi_depth(NativeDepth &output, const DepthStencilState &state) {
    output.DepthEnable = state.depthTest;
    output.DepthWriteMask = static_cast<decltype(output.DepthWriteMask)>(state.depthWrite ? 1 : 0);
    output.DepthFunc =
        static_cast<decltype(output.DepthFunc)>(static_cast<unsigned>(state.depthCompare) + 1);
    output.StencilEnable = state.stencilTest;
    output.StencilReadMask = state.stencilReadMask;
    output.StencilWriteMask = state.stencilWriteMask;
    fill_dxgi_stencil(output.FrontFace, state.front);
    fill_dxgi_stencil(output.BackFace, state.back);
}
template <class NativeBlend>
void fill_dxgi_blend(NativeBlend &output, const BlendAttachmentState &state) {
    output.BlendEnable = state.enabled;
    output.SrcBlend = static_cast<decltype(output.SrcBlend)>(dxgi_blend_factor(state.srcColor));
    output.DestBlend = static_cast<decltype(output.DestBlend)>(dxgi_blend_factor(state.dstColor));
    output.BlendOp =
        static_cast<decltype(output.BlendOp)>(static_cast<unsigned>(state.colorOp) + 1);
    output.SrcBlendAlpha =
        static_cast<decltype(output.SrcBlendAlpha)>(dxgi_alpha_blend_factor(state.srcAlpha));
    output.DestBlendAlpha =
        static_cast<decltype(output.DestBlendAlpha)>(dxgi_alpha_blend_factor(state.dstAlpha));
    output.BlendOpAlpha =
        static_cast<decltype(output.BlendOpAlpha)>(static_cast<unsigned>(state.alphaOp) + 1);
    output.RenderTargetWriteMask = state.writeMask;
}
template <class NativeRaster>
void fill_dxgi_raster(NativeRaster &output, const RasterizerState &state) {
    output.FrontCounterClockwise = state.frontCounterClockwise;
    output.DepthClipEnable = state.depthClip;
    output.DepthBias = state.depthBias;
    output.DepthBiasClamp = state.depthBiasClamp;
    output.SlopeScaledDepthBias = state.slopeScaledDepthBias;
}
} // namespace shinkou::render
#endif

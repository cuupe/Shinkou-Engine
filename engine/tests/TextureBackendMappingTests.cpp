#include "shinkou/render/TextureBackendMapping.h"

#include <algorithm>
#include <iostream>

namespace {

using namespace shinkou::render;

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

TextureDesc color_desc(TextureDimension dimension = TextureDimension::Texture2D) {
    TextureDesc description;
    description.width = 256;
    description.height = 256;
    description.layers = 1;
    description.mipLevels = 0;
    description.format = "rgba8";
    description.dimension = dimension;
    description.formatKind = TextureFormat::RGBA8Unorm;
    return description;
}

} // namespace

int main() {
    const auto capabilities = TextureCapabilities::common();
    bool ok = true;

    const auto twoD = map_texture_to_backend(color_desc(), BackendApi::DirectX12, capabilities);
    ok &= expect(twoD && twoD.plan.imageType == TextureBackendImageType::Texture2D &&
                     twoD.plan.physicalArrayLayers == 1 && twoD.plan.mipLevels == 9,
                 "2D physical image plan is incorrect");

    auto volumeDescription = color_desc(TextureDimension::Texture3D);
    volumeDescription.depth = 32;
    const auto volume = map_texture_to_backend(volumeDescription, BackendApi::Vulkan, capabilities);
    ok &= expect(volume && volume.plan.imageType == TextureBackendImageType::Texture3D &&
                     volume.plan.depth == 32 && volume.plan.physicalArrayLayers == 1,
                 "3D physical image plan is incorrect");

    auto cubeDescription = color_desc(TextureDimension::Cube);
    const auto cube = map_texture_to_backend(cubeDescription, BackendApi::DirectX11, capabilities);
    ok &= expect(cube && cube.plan.cubeCompatible && cube.plan.physicalArrayLayers == 6,
                 "cube physical layer mapping is incorrect");

    auto cubeArrayDescription = color_desc(TextureDimension::CubeArray);
    cubeArrayDescription.layers = 3;
    const auto cubeArray = map_texture_to_backend(cubeArrayDescription, BackendApi::Vulkan, capabilities);
    ok &= expect(cubeArray && cubeArray.plan.physicalArrayLayers == 18,
                 "cube-array physical layer mapping is incorrect");

    auto multisample = color_desc();
    multisample.mipLevels = 1;
    multisample.renderTarget = true;
    multisample.sampleCount = 4;
    multisample.resolve = true;
    multisample.resolveMode = TextureResolveMode::Source;
    const auto msaa = map_texture_to_backend(multisample, BackendApi::DirectX12, capabilities);
    ok &= expect(msaa && msaa.plan.sampleCount == 4 && msaa.plan.resolve.enabled &&
                     has_texture_backend_usage(msaa.plan.usage, TextureBackendUsage::ResolveSource),
                 "MSAA resolve mapping is incorrect");

    auto invalidMultisample = multisample;
    invalidMultisample.renderTarget = false;
    const auto invalidMsaaPlan = map_texture_to_backend(invalidMultisample, BackendApi::DirectX12, capabilities);
    ok &= expect(!invalidMsaaPlan && !invalidMsaaPlan.diagnostics.empty() &&
                     invalidMsaaPlan.diagnostics.front().code == TextureBackendMappingCode::UnsupportedUsage,
                 "non-attachment MSAA texture was accepted");

    auto sampleLimited = capabilities;
    const auto rgba8Capabilities = std::find_if(sampleLimited.formats.begin(), sampleLimited.formats.end(), [](const auto& format) {
        return format.format == TextureFormat::RGBA8Unorm;
    });
    if (rgba8Capabilities != sampleLimited.formats.end()) rgba8Capabilities->sampleCounts = {1, 4};
    auto unsupportedSample = multisample;
    unsupportedSample.sampleCount = 2;
    unsupportedSample.resolve = false;
    unsupportedSample.resolveMode = TextureResolveMode::None;
    const auto unsupportedSamplePlan = map_texture_to_backend(unsupportedSample, BackendApi::Vulkan, sampleLimited);
    ok &= expect(!unsupportedSamplePlan && !unsupportedSamplePlan.diagnostics.empty() &&
                     unsupportedSamplePlan.diagnostics.front().code == TextureBackendMappingCode::UnsupportedSampleCount,
                 "backend sample-count capability was ignored");

    TextureDesc depth;
    depth.width = 128;
    depth.height = 128;
    depth.mipLevels = 1;
    depth.format = "d24s8";
    depth.formatKind = TextureFormat::D24UnormS8Uint;
    depth.depthStencil = true;
    const auto depthView = map_texture_view_to_backend(
        depth, TextureViewDesc{TextureDimension::Auto, TextureFormat::Unknown, 0, 1, 0, 1, TextureAspect::DepthStencil},
        BackendApi::Vulkan, capabilities);
    ok &= expect(depthView && depthView.plan.aspect == TextureAspect::DepthStencil,
                 "depth-stencil view aspect mapping is incorrect");

    auto compressed = color_desc();
    compressed.format = "bc7";
    compressed.formatKind = TextureFormat::BC7RGBAUnorm;
    auto restricted = capabilities;
    restricted.supportsCompression = false;
    restricted.formats.erase(
        std::remove_if(restricted.formats.begin(), restricted.formats.end(), [](const auto& format) {
            return format.format == TextureFormat::BC7RGBAUnorm;
        }), restricted.formats.end());
    const auto rejected = map_texture_to_backend(compressed, BackendApi::DirectX11, restricted);
    ok &= expect(!rejected, "unsupported compressed format was accepted without fallback");
    const auto downgraded = map_texture_to_backend(
        compressed, BackendApi::DirectX11, restricted, TextureBackendMappingOptions{true, {}});
    ok &= expect(downgraded && downgraded.plan.format == TextureFormat::RGBA8Unorm &&
                     downgraded.plan.formatDowngraded,
                 "explicit compressed-format fallback was not applied");

    const auto invalidAspect = map_texture_view_to_backend(
        color_desc(), TextureViewDesc{TextureDimension::Auto, TextureFormat::Unknown, 0, 1, 0, 1, TextureAspect::Depth},
        BackendApi::Vulkan, capabilities);
    ok &= expect(!invalidAspect, "invalid color view aspect was accepted");

    auto arrayDescription = color_desc(TextureDimension::Texture2DArray);
    arrayDescription.layers = 2;
    const auto invalidArrayView = map_texture_view_to_backend(
        arrayDescription, TextureViewDesc{TextureDimension::Texture2D, TextureFormat::Unknown, 0, 1, 0, 2, TextureAspect::Color},
        BackendApi::DirectX12, capabilities);
    ok &= expect(!invalidArrayView, "2D view spanning multiple array layers was accepted");

    const auto dx11Capabilities = TextureCapabilities::for_backend(BackendApi::DirectX11);
    const auto dx12Capabilities = TextureCapabilities::for_backend(BackendApi::DirectX12);
    const auto vulkanCapabilities = TextureCapabilities::for_backend(BackendApi::Vulkan);
    ok &= expect(dx11Capabilities.known && dx12Capabilities.known && vulkanCapabilities.known &&
                     dx11Capabilities.supportsTextureViews && dx12Capabilities.supportsTextureViews &&
                     vulkanCapabilities.supportsTextureViews,
                 "backend texture capability profiles are incomplete");

    return ok ? 0 : 1;
}

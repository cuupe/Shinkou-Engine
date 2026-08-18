#include "shinkou/render/TextureModel.h"
#include <iostream>
#include <limits>

using namespace shinkou::render;

int main() {
    const auto capabilities = TextureCapabilities::common();

    TextureDesc array{};
    array.width = 128;
    array.height = 64;
    array.layers = 4;
    array.mipLevels = 8;
    array.format = "rgba8";
    array.dimension = TextureDimension::Texture2DArray;
    if (!validate_texture_desc(array, capabilities)) return 1;

    TextureDesc volume = array;
    volume.dimension = TextureDimension::Texture3D;
    volume.depth = 16;
    volume.layers = 1;
    if (!validate_texture_desc(volume, capabilities)) return 2;

    TextureDesc cube = array;
    cube.dimension = TextureDimension::Cube;
    cube.width = cube.height = 64;
    cube.layers = 1;
    cube.mipLevels = 1;
    if (!validate_texture_desc(cube, capabilities)) return 3;
    TextureViewDesc cubeView;
    cubeView.dimension = TextureDimension::Cube;
    if (!validate_texture_view(cube, cubeView, capabilities)) return 12;

    TextureDesc msaa{};
    msaa.width = msaa.height = 1920;
    msaa.renderTarget = true;
    msaa.sampleCount = 4;
    msaa.resolve = true;
    msaa.resolveMode = TextureResolveMode::Source;
    if (!validate_texture_desc(msaa, capabilities)) return 4;

    TextureDesc badMsaa = msaa;
    badMsaa.mipLevels = 2;
    if (validate_texture_desc(badMsaa, capabilities).code != TextureValidationCode::InvalidMultisampleTexture) return 5;

    auto nonAttachmentMsaa = msaa;
    nonAttachmentMsaa.renderTarget = false;
    if (validate_texture_desc(nonAttachmentMsaa, capabilities).code != TextureValidationCode::UnsupportedUsage) return 14;

    auto badResolveSource = msaa;
    badResolveSource.sampleCount = 1;
    if (validate_texture_desc(badResolveSource, capabilities).code != TextureValidationCode::InvalidResolve) return 15;

    auto badResolveDestination = msaa;
    badResolveDestination.resolveMode = TextureResolveMode::Destination;
    if (validate_texture_desc(badResolveDestination, capabilities).code != TextureValidationCode::InvalidResolve) return 16;

    TextureDesc depth{};
    depth.width = depth.height = 64;
    depth.format = "d24s8";
    depth.depthStencil = true;
    if (!validate_texture_desc(depth, capabilities)) return 6;

    auto depthWithoutUsage = depth;
    depthWithoutUsage.depthStencil = false;
    if (validate_texture_desc(depthWithoutUsage, capabilities).code != TextureValidationCode::UnsupportedUsage) return 17;

    auto colorWithDepthUsage = array;
    colorWithDepthUsage.depthStencil = true;
    if (validate_texture_desc(colorWithDepthUsage, capabilities).code != TextureValidationCode::UnsupportedUsage) return 18;

    TextureDesc compressed{};
    compressed.width = compressed.height = 64;
    compressed.format = "bc7";
    if (!validate_texture_desc(compressed, capabilities)) return 7;
    compressed.storage = true;
    if (validate_texture_desc(compressed, capabilities).code != TextureValidationCode::UnsupportedUsage) return 8;

    TextureDesc badHdr{};
    badHdr.hdr = true;
    if (validate_texture_desc(badHdr, capabilities).code != TextureValidationCode::UnsupportedUsage) return 13;

    TextureViewDesc view;
    view.dimension = TextureDimension::Texture2D;
    view.baseMipLevel = 2;
    view.mipLevelCount = 2;
    view.baseLayer = 1;
    view.layerCount = 1;
    if (!validate_texture_view(array, view, capabilities)) return 9;

    TextureViewDesc badView = view;
    badView.baseMipLevel = 8;
    if (validate_texture_view(array, badView, capabilities).code != TextureValidationCode::InvalidView) return 10;

    TextureViewDesc arrayAsTexture2D = view;
    arrayAsTexture2D.baseLayer = 1;
    arrayAsTexture2D.layerCount = 1;
    if (!validate_texture_view(array, arrayAsTexture2D, capabilities)) return 19;
    auto allArrayLayersAsTexture2D = arrayAsTexture2D;
    allArrayLayersAsTexture2D.baseLayer = 0;
    allArrayLayersAsTexture2D.layerCount = 2;
    if (validate_texture_view(array, allArrayLayersAsTexture2D, capabilities).code != TextureValidationCode::InvalidView) return 20;

    auto noViewCapabilities = capabilities;
    noViewCapabilities.supportsTextureViews = false;
    auto nonDefaultView = TextureViewDesc{};
    nonDefaultView.mipLevelCount = 1;
    if (validate_texture_view(array, nonDefaultView, noViewCapabilities).code != TextureValidationCode::InvalidView) return 21;

    auto cubeArray = array;
    cubeArray.dimension = TextureDimension::CubeArray;
    cubeArray.width = cubeArray.height = 64;
    cubeArray.layers = 2;
    cubeArray.mipLevels = 1;
    if (!validate_texture_desc(cubeArray, capabilities)) return 22;
    TextureViewDesc oneCube{TextureDimension::Cube, TextureFormat::Unknown, 0, 1, 6, 6, TextureAspect::Auto};
    if (!validate_texture_view(cubeArray, oneCube, capabilities)) return 23;
    TextureViewDesc twoCubes = oneCube;
    twoCubes.layerCount = 12;
    if (validate_texture_view(cubeArray, twoCubes, capabilities).code != TextureValidationCode::InvalidView) return 24;

    TextureViewDesc overflowingView = view;
    overflowingView.baseLayer = 1;
    overflowingView.layerCount = std::numeric_limits<std::uint32_t>::max();
    if (validate_texture_view(array, overflowingView, capabilities).code != TextureValidationCode::InvalidView) return 25;

    if (texture_format_from_name("RGBA16FLOAT") != TextureFormat::RGBA16Float ||
        texture_format_name(TextureFormat::D24UnormS8Uint) != "d24s8" ||
        texture_full_mip_count(volume) != 8) return 11;

    std::cout << "texture model tests passed\n";
    return 0;
}

#include "shinkou/render/TextureModel.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>

namespace shinkou::render {
namespace {

std::string lower(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

TextureFormat description_format(const TextureDesc& description) noexcept {
    return description.formatKind == TextureFormat::Unknown
        ? texture_format_from_name(description.format) : description.formatKind;
}

TextureDimension description_dimension(const TextureDesc& description) noexcept {
    return description.dimension == TextureDimension::Auto
        ? TextureDimension::Texture2D : description.dimension;
}

TextureFormatCapabilities format_capabilities(TextureFormat format) {
    TextureFormatCapabilities result;
    result.format = format;
    result.supported = format != TextureFormat::Unknown;
    result.sampled = true;
    result.filterable = true;
    result.colorAttachment = true;
    result.blendable = true;
    result.storage = true;
    result.resolve = true;
    result.maxSampleCount = 8;
    result.sampleCounts = {1, 2, 4, 8};

    switch (format) {
    case TextureFormat::D24UnormS8Uint:
        result.sampled = false; result.filterable = false; result.colorAttachment = false;
        result.blendable = false; result.storage = false; result.depthStencilAttachment = true;
        result.hdr = false; result.resolve = false;
        break;
    case TextureFormat::D32Float:
        result.sampled = false; result.filterable = false; result.colorAttachment = false;
        result.blendable = false; result.storage = false; result.depthStencilAttachment = true;
        result.resolve = false;
        break;
    case TextureFormat::BC1RGBAUnorm:
    case TextureFormat::BC3RGBAUnorm:
    case TextureFormat::BC5RGUnorm:
    case TextureFormat::BC6HUFloat:
    case TextureFormat::BC7RGBAUnorm:
        result.compressed = true; result.colorAttachment = false; result.blendable = false;
        result.storage = false; result.resolve = false; result.maxSampleCount = 1;
        result.sampleCounts = {1};
        result.hdr = format == TextureFormat::BC6HUFloat;
        break;
    case TextureFormat::RGBA16Float:
        result.hdr = true;
        break;
    case TextureFormat::Unknown:
        result = {};
        result.format = format;
        break;
    default:
        break;
    }
    if (format == TextureFormat::D24UnormS8Uint || format == TextureFormat::D32Float) {
        result.depthStencilAttachment = true;
    }
    return result;
}

std::vector<TextureFormatCapabilities> common_format_capabilities() {
    return {
        format_capabilities(TextureFormat::RGBA8Unorm),
        format_capabilities(TextureFormat::BGRA8Unorm),
        format_capabilities(TextureFormat::RGBA16Float),
        format_capabilities(TextureFormat::R32Float),
        format_capabilities(TextureFormat::D24UnormS8Uint),
        format_capabilities(TextureFormat::D32Float),
        format_capabilities(TextureFormat::BC1RGBAUnorm),
        format_capabilities(TextureFormat::BC3RGBAUnorm),
        format_capabilities(TextureFormat::BC5RGUnorm),
        format_capabilities(TextureFormat::BC6HUFloat),
        format_capabilities(TextureFormat::BC7RGBAUnorm),
    };
}

TextureValidationResult failure(TextureValidationCode code, std::string message) {
    return {code, std::move(message)};
}

std::uint32_t effective_mips(const TextureDesc& description) noexcept {
    return description.mipLevels == 0 ? texture_full_mip_count(description) : description.mipLevels;
}

bool is_cube(TextureDimension dimension) noexcept {
    return dimension == TextureDimension::Cube || dimension == TextureDimension::CubeArray;
}

} // namespace

const TextureFormatCapabilities* TextureCapabilities::format_capabilities(TextureFormat format) const noexcept {
    const auto it = std::find_if(formats.begin(), formats.end(), [format](const auto& entry) {
        return entry.format == format;
    });
    return it == formats.end() ? nullptr : &*it;
}

bool TextureCapabilities::supports_format(TextureFormat format) const noexcept {
    const auto* entry = format_capabilities(format);
    return known && entry != nullptr && entry->supported;
}

bool TextureCapabilities::supports_sample_count(TextureFormat format, std::uint32_t sampleCount) const noexcept {
    const auto* entry = format_capabilities(format);
    if (!entry || !entry->supported || sampleCount == 0) return false;
    if (sampleCount == 1) return true;
    if (entry->maxSampleCount == 0 || sampleCount > entry->maxSampleCount || entry->sampleCounts.empty()) return false;
    return std::find(entry->sampleCounts.begin(), entry->sampleCounts.end(), sampleCount) != entry->sampleCounts.end();
}

TextureCapabilities TextureCapabilities::common() {
    TextureCapabilities result;
    result.known = true;
    result.supports2D = true;
    result.supports3D = true;
    result.supportsCube = true;
    result.supports2DArray = true;
    result.supportsCubeArray = true;
    result.supportsTextureViews = true;
    result.supportsResolve = true;
    result.supportsDepth = true;
    result.supportsHDR = true;
    result.supportsCompression = true;
    result.maxWidth = 16384;
    result.maxHeight = 16384;
    result.maxDepth = 2048;
    result.maxLayers = 2048;
    result.maxMipLevels = 15;
    result.formats = common_format_capabilities();
    return result;
}

TextureCapabilities TextureCapabilities::for_backend(BackendApi api) {
    auto result = common();
    // These are model-level defaults. A device-backed implementation can
    // replace them with queried limits. Keep the profiles distinct where the
    // API has a different baseline rather than silently treating all backends
    // as interchangeable.
    switch (api) {
    case BackendApi::Null:
        break;
    case BackendApi::DirectX11:
        // D3D11's guaranteed texture-view path is 2D/array based; cube
        // resources are still represented as 2D arrays with cube metadata.
        result.supportsCubeArray = true;
        result.supportsTextureViews = true;
        break;
    case BackendApi::DirectX12:
        // D3D12 supports the same resource dimensions, but its resolve path
        // is explicitly color-only in this backend-neutral model.
        result.supportsCubeArray = true;
        result.supportsTextureViews = true;
        break;
    case BackendApi::Vulkan:
        // Vulkan image-view dimensions are explicit and all modelled view
        // forms are available when the corresponding image feature exists.
        result.supportsCubeArray = true;
        result.supportsTextureViews = true;
        break;
    }
    return result;
}

TextureFormat texture_format_from_name(std::string_view name) noexcept {
    const auto value = lower(name);
    if (value == "rgba8" || value == "rgba8unorm" || value == "rgba8_unorm") return TextureFormat::RGBA8Unorm;
    if (value == "bgra8" || value == "bgra8unorm" || value == "bgra8_unorm") return TextureFormat::BGRA8Unorm;
    if (value == "rgba16f" || value == "rgba16float") return TextureFormat::RGBA16Float;
    if (value == "r32f" || value == "r32float") return TextureFormat::R32Float;
    if (value == "d24s8" || value == "d24_unorm_s8_uint") return TextureFormat::D24UnormS8Uint;
    if (value == "d32f" || value == "d32float") return TextureFormat::D32Float;
    if (value == "bc1" || value == "bc1_rgba" || value == "bc1_rgba_unorm") return TextureFormat::BC1RGBAUnorm;
    if (value == "bc3" || value == "bc3_rgba" || value == "bc3_rgba_unorm") return TextureFormat::BC3RGBAUnorm;
    if (value == "bc5" || value == "bc5_rg" || value == "bc5_rg_unorm") return TextureFormat::BC5RGUnorm;
    if (value == "bc6h" || value == "bc6h_ufloat" || value == "bc6hufloat") return TextureFormat::BC6HUFloat;
    if (value == "bc7" || value == "bc7_rgba" || value == "bc7_rgba_unorm") return TextureFormat::BC7RGBAUnorm;
    return TextureFormat::Unknown;
}

std::string_view texture_format_name(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::RGBA8Unorm: return "rgba8";
    case TextureFormat::BGRA8Unorm: return "bgra8";
    case TextureFormat::RGBA16Float: return "rgba16f";
    case TextureFormat::R32Float: return "r32f";
    case TextureFormat::D24UnormS8Uint: return "d24s8";
    case TextureFormat::D32Float: return "d32f";
    case TextureFormat::BC1RGBAUnorm: return "bc1";
    case TextureFormat::BC3RGBAUnorm: return "bc3";
    case TextureFormat::BC5RGUnorm: return "bc5";
    case TextureFormat::BC6HUFloat: return "bc6h";
    case TextureFormat::BC7RGBAUnorm: return "bc7";
    case TextureFormat::Unknown: break;
    }
    return "unknown";
}

bool texture_format_is_depth(TextureFormat format) noexcept {
    return format == TextureFormat::D24UnormS8Uint || format == TextureFormat::D32Float;
}

bool texture_format_is_hdr(TextureFormat format) noexcept {
    return format == TextureFormat::RGBA16Float || format == TextureFormat::BC6HUFloat;
}

bool texture_format_is_compressed(TextureFormat format) noexcept {
    return format == TextureFormat::BC1RGBAUnorm || format == TextureFormat::BC3RGBAUnorm ||
        format == TextureFormat::BC5RGUnorm || format == TextureFormat::BC6HUFloat ||
        format == TextureFormat::BC7RGBAUnorm;
}

std::uint32_t texture_full_mip_count(const TextureDesc& description) noexcept {
    const auto largest = std::max({description.width, description.height,
                                   description.dimension == TextureDimension::Texture3D ? description.depth : 1u});
    if (largest == 0) return 0;
    std::uint32_t result = 1;
    for (auto extent = largest; extent > 1; extent >>= 1) ++result;
    return result;
}

std::uint32_t texture_physical_layer_count(const TextureDesc& description) noexcept {
    if (description.dimension == TextureDimension::Texture3D) return 1;
    const auto layers = description.layers == 0 ? 0u : description.layers;
    if (!is_cube(description.dimension)) return layers;
    if (layers > std::numeric_limits<std::uint32_t>::max() / 6u) return std::numeric_limits<std::uint32_t>::max();
    return layers * 6u;
}

TextureValidationResult validate_texture_desc(const TextureDesc& description,
                                              const TextureCapabilities& capabilities) {
    if (!capabilities.known) return failure(TextureValidationCode::CapabilitiesUnavailable, "texture capabilities are unavailable");
    if (description.width == 0 || description.height == 0 ||
        (description.dimension == TextureDimension::Texture3D && description.depth == 0)) {
        return failure(TextureValidationCode::InvalidDimensions, "texture dimensions must be non-zero");
    }
    const auto dimension = description_dimension(description);
    if ((capabilities.maxWidth != 0 && description.width > capabilities.maxWidth) ||
        (capabilities.maxHeight != 0 && description.height > capabilities.maxHeight) ||
        (dimension == TextureDimension::Texture3D && capabilities.maxDepth != 0 && description.depth > capabilities.maxDepth)) {
        return failure(TextureValidationCode::InvalidDimensions, "texture dimensions exceed capabilities");
    }

    switch (dimension) {
    case TextureDimension::Texture2D:
        if (!capabilities.supports2D) return failure(TextureValidationCode::UnsupportedDimension, "2D textures are unsupported");
        if (description.depth != 1) return failure(TextureValidationCode::InvalidDimensions, "2D textures must have depth one");
        if (description.layers != 1) return failure(TextureValidationCode::InvalidLayers, "a 2D texture must have one layer");
        break;
    case TextureDimension::Texture3D:
        if (!capabilities.supports3D) return failure(TextureValidationCode::UnsupportedDimension, "3D textures are unsupported");
        if (description.layers != 1) return failure(TextureValidationCode::InvalidLayers, "a 3D texture cannot have array layers");
        break;
    case TextureDimension::Cube:
        if (!capabilities.supportsCube) return failure(TextureValidationCode::UnsupportedDimension, "cube textures are unsupported");
        if (description.width != description.height) return failure(TextureValidationCode::InvalidDimensions, "cube textures must be square");
        if (description.depth != 1) return failure(TextureValidationCode::InvalidDimensions, "cube textures must have depth one");
        if (description.layers != 1) return failure(TextureValidationCode::InvalidLayers, "a cube texture has one logical layer");
        break;
    case TextureDimension::Texture2DArray:
        if (!capabilities.supports2DArray) return failure(TextureValidationCode::UnsupportedDimension, "2D texture arrays are unsupported");
        if (description.depth != 1) return failure(TextureValidationCode::InvalidDimensions, "2D texture arrays must have depth one");
        if (description.layers == 0) return failure(TextureValidationCode::InvalidLayers, "a 2D array must have at least one layer");
        break;
    case TextureDimension::CubeArray:
        if (!capabilities.supportsCubeArray) return failure(TextureValidationCode::UnsupportedDimension, "cube texture arrays are unsupported");
        if (description.width != description.height) return failure(TextureValidationCode::InvalidDimensions, "cube textures must be square");
        if (description.depth != 1) return failure(TextureValidationCode::InvalidDimensions, "cube arrays must have depth one");
        if (description.layers == 0) return failure(TextureValidationCode::InvalidLayers, "a cube array must have at least one logical layer");
        break;
    case TextureDimension::Auto:
        break;
    }
    if (capabilities.maxLayers != 0 && texture_physical_layer_count(description) > capabilities.maxLayers && dimension != TextureDimension::Texture3D) {
        return failure(TextureValidationCode::InvalidLayers, "texture layers exceed capabilities");
    }

    const auto format = description_format(description);
    const auto* formatCaps = capabilities.format_capabilities(format);
    if (!formatCaps || !formatCaps->supported) return failure(TextureValidationCode::UnsupportedFormat, "texture format is unsupported");
    const auto depthFormat = texture_format_is_depth(format);
    const auto compressedFormat = texture_format_is_compressed(format);
    const auto hdrFormat = texture_format_is_hdr(format);
    if (depthFormat && !capabilities.supportsDepth) return failure(TextureValidationCode::UnsupportedUsage, "depth textures are unsupported");
    if (description.hdr && (!hdrFormat || !formatCaps->hdr || !capabilities.supportsHDR)) return failure(TextureValidationCode::UnsupportedUsage, "hdr=true requires a supported HDR format");
    if (texture_format_is_hdr(format) && !capabilities.supportsHDR) return failure(TextureValidationCode::UnsupportedUsage, "HDR textures are unsupported");
    if (hdrFormat && !formatCaps->hdr) return failure(TextureValidationCode::UnsupportedUsage, "backend does not expose HDR support for this format");
    if (compressedFormat && !capabilities.supportsCompression) return failure(TextureValidationCode::UnsupportedUsage, "compressed textures are unsupported");
    if (description.depthStencil != depthFormat) return failure(TextureValidationCode::UnsupportedUsage,
        depthFormat ? "a depth format requires depthStencil usage" : "depthStencil usage requires a depth format");
    if (description.renderTarget && depthFormat && !formatCaps->depthStencilAttachment) return failure(TextureValidationCode::UnsupportedUsage, "format cannot be a depth-stencil attachment");
    if (description.renderTarget && !depthFormat && !formatCaps->colorAttachment) return failure(TextureValidationCode::UnsupportedUsage, "format cannot be a color attachment");
    if (description.depthStencil && !formatCaps->depthStencilAttachment) return failure(TextureValidationCode::UnsupportedUsage, "format cannot be a depth-stencil attachment");
    if (description.storage && !formatCaps->storage) return failure(TextureValidationCode::UnsupportedUsage, "format cannot be a storage texture");
    if (compressedFormat && (description.renderTarget || description.storage || description.depthStencil)) return failure(TextureValidationCode::UnsupportedUsage, "compressed formats cannot be render, depth, or storage targets");
    if (description.hdr && (depthFormat || compressedFormat && !hdrFormat)) return failure(TextureValidationCode::UnsupportedUsage, "HDR usage is incompatible with this format");

    const auto mipLevels = effective_mips(description);
    if (mipLevels == 0 || mipLevels > texture_full_mip_count(description) ||
        (capabilities.maxMipLevels != 0 && mipLevels > capabilities.maxMipLevels)) {
        return failure(TextureValidationCode::InvalidMipLevels, "mipLevels exceeds the texture mip chain or capability");
    }
    if (description.sampleCount == 0 || !capabilities.supports_sample_count(format, description.sampleCount)) {
        return failure(TextureValidationCode::UnsupportedSampleCount, "sampleCount is unsupported for this format");
    }
    if (description.sampleCount > 1 && (mipLevels != 1 || dimension == TextureDimension::Texture3D || is_cube(dimension) || compressedFormat)) {
        return failure(TextureValidationCode::InvalidMultisampleTexture, "MSAA is only valid for uncompressed 2D resources");
    }
    if (description.sampleCount > 1 && !description.renderTarget && !description.depthStencil) {
        return failure(TextureValidationCode::UnsupportedUsage, "multisample textures must be render or depth-stencil attachments");
    }
    if (description.sampleCount > 1 && description.storage) {
        return failure(TextureValidationCode::UnsupportedUsage, "multisample textures cannot be storage textures");
    }
    if (description.generateMips && (mipLevels <= 1 || description.sampleCount != 1 || depthFormat || compressedFormat)) {
        return failure(TextureValidationCode::UnsupportedUsage, "mip generation requires multiple mips, one sample, and a color format");
    }

    if (description.resolve) {
        if (!capabilities.supportsResolve || !formatCaps->resolve) return failure(TextureValidationCode::InvalidResolve, "resolve is unsupported");
        if (description.resolveMode == TextureResolveMode::None) return failure(TextureValidationCode::InvalidResolve, "resolve mode must be Source or Destination");
        if (description.resolveMode == TextureResolveMode::Source && (description.sampleCount <= 1 || !description.renderTarget)) return failure(TextureValidationCode::InvalidResolve, "a resolve source must be a multisampled render target");
        if (description.resolveMode == TextureResolveMode::Destination && description.sampleCount != 1) return failure(TextureValidationCode::InvalidResolve, "a resolve destination must have one sample");
        if (depthFormat) return failure(TextureValidationCode::InvalidResolve, "depth-stencil resolve is not supported by the model");
    } else if (description.resolveMode != TextureResolveMode::None) {
        return failure(TextureValidationCode::InvalidResolve, "resolveMode requires resolve=true");
    }
    return {};
}

TextureValidationResult validate_texture_view(const TextureDesc& texture,
                                              const TextureViewDesc& view,
                                              const TextureCapabilities& capabilities) {
    const auto resource = validate_texture_desc(texture, capabilities);
    if (!resource) return resource;
    const auto sourceDimension = description_dimension(texture);
    const auto viewDimension = view.dimension == TextureDimension::Auto ? sourceDimension : view.dimension;
    const auto mipLevels = effective_mips(texture);
    const auto layers = texture_physical_layer_count(texture);
    if (view.baseMipLevel >= mipLevels || view.baseLayer >= layers) return failure(TextureValidationCode::InvalidView, "view base range is outside the texture");
    const auto mipCount = view.mipLevelCount == 0 ? mipLevels - view.baseMipLevel : view.mipLevelCount;
    const auto layerCount = view.layerCount == 0 ? layers - view.baseLayer : view.layerCount;
    if (mipCount == 0 || mipCount > mipLevels - view.baseMipLevel) return failure(TextureValidationCode::InvalidView, "view mip range is outside the texture");
    if (layerCount == 0 || layerCount > layers - view.baseLayer) return failure(TextureValidationCode::InvalidView, "view layer range is outside the texture");

    bool dimensionCompatible = viewDimension == sourceDimension;
    if (sourceDimension == TextureDimension::Texture2DArray && viewDimension == TextureDimension::Texture2D) {
        dimensionCompatible = layerCount == 1;
    } else if (sourceDimension == TextureDimension::CubeArray && viewDimension == TextureDimension::Cube) {
        dimensionCompatible = view.baseLayer % 6 == 0 && layerCount == 6;
    }
    if (!dimensionCompatible) return failure(TextureValidationCode::InvalidView, "view dimension or layer span does not match the texture");
    if (is_cube(viewDimension) && (view.baseLayer % 6 != 0 || layerCount % 6 != 0)) return failure(TextureValidationCode::InvalidView, "cube views require complete cube layer groups");

    const auto viewSupportsDimension = [&capabilities](TextureDimension dimension) {
        switch (dimension) {
        case TextureDimension::Texture2D: return capabilities.supports2D;
        case TextureDimension::Texture3D: return capabilities.supports3D;
        case TextureDimension::Cube: return capabilities.supportsCube;
        case TextureDimension::Texture2DArray: return capabilities.supports2DArray;
        case TextureDimension::CubeArray: return capabilities.supportsCubeArray;
        case TextureDimension::Auto: return false;
        }
        return false;
    };
    if (!viewSupportsDimension(viewDimension)) return failure(TextureValidationCode::InvalidView, "view dimension is unsupported");
    if (sourceDimension == TextureDimension::Texture3D && (view.baseLayer != 0 || layerCount != 1)) return failure(TextureValidationCode::InvalidView, "3D views cannot select array layers");
    const auto format = view.format == TextureFormat::Unknown ? description_format(texture) : view.format;
    if (format != description_format(texture)) return failure(TextureValidationCode::InvalidView, "format reinterpretation is not supported by this model");
    if (view.aspect != TextureAspect::Auto) {
        if (!texture_format_is_depth(format) && view.aspect != TextureAspect::Color) return failure(TextureValidationCode::InvalidView, "color textures only expose the color aspect");
        if (texture_format_is_depth(format) && (view.aspect == TextureAspect::Color ||
            (view.aspect == TextureAspect::Stencil && format != TextureFormat::D24UnormS8Uint) ||
            (view.aspect == TextureAspect::DepthStencil && format != TextureFormat::D24UnormS8Uint))) {
            return failure(TextureValidationCode::InvalidView, "view aspect is not present in the texture format");
        }
    }
    if (!capabilities.supportsTextureViews && (view.dimension != TextureDimension::Auto || view.format != TextureFormat::Unknown ||
        view.baseMipLevel != 0 || view.mipLevelCount != 0 || view.baseLayer != 0 || view.layerCount != 0 || view.aspect != TextureAspect::Auto)) {
        return failure(TextureValidationCode::InvalidView, "texture views are unsupported");
    }
    return {};
}

} // namespace shinkou::render

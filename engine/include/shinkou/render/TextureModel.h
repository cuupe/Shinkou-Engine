#pragma once

#include "shinkou/render/RenderTypes.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::render {

enum class TextureAspect { Auto, Color, Depth, Stencil, DepthStencil };

// Zero mip/layer counts mean "the rest". TextureDesc::layers is the logical
// array count: a cube has one logical layer and a cube array has N; view layer
// ranges use physical faces (six per cube).
struct TextureViewDesc {
    TextureDimension dimension{TextureDimension::Auto};
    TextureFormat format{TextureFormat::Unknown};
    std::uint32_t baseMipLevel{0};
    std::uint32_t mipLevelCount{0};
    std::uint32_t baseLayer{0};
    std::uint32_t layerCount{0};
    TextureAspect aspect{TextureAspect::Auto};
};

struct TextureFormatCapabilities {
    TextureFormat format{TextureFormat::Unknown};
    bool supported{false};
    bool sampled{false};
    bool storage{false};
    bool colorAttachment{false};
    bool depthStencilAttachment{false};
    bool filterable{false};
    bool blendable{false};
    bool compressed{false};
    bool hdr{false};
    bool resolve{false};
    std::uint32_t maxSampleCount{1};
    std::vector<std::uint32_t> sampleCounts;
};

struct TextureCapabilities {
    // Validation fails closed until a backend populates this optional model.
    bool known{false};
    bool supports2D{false};
    bool supports3D{false};
    bool supportsCube{false};
    bool supports2DArray{false};
    bool supportsCubeArray{false};
    bool supportsTextureViews{false};
    bool supportsResolve{false};
    bool supportsDepth{false};
    bool supportsHDR{false};
    bool supportsCompression{false};
    std::uint32_t maxWidth{0};
    std::uint32_t maxHeight{0};
    std::uint32_t maxDepth{0};
    std::uint32_t maxLayers{0};
    std::uint32_t maxMipLevels{0};
    std::vector<TextureFormatCapabilities> formats;

    const TextureFormatCapabilities* format_capabilities(TextureFormat format) const noexcept;
    bool supports_format(TextureFormat format) const noexcept;
    bool supports_sample_count(TextureFormat format, std::uint32_t sampleCount) const noexcept;
    static TextureCapabilities common();
    static TextureCapabilities for_backend(BackendApi api);
};

enum class TextureValidationCode {
    None,
    CapabilitiesUnavailable,
    InvalidDimensions,
    UnsupportedDimension,
    UnsupportedFormat,
    InvalidMipLevels,
    InvalidLayers,
    UnsupportedSampleCount,
    InvalidMultisampleTexture,
    UnsupportedUsage,
    InvalidResolve,
    InvalidView
};

struct TextureValidationResult {
    TextureValidationCode code{TextureValidationCode::None};
    std::string message;
    bool valid() const noexcept { return code == TextureValidationCode::None; }
    explicit operator bool() const noexcept { return valid(); }
};

TextureFormat texture_format_from_name(std::string_view name) noexcept;
std::string_view texture_format_name(TextureFormat format) noexcept;
bool texture_format_is_depth(TextureFormat format) noexcept;
bool texture_format_is_hdr(TextureFormat format) noexcept;
bool texture_format_is_compressed(TextureFormat format) noexcept;
std::uint32_t texture_full_mip_count(const TextureDesc& description) noexcept;
std::uint32_t texture_physical_layer_count(const TextureDesc& description) noexcept;

TextureValidationResult validate_texture_desc(const TextureDesc& description,
                                              const TextureCapabilities& capabilities);
TextureValidationResult validate_texture_view(const TextureDesc& texture,
                                              const TextureViewDesc& view,
                                              const TextureCapabilities& capabilities);

} // namespace shinkou::render

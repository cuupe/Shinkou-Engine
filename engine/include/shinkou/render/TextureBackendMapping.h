#pragma once

#include "shinkou/render/TextureModel.h"
#include <cstdint>
#include <string>
#include <vector>

namespace shinkou::render {

enum class TextureBackendImageType : std::uint8_t {
    Texture2D,
    Texture3D,
};

enum class TextureBackendUsage : std::uint32_t {
    None = 0,
    Sampled = 1u << 0,
    Storage = 1u << 1,
    ColorAttachment = 1u << 2,
    DepthStencilAttachment = 1u << 3,
    ResolveSource = 1u << 4,
    ResolveDestination = 1u << 5,
};

constexpr TextureBackendUsage operator|(TextureBackendUsage left, TextureBackendUsage right) noexcept {
    return static_cast<TextureBackendUsage>(static_cast<std::uint32_t>(left) |
                                            static_cast<std::uint32_t>(right));
}

constexpr TextureBackendUsage& operator|=(TextureBackendUsage& left, TextureBackendUsage right) noexcept {
    left = left | right;
    return left;
}

constexpr bool has_texture_backend_usage(TextureBackendUsage value, TextureBackendUsage flag) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

struct TextureBackendResolvePlan {
    bool enabled{false};
    TextureResolveMode mode{TextureResolveMode::None};
    std::uint32_t sampleCount{1};
    TextureFormat format{TextureFormat::Unknown};
};

struct TexturePhysicalImagePlan {
    BackendApi backend{BackendApi::Null};
    TextureDimension logicalDimension{TextureDimension::Texture2D};
    TextureBackendImageType imageType{TextureBackendImageType::Texture2D};
    TextureFormat requestedFormat{TextureFormat::Unknown};
    TextureFormat format{TextureFormat::Unknown};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t depth{1};
    // APIs consume cube maps as 2D images with six physical array slices.
    std::uint32_t physicalArrayLayers{1};
    std::uint32_t mipLevels{1};
    std::uint32_t sampleCount{1};
    bool cubeCompatible{false};
    bool formatDowngraded{false};
    TextureBackendUsage usage{TextureBackendUsage::None};
    TextureBackendResolvePlan resolve{};
};

struct TextureBackendViewPlan {
    BackendApi backend{BackendApi::Null};
    TextureDimension logicalDimension{TextureDimension::Texture2D};
    TextureBackendImageType imageType{TextureBackendImageType::Texture2D};
    TextureFormat requestedFormat{TextureFormat::Unknown};
    TextureFormat format{TextureFormat::Unknown};
    TextureAspect aspect{TextureAspect::Color};
    std::uint32_t baseMipLevel{0};
    std::uint32_t mipLevelCount{0};
    std::uint32_t baseLayer{0};
    std::uint32_t layerCount{0};
    bool cubeCompatible{false};
    bool formatDowngraded{false};
};

enum class TextureBackendMappingCode : std::uint8_t {
    None,
    InvalidDescription,
    InvalidView,
    CapabilitiesUnavailable,
    UnsupportedDimension,
    UnsupportedFormat,
    UnsupportedUsage,
    UnsupportedResolve,
    FormatDowngraded,
    UnsupportedSampleCount,
    InvalidMultisample,
};

enum class TextureBackendDiagnosticSeverity : std::uint8_t { Warning, Error };

struct TextureBackendDiagnostic {
    TextureBackendDiagnosticSeverity severity{TextureBackendDiagnosticSeverity::Error};
    TextureBackendMappingCode code{TextureBackendMappingCode::None};
    std::string message;
};

struct TextureBackendMappingResult {
    TexturePhysicalImagePlan plan{};
    std::vector<TextureBackendDiagnostic> diagnostics;

    bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }
    std::string error_summary() const;
};

struct TextureBackendViewMappingResult {
    TextureBackendViewPlan plan{};
    std::vector<TextureBackendDiagnostic> diagnostics;

    bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }
    std::string error_summary() const;
};

struct TextureBackendMappingOptions {
    bool allowFormatDowngrade{false};
    // Candidates are tried in order. An empty list uses the conservative
    // built-in color fallbacks (BC formats -> RGBA8, BC6H -> RGBA16F).
    std::vector<TextureFormat> formatFallbacks;
};

TextureBackendMappingResult map_texture_to_backend(
    const TextureDesc& description, BackendApi backend,
    const TextureCapabilities& capabilities,
    const TextureBackendMappingOptions& options = {});

TextureBackendViewMappingResult map_texture_view_to_backend(
    const TextureDesc& texture, const TextureViewDesc& view, BackendApi backend,
    const TextureCapabilities& capabilities,
    const TextureBackendMappingOptions& options = {});

// Descriptive aliases for callers that prefer the word "plan".
TextureBackendMappingResult build_texture_backend_plan(
    const TextureDesc& description, BackendApi backend,
    const TextureCapabilities& capabilities,
    const TextureBackendMappingOptions& options = {});

TextureBackendViewMappingResult build_texture_view_backend_plan(
    const TextureDesc& texture, const TextureViewDesc& view, BackendApi backend,
    const TextureCapabilities& capabilities,
    const TextureBackendMappingOptions& options = {});

} // namespace shinkou::render

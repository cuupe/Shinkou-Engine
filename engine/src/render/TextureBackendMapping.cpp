#include "shinkou/render/TextureBackendMapping.h"

#include <sstream>
#include <utility>

namespace shinkou::render {
namespace {

TextureFormat requested_format(const TextureDesc& description) noexcept {
    return description.formatKind == TextureFormat::Unknown
        ? texture_format_from_name(description.format) : description.formatKind;
}

TextureDimension logical_dimension(const TextureDesc& description) noexcept {
    return description.dimension == TextureDimension::Auto
        ? TextureDimension::Texture2D : description.dimension;
}

TextureBackendImageType image_type(TextureDimension dimension) noexcept {
    return dimension == TextureDimension::Texture3D
        ? TextureBackendImageType::Texture3D : TextureBackendImageType::Texture2D;
}

bool is_cube_dimension(TextureDimension dimension) noexcept {
    return dimension == TextureDimension::Cube || dimension == TextureDimension::CubeArray;
}

TextureBackendMappingCode mapping_code(TextureValidationCode code) noexcept {
    switch (code) {
    case TextureValidationCode::CapabilitiesUnavailable: return TextureBackendMappingCode::CapabilitiesUnavailable;
    case TextureValidationCode::UnsupportedDimension: return TextureBackendMappingCode::UnsupportedDimension;
    case TextureValidationCode::UnsupportedFormat: return TextureBackendMappingCode::UnsupportedFormat;
    case TextureValidationCode::UnsupportedUsage: return TextureBackendMappingCode::UnsupportedUsage;
    case TextureValidationCode::UnsupportedSampleCount: return TextureBackendMappingCode::UnsupportedSampleCount;
    case TextureValidationCode::InvalidMultisampleTexture: return TextureBackendMappingCode::InvalidMultisample;
    case TextureValidationCode::InvalidResolve: return TextureBackendMappingCode::UnsupportedResolve;
    case TextureValidationCode::None: return TextureBackendMappingCode::None;
    default: return TextureBackendMappingCode::InvalidDescription;
    }
}

void add_error(std::vector<TextureBackendDiagnostic>& diagnostics,
               TextureBackendMappingCode code, std::string message) {
    diagnostics.push_back({TextureBackendDiagnosticSeverity::Error, code, std::move(message)});
}

void add_warning(std::vector<TextureBackendDiagnostic>& diagnostics,
                 TextureBackendMappingCode code, std::string message) {
    diagnostics.push_back({TextureBackendDiagnosticSeverity::Warning, code, std::move(message)});
}

std::vector<TextureFormat> default_fallbacks(TextureFormat format) {
    switch (format) {
    case TextureFormat::BC6HUFloat:
        return {TextureFormat::RGBA16Float};
    case TextureFormat::BC1RGBAUnorm:
    case TextureFormat::BC3RGBAUnorm:
    case TextureFormat::BC7RGBAUnorm:
        return {TextureFormat::RGBA8Unorm};
    case TextureFormat::BGRA8Unorm:
        return {TextureFormat::RGBA8Unorm};
    default:
        return {};
    }
}

TextureDesc with_format(const TextureDesc& source, TextureFormat format) {
    auto result = source;
    result.formatKind = format;
    result.format = std::string(texture_format_name(format));
    return result;
}

void add_validation_error(std::vector<TextureBackendDiagnostic>& diagnostics,
                          const TextureValidationResult& validation) {
    add_error(diagnostics, mapping_code(validation.code), validation.message);
}

TextureAspect default_aspect(TextureFormat format) noexcept {
    if (!texture_format_is_depth(format)) return TextureAspect::Color;
    return format == TextureFormat::D24UnormS8Uint ? TextureAspect::DepthStencil : TextureAspect::Depth;
}

} // namespace

bool TextureBackendMappingResult::valid() const noexcept {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == TextureBackendDiagnosticSeverity::Error) return false;
    }
    return plan.format != TextureFormat::Unknown;
}

std::string TextureBackendMappingResult::error_summary() const {
    std::ostringstream stream;
    bool first = true;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity != TextureBackendDiagnosticSeverity::Error) continue;
        if (!first) stream << "; ";
        first = false;
        stream << diagnostic.message;
    }
    return stream.str();
}

bool TextureBackendViewMappingResult::valid() const noexcept {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == TextureBackendDiagnosticSeverity::Error) return false;
    }
    return plan.format != TextureFormat::Unknown;
}

std::string TextureBackendViewMappingResult::error_summary() const {
    std::ostringstream stream;
    bool first = true;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity != TextureBackendDiagnosticSeverity::Error) continue;
        if (!first) stream << "; ";
        first = false;
        stream << diagnostic.message;
    }
    return stream.str();
}

TextureBackendMappingResult map_texture_to_backend(
    const TextureDesc& description, BackendApi backend,
    const TextureCapabilities& capabilities,
    const TextureBackendMappingOptions& options) {
    TextureBackendMappingResult result;
    result.plan.backend = backend;
    const auto requested = requested_format(description);
    auto selectedDescription = description;
    auto validation = validate_texture_desc(selectedDescription, capabilities);
    bool downgraded = false;

    if (!validation && options.allowFormatDowngrade &&
        (validation.code == TextureValidationCode::UnsupportedFormat ||
         validation.code == TextureValidationCode::UnsupportedUsage)) {
        auto candidates = options.formatFallbacks;
        if (candidates.empty()) candidates = default_fallbacks(requested);
        for (const auto candidate : candidates) {
            if (candidate == requested) continue;
            auto candidateDescription = with_format(description, candidate);
            const auto candidateValidation = validate_texture_desc(candidateDescription, capabilities);
            if (candidateValidation) {
                selectedDescription = std::move(candidateDescription);
                validation = candidateValidation;
                downgraded = true;
                break;
            }
        }
    }

    if (!validation) {
        add_validation_error(result.diagnostics, validation);
        return result;
    }

    const auto dimension = logical_dimension(selectedDescription);
    const auto format = requested_format(selectedDescription);
    result.plan.logicalDimension = dimension;
    result.plan.imageType = image_type(dimension);
    result.plan.requestedFormat = requested;
    result.plan.format = format;
    result.plan.width = selectedDescription.width;
    result.plan.height = selectedDescription.height;
    result.plan.depth = dimension == TextureDimension::Texture3D ? selectedDescription.depth : 1;
    result.plan.physicalArrayLayers = texture_physical_layer_count(selectedDescription);
    result.plan.mipLevels = selectedDescription.mipLevels == 0
        ? texture_full_mip_count(selectedDescription) : selectedDescription.mipLevels;
    result.plan.sampleCount = selectedDescription.sampleCount;
    result.plan.cubeCompatible = is_cube_dimension(dimension);
    result.plan.formatDowngraded = downgraded;
    if (selectedDescription.storage) result.plan.usage |= TextureBackendUsage::Storage;
    if (!texture_format_is_depth(format)) result.plan.usage |= TextureBackendUsage::Sampled;
    if (selectedDescription.depthStencil || texture_format_is_depth(format)) {
        result.plan.usage |= TextureBackendUsage::DepthStencilAttachment;
    } else if (selectedDescription.renderTarget || selectedDescription.generateMips) {
        result.plan.usage |= TextureBackendUsage::ColorAttachment;
    }
    result.plan.resolve.enabled = selectedDescription.resolve;
    result.plan.resolve.mode = selectedDescription.resolveMode;
    result.plan.resolve.sampleCount = selectedDescription.sampleCount;
    result.plan.resolve.format = format;
    if (selectedDescription.resolveMode == TextureResolveMode::Source) {
        result.plan.usage |= TextureBackendUsage::ResolveSource;
    } else if (selectedDescription.resolveMode == TextureResolveMode::Destination) {
        result.plan.usage |= TextureBackendUsage::ResolveDestination;
    }

    if (downgraded) {
        add_warning(result.diagnostics, TextureBackendMappingCode::FormatDowngraded,
                    "texture format " + std::string(texture_format_name(requested)) +
                    " was downgraded to " + std::string(texture_format_name(format)) +
                    " for backend capability compatibility");
    }
    return result;
}

TextureBackendViewMappingResult map_texture_view_to_backend(
    const TextureDesc& texture, const TextureViewDesc& view, BackendApi backend,
    const TextureCapabilities& capabilities,
    const TextureBackendMappingOptions& options) {
    TextureBackendViewMappingResult result;
    const auto image = map_texture_to_backend(texture, backend, capabilities, options);
    result.diagnostics = image.diagnostics;
    if (!image) return result;

    auto viewTexture = texture;
    const auto requestedTextureFormat = requested_format(texture);
    const auto physicalViewFormat = image.plan.format;
    viewTexture.formatKind = physicalViewFormat;
    viewTexture.format = std::string(texture_format_name(physicalViewFormat));
    auto physicalView = view;
    if (physicalView.format == TextureFormat::Unknown || physicalView.format == requestedTextureFormat) {
        physicalView.format = physicalViewFormat;
    }
    const auto validation = validate_texture_view(viewTexture, physicalView, capabilities);
    if (!validation) {
        add_error(result.diagnostics, TextureBackendMappingCode::InvalidView, validation.message);
        return result;
    }

    const auto sourceDimension = logical_dimension(texture);
    const auto viewDimension = physicalView.dimension == TextureDimension::Auto
        ? sourceDimension : physicalView.dimension;
    const auto mipLevels = image.plan.mipLevels;
    const auto layers = image.plan.physicalArrayLayers;
    result.plan.backend = backend;
    result.plan.logicalDimension = viewDimension;
    result.plan.imageType = image.plan.imageType;
    result.plan.requestedFormat = view.format == TextureFormat::Unknown
        ? requestedTextureFormat : view.format;
    result.plan.format = physicalView.format;
    result.plan.aspect = physicalView.aspect == TextureAspect::Auto
        ? default_aspect(physicalView.format) : physicalView.aspect;
    result.plan.baseMipLevel = physicalView.baseMipLevel;
    result.plan.mipLevelCount = physicalView.mipLevelCount == 0
        ? mipLevels - physicalView.baseMipLevel : physicalView.mipLevelCount;
    result.plan.baseLayer = physicalView.baseLayer;
    result.plan.layerCount = physicalView.layerCount == 0
        ? layers - physicalView.baseLayer : physicalView.layerCount;
    result.plan.cubeCompatible = is_cube_dimension(viewDimension);
    result.plan.formatDowngraded = image.plan.formatDowngraded &&
        result.plan.requestedFormat != result.plan.format;
    return result;
}

TextureBackendMappingResult build_texture_backend_plan(
    const TextureDesc& description, BackendApi backend,
    const TextureCapabilities& capabilities,
    const TextureBackendMappingOptions& options) {
    return map_texture_to_backend(description, backend, capabilities, options);
}

TextureBackendViewMappingResult build_texture_view_backend_plan(
    const TextureDesc& texture, const TextureViewDesc& view, BackendApi backend,
    const TextureCapabilities& capabilities,
    const TextureBackendMappingOptions& options) {
    return map_texture_view_to_backend(texture, view, backend, capabilities, options);
}

} // namespace shinkou::render

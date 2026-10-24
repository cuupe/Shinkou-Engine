#pragma once

#include "shinkou/render/RenderBackend.h"
#include "shinkou/render/GpuMemoryAllocator.h"
#include "shinkou/render/QueueSync.h"
#include "shinkou/render/ShaderCompiler.h"
#include "shinkou/render/TextureBackendMapping.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace shinkou::render {
namespace {

inline const char* descriptor_type_name(DescriptorType type) {
    switch (type) {
    case DescriptorType::Texture: return "texture";
    case DescriptorType::Sampler: return "sampler";
    case DescriptorType::UniformBuffer: return "uniform_buffer";
    case DescriptorType::StorageBuffer: return "storage_buffer";
    case DescriptorType::StorageTexture: return "storage_texture";
    case DescriptorType::StructuredBuffer: return "structured_buffer";
    }
    return "unknown";
}

inline std::uint32_t descriptor_binding_resource_count(const DescriptorBinding& binding) noexcept {
    const auto count = binding.resources.empty()
        ? (binding.resource ? std::size_t{1} : std::size_t{0})
        : binding.resources.size();
    return count > std::numeric_limits<std::uint32_t>::max()
        ? 0u : static_cast<std::uint32_t>(count);
}

inline bool shader_binding_matches(const ShaderBinding& reflected, const DescriptorBinding& requested) {
    const auto resourceCount = descriptor_binding_resource_count(requested);
    return reflected.slot == requested.slot && reflected.space == requested.space &&
        reflected.type == descriptor_type_name(requested.type) &&
        reflected.unbounded == requested.unbounded &&
        (reflected.unbounded ? resourceCount != 0u : reflected.count == resourceCount);
}

inline bool validate_backend_shader_layout(const std::vector<ShaderBinding>& bindings, std::string& error) {
    std::vector<ShaderBinding> merged;
    if (!merge_shader_bindings(bindings, merged, error)) return false;
    for (const auto& binding : merged) {
        if (binding.count == 0 && !binding.unbounded) {
            error = "shader descriptor has an invalid zero-sized array";
            return false;
        }
    }
    return true;
}

inline std::uint64_t d3d12_binding_key(std::uint32_t space, std::uint32_t slot, std::uint32_t type) {
    return (static_cast<std::uint64_t>(space) << 32u) |
        (static_cast<std::uint64_t>(slot) << 3u) | static_cast<std::uint64_t>(type);
}

inline bool is_color_attachment_usage(ResourceUsage usage) {
    return usage >= ResourceUsage::ColorAttachment0 && usage <= ResourceUsage::ColorAttachment7;
}

inline bool is_supported_color_format(std::string_view format) {
    return format == "rgba8" || format == "bgra8" || format == "rgba16f" || format == "r32f";
}

inline bool is_supported_pipeline_depth_format(std::string_view format) {
    return format == "d24s8" || format == "d32f" || format == "none";
}

inline bool is_supported_sample_count(std::uint32_t sampleCount) {
    return sampleCount == 1 || sampleCount == 2 || sampleCount == 4 || sampleCount == 8;
}

inline bool resource_description_matches_kind(ResourceKind kind, const ResourceDesc& description) {
    switch (kind) {
    case ResourceKind::Texture2D:
    case ResourceKind::DepthStencil: return std::holds_alternative<TextureDesc>(description);
    case ResourceKind::Buffer: return std::holds_alternative<BufferDesc>(description);
    case ResourceKind::Shader: return std::holds_alternative<ShaderDesc>(description);
    case ResourceKind::Pipeline: return std::holds_alternative<PipelineDesc>(description);
    case ResourceKind::Material: return std::holds_alternative<MaterialDesc>(description);
    case ResourceKind::Sampler: return std::holds_alternative<SamplerDesc>(description);
    }
    return false;
}

inline bool prepare_backend_texture(const TextureDesc& description, BackendApi backend,
                             TexturePhysicalImagePlan& plan, std::string& error) {
    const auto mapped = map_texture_to_backend(description, backend,
                                                TextureCapabilities::for_backend(backend));
    if (!mapped) {
        error = mapped.error_summary();
        if (error.empty()) error = "texture backend mapping failed";
        return false;
    }
    if (mapped.plan.logicalDimension == TextureDimension::Texture3D || mapped.plan.cubeCompatible) {
        error = "texture dimension is valid but this backend's existing view/update path only supports 2D and 2D arrays";
        return false;
    }
    if (mapped.plan.formatDowngraded) {
        error = "texture format downgrade is disabled for native resource creation";
        return false;
    }
    if (mapped.plan.sampleCount > 1 &&
        (!description.initialData.empty() || !description.initialSubresources.empty())) {
        error = "multisample textures cannot use the existing CPU upload path";
        return false;
    }
    if (texture_format_is_compressed(mapped.plan.format) &&
        (!description.initialData.empty() || !description.initialSubresources.empty())) {
        error = "compressed textures cannot use the existing uncompressed CPU upload path";
        return false;
    }
    plan = mapped.plan;
    return true;
}

inline bool indirect_arguments_fit(std::size_t bufferSize, const IndirectMeshDraw& draw) noexcept {
    constexpr std::size_t commandBytes = sizeof(std::uint32_t) * 5u;
    // The backend contract uses the native five-DWORD indexed draw command.
    // D3D12 encodes the stride in the command signature, so accepting a
    // larger caller stride would make the three backends interpret the same
    // argument buffer differently.
    if (draw.maxDrawCount == 0 || draw.stride != commandBytes || draw.argumentOffset > bufferSize ||
        bufferSize - draw.argumentOffset < commandBytes) return false;
    const auto lastCommand = static_cast<std::size_t>(draw.maxDrawCount - 1u);
    const auto remaining = bufferSize - draw.argumentOffset - commandBytes;
    return lastCommand <= remaining / draw.stride;
}

struct DrawVertexBindings {
    std::array<VertexBufferBinding,16> values{};
    std::size_t count{0};
    auto begin() { return values.begin(); }
    auto end() { return values.begin()+count; }
    auto begin() const { return values.begin(); }
    auto end() const { return values.begin()+count; }
    auto& operator[](std::size_t index) { return values[index]; }
    std::size_t size() const { return count; }
    bool empty() const { return count==0; }
    void push_back(VertexBufferBinding value) { values[count++]=value; }
};
inline bool draw_vertex_bindings(const PipelineDesc& pipeline, ResourceHandle primary,
    const std::vector<VertexBufferBinding>& explicitBindings,
    DrawVertexBindings& result, std::string& error) {
    const auto& layout = pipeline.vertexLayout;
    if (explicitBindings.size()>16) { error="too many vertex streams"; return false; }
    for (const auto& binding:explicitBindings) result.push_back(binding);
    if (result.empty()) {
        if (layout.bindings.size() > 1) { error = "draw is missing vertex streams"; return false; }
        result.push_back({primary,layout.bindings.empty() ? 0u : layout.bindings.front().binding,0});
    }
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (!result[i].buffer || result[i].buffer.kind != ResourceKind::Buffer || result[i].binding >= 16 ||
            std::any_of(result.begin(),result.begin()+i,[&](const auto& item) { return item.binding == result[i].binding; })) {
            error = "draw has an invalid or duplicate vertex stream";
            return false;
        }
    }
    for (const auto& binding : layout.bindings) {
        if (std::none_of(result.begin(),result.end(),[&](const auto& item) { return item.binding == binding.binding; })) {
            error = "draw is missing a required vertex stream";
            return false;
        }
    }
    return true;
}

inline bool indexed_range_fits(std::size_t bufferSize, std::uint32_t firstIndex,
                        std::uint32_t indexCount) noexcept {
    constexpr std::size_t indexBytes = sizeof(std::uint32_t);
    if (indexCount == 0 || firstIndex > std::numeric_limits<std::size_t>::max() / indexBytes ||
        indexCount > std::numeric_limits<std::size_t>::max() / indexBytes) return false;
    const auto firstByte = static_cast<std::size_t>(firstIndex) * indexBytes;
    const auto countBytes = static_cast<std::size_t>(indexCount) * indexBytes;
    return firstByte <= bufferSize && countBytes <= bufferSize - firstByte;
}
}

}

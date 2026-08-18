#pragma once

#include "shinkou/Math.h"
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace shinkou::render {
enum class BackendApi { Null, DirectX11, DirectX12, Vulkan };
// Auto is only valid for a TextureViewDesc. Legacy TextureDesc values use
// Texture2D by default.
enum class TextureDimension { Auto, Texture2D, Texture3D, Cube, Texture2DArray, CubeArray };
enum class TextureFormat {
    Unknown,
    RGBA8Unorm,
    BGRA8Unorm,
    RGBA16Float,
    R32Float,
    D24UnormS8Uint,
    D32Float,
    BC1RGBAUnorm,
    BC3RGBAUnorm,
    BC5RGUnorm,
    BC6HUFloat,
    BC7RGBAUnorm
};
enum class TextureResolveMode { None, Source, Destination };
enum class RenderQueue { Graphics, Compute, Copy };
enum class ResourceKind { Texture2D, Buffer, DepthStencil, Shader, Pipeline, Material, Sampler };
enum class ShaderStage { Vertex, Fragment, Compute };
enum class ShaderSourceKind { Source, Dxbc, Dxil, SpirV };
enum class ResourceUsage {
    Unknown,
    ShaderRead,
    ShaderWrite,
    ColorAttachment,
    ColorAttachment0 = ColorAttachment,
    ColorAttachment1,
    ColorAttachment2,
    ColorAttachment3,
    ColorAttachment4,
    ColorAttachment5,
    ColorAttachment6,
    ColorAttachment7,
    DepthStencil,
    VertexBuffer,
    IndexBuffer,
    CopySource,
    CopyDestination,
    UniformBuffer,
    StorageRead,
    StorageWrite,
    IndirectArguments,
    Present
};

struct TextureDesc {
    std::uint32_t width{1};
    std::uint32_t height{1};
    std::uint32_t layers{1};
    std::uint32_t mipLevels{1};
    std::string format{"rgba8"};
    bool renderTarget{false};
    bool generateMips{false};
    std::vector<std::uint8_t> initialData;
    bool hdr{false};
    std::string colorSpace{"linear"};
    bool storage{false};
    struct SubresourceData {
        std::uint32_t mipLevel{0};
        std::uint32_t layer{0};
        std::uint32_t width{0};
        std::uint32_t height{0};
        std::size_t rowPitch{0};
        std::vector<std::uint8_t> data;
    };
    std::vector<SubresourceData> initialSubresources;
    bool retainCpuCopy{true};
    // Appended to preserve existing aggregate initialization.
    TextureDimension dimension{TextureDimension::Texture2D};
    std::uint32_t depth{1};
    TextureFormat formatKind{TextureFormat::Unknown};
    std::uint32_t sampleCount{1};
    bool resolve{false};
    TextureResolveMode resolveMode{TextureResolveMode::None};
    bool depthStencil{false};
};

struct SamplerDesc {
    std::string filter{"linear"};
    std::string addressU{"repeat"};
    std::string addressV{"repeat"};
    std::string addressW{"repeat"};
    float maxAnisotropy{1.0f};
    bool compareEnable{false};
};

struct BufferDesc {
    std::size_t size{0};
    std::size_t stride{0};
    bool vertexBuffer{false};
    bool indexBuffer{false};
    std::vector<std::uint8_t> initialData;
    bool indirectBuffer{false};
    bool structuredBuffer{false};
    bool storageBuffer{false};
    bool retainCpuCopy{true};
};

struct ShaderDesc {
    ShaderStage stage{ShaderStage::Vertex};
    std::string name;
    std::string source;
    std::string entryPoint{"main"};
    std::string profile;
    ShaderSourceKind sourceKind{ShaderSourceKind::Source};
    std::vector<std::uint8_t> bytecode;
    std::vector<std::string> defines;
    std::uint64_t revision{0};
};

struct ResourceHandle {
    std::uint32_t id{0};
    ResourceKind kind{ResourceKind::Buffer};
    explicit operator bool() const noexcept { return id != 0; }
};

struct ResourceAccess {
    ResourceHandle resource{};
    ResourceUsage usage{ResourceUsage::Unknown};
};

enum class DescriptorType { Texture, UniformBuffer, StorageBuffer, Sampler, StorageTexture, StructuredBuffer };

struct BindlessTableDesc {
    std::uint32_t capacity{1024};
    bool updateAfterBind{true};
    DescriptorType type{DescriptorType::Texture};
};

struct BindlessTableHandle {
    std::uint32_t id{0};
    std::uint32_t baseIndex{0};
    std::uint32_t capacity{0};
    explicit operator bool() const noexcept { return id != 0 && capacity != 0; }
};

struct DescriptorBinding {
    std::string name;
    ResourceHandle resource{};
    DescriptorType type{DescriptorType::Texture};
    std::uint32_t slot{0};
    std::uint32_t space{0};
    std::uint32_t count{1};
    bool unbounded{false};
    // Legacy bindings use resource. Array bindings provide one resource per
    // descriptor; the first entry is also accepted as the legacy resource.
    std::vector<ResourceHandle> resources;
};

struct MaterialDesc {
    std::string name;
    ResourceHandle pipeline{};
    std::vector<DescriptorBinding> bindings;
    bool bindless{false};
};

struct PipelineDesc {
    std::string name;
    std::uint32_t vertexShader{0};
    std::uint32_t fragmentShader{0};
    std::uint32_t computeShader{0};
    bool depthTest{true};
    bool depthWrite{true};
    bool alphaBlend{false};
    bool vertexInput{false};
    std::string cullMode{"back"};
    std::string fillMode{"solid"};
    std::string topology{"triangle"};
    std::string colorFormat{"rgba8"};
    std::string depthFormat{"d24s8"};
    std::uint32_t sampleCount{1};
    std::vector<std::string> colorFormats;
};

using ResourceDesc = std::variant<TextureDesc, BufferDesc, ShaderDesc, PipelineDesc, MaterialDesc, SamplerDesc>;

struct SpriteDraw {
    ResourceHandle texture{};
    math::Vec2 position{};
    math::Vec2 size{1, 1};
    float rotation{0};
};

struct MeshDraw {
    ResourceHandle vertexBuffer{};
    ResourceHandle indexBuffer{};
    std::uint32_t indexCount{0};
    math::Mat4 transform{math::Mat4::Identity()};
};

struct IndirectMeshDraw {
    ResourceHandle vertexBuffer{};
    ResourceHandle indexBuffer{};
    ResourceHandle argumentBuffer{};
    std::uint32_t maxDrawCount{1};
    std::uint32_t stride{sizeof(std::uint32_t) * 5};
    std::size_t argumentOffset{0};
};

struct DispatchDesc {
    std::uint32_t groupCountX{1};
    std::uint32_t groupCountY{1};
    std::uint32_t groupCountZ{1};
};

struct CameraData {
    math::Mat4 view{math::Mat4::Identity()};
    math::Mat4 projection{math::Mat4::Identity()};
};
}

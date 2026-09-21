#include "shinkou/editor/EditorModelPreviewRenderer.h"

#include "shinkou/render/Renderer.h"
#include "shinkou/ui/Render.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace shinkou::editor {
namespace {

constexpr std::size_t kMaxPreviewVertices = 2'000'000;
constexpr std::size_t kMaxPreviewIndices = 6'000'000;
constexpr std::uint32_t kMaxPreviewRenderTargetDimension = 4096u;

struct PreviewSceneFrame {
    math::Mat4 viewProjection{math::Mat4::Identity()};
    std::array<float, 4> cameraPositionAndFlags{};
};

struct PreviewMaterialFrame {
    std::array<float, 4> baseColor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic{0.0f};
    float roughness{1.0f};
    float normalMapEnabled{0.0f};
    float metallicRoughnessTextureEnabled{0.0f};
    std::array<float, 4> padding{};
};

struct PreviewVertex {
    math::Vec3 position{};
    math::Vec2 textureCoordinate{};
    math::Vec3 normal{0.0f, 0.0f, 1.0f};
};

static_assert(sizeof(PreviewSceneFrame) % 16u == 0u);
static_assert(sizeof(PreviewMaterialFrame) % 16u == 0u);
static_assert(sizeof(PreviewVertex) == sizeof(float) * 8u);

constexpr std::string_view kPreviewVertexShader = R"hlsl(
cbuffer SceneFrame : register(b2)
{
    float4x4 viewProjection;
    float4 cameraPositionAndFlags;
};

struct VertexInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL0;
    float3 worldPosition : TEXCOORD1;
};

VertexOutput main(VertexInput input)
{
    VertexOutput output;
    // Shinkou math uses column-major, column-vector transforms. Keep the
    // multiplication order explicit so the camera translation reaches clip
    // space on D3D11 instead of silently dropping the fourth matrix column.
    output.position = mul(viewProjection, float4(input.position, 1.0f));
    output.uv = input.uv;
    output.normal = input.normal;
    output.worldPosition = input.position;
    return output;
}
)hlsl";

constexpr std::string_view kPreviewFragmentShader = R"hlsl(
Texture2D BaseColorTexture : register(t0);
SamplerState BaseColorSampler : register(s1);
Texture2D NormalTexture : register(t5);
SamplerState NormalSampler : register(s6);
Texture2D MetallicRoughnessTexture : register(t7);
SamplerState MetallicRoughnessSampler : register(s8);

cbuffer SceneFrame : register(b2)
{
    float4x4 viewProjection;
    float4 cameraPositionAndFlags;
};

cbuffer PreviewMaterial : register(b4)
{
    float4 baseColor;
    float metallic;
    float roughness;
    float normalMapEnabled;
    float metallicRoughnessTextureEnabled;
    float4 materialPadding;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL0;
    float3 worldPosition : TEXCOORD1;
};

float4 main(FragmentInput input) : SV_Target
{
    const float3 lightDirection = normalize(float3(0.35, 0.65, 0.75));
    float3 normal = normalize(input.normal);
    if (normalMapEnabled > 0.5f)
    {
        const float3 positionDerivativeX = ddx(input.worldPosition);
        const float3 positionDerivativeY = ddy(input.worldPosition);
        const float2 uvDerivativeX = ddx(input.uv);
        const float2 uvDerivativeY = ddy(input.uv);
        float3 tangent = positionDerivativeX * uvDerivativeY.y - positionDerivativeY * uvDerivativeX.y;
        tangent = normalize(tangent - normal * dot(normal, tangent));
        const float3 bitangent = normalize(cross(normal, tangent));
        const float3 tangentNormal = normalize(
            NormalTexture.Sample(NormalSampler, saturate(input.uv)).xyz * 2.0f - 1.0f);
        normal = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y +
                           normal * tangentNormal.z);
    }
    float materialMetallic = saturate(metallic);
    float materialRoughness = saturate(roughness);
    if (metallicRoughnessTextureEnabled > 0.5f)
    {
        const float4 metallicRoughness = MetallicRoughnessTexture.Sample(
            MetallicRoughnessSampler, saturate(input.uv));
        materialMetallic = saturate(materialMetallic * metallicRoughness.b);
        materialRoughness = clamp(materialRoughness * metallicRoughness.g, 0.04f, 1.0f);
    }
    const float3 viewDirection = normalize(cameraPositionAndFlags.xyz - input.worldPosition);
    const float3 halfway = normalize(lightDirection + viewDirection);
    const float ndotl = saturate(dot(normal, lightDirection));
    const float diffuse = 0.18 + 0.82 * ndotl;
    const float specularPower = lerp(128.0, 4.0, materialRoughness);
    const float specular = pow(saturate(dot(normal, halfway)), specularPower) *
        lerp(0.04, 1.0, materialMetallic);
    const float3 albedo = baseColor.rgb * BaseColorTexture.Sample(BaseColorSampler, saturate(input.uv)).rgb;
    const float3 color = albedo * (0.08 + diffuse * (1.0 - materialMetallic)) + specular.xxx;
    return float4(color, baseColor.a);
}
)hlsl";

constexpr std::string_view kPreviewCompositeVertexShader = R"hlsl(
struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput main(uint vertexId : SV_VertexID)
{
    float2 position;
    float2 uv;
    if (vertexId == 0) { position = float2(-1.0f, -1.0f); uv = float2(0.0f, 1.0f); }
    else if (vertexId == 1) { position = float2(-1.0f, 1.0f); uv = float2(0.0f, 0.0f); }
    else if (vertexId == 2) { position = float2(1.0f, 1.0f); uv = float2(1.0f, 0.0f); }
    else if (vertexId == 3) { position = float2(-1.0f, -1.0f); uv = float2(0.0f, 1.0f); }
    else if (vertexId == 4) { position = float2(1.0f, 1.0f); uv = float2(1.0f, 0.0f); }
    else { position = float2(1.0f, -1.0f); uv = float2(1.0f, 1.0f); }
    VertexOutput output;
    output.position = float4(position, 0.0f, 1.0f);
    output.uv = uv;
    return output;
}
)hlsl";

constexpr std::string_view kPreviewCompositeFragmentShader = R"hlsl(
Texture2D SourceTexture : register(t0);
SamplerState SourceSampler : register(s1);

struct FragmentInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(FragmentInput input) : SV_Target
{
    return SourceTexture.Sample(SourceSampler, saturate(input.uv));
}
)hlsl";

bool finite_vec3(const math::Vec3& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

std::uint64_t mix_key(std::uint64_t value, std::uint32_t input) noexcept {
    value ^= static_cast<std::uint64_t>(input) + 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
    return value;
}

std::uint32_t float_bits(float value) noexcept {
    std::uint32_t result = 0;
    static_assert(sizeof(result) == sizeof(value));
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

std::uint64_t projection_key(const EditorModelPreviewSnapshot& snapshot,
                             const EditorModelPreviewSceneState& scene,
                             const render::EditorViewportRect& viewport) noexcept {
    auto result = mix_key(0xcbf29ce484222325ull,
                          static_cast<std::uint32_t>(snapshot.revision ^ (snapshot.revision >> 32u)));
    result = mix_key(result, static_cast<std::uint32_t>(scene.projectionRevision ^ (scene.projectionRevision >> 32u)));
    result = mix_key(result, float_bits(viewport.width));
    result = mix_key(result, float_bits(viewport.height));
    return result == 0 ? 1 : result;
}

std::string backend_label(render::BackendApi api) {
    switch (api) {
    case render::BackendApi::DirectX11: return "D3D11";
    case render::BackendApi::DirectX12: return "D3D12";
    case render::BackendApi::Vulkan: return "Vulkan";
    case render::BackendApi::Null: break;
    }
    return "Null";
}

bool valid_buffer_description(const render::Renderer& renderer, render::ResourceHandle handle,
                              std::size_t bytes, bool vertex, bool index) noexcept {
    const auto* description = renderer.resource_description(handle);
    const auto* buffer = description ? std::get_if<render::BufferDesc>(description) : nullptr;
    return buffer && buffer->size >= bytes && buffer->vertexBuffer == vertex && buffer->indexBuffer == index;
}

std::uint32_t preview_target_dimension(float value) noexcept {
    if (!std::isfinite(value) || value <= 0.0f) return 0;
    const auto rounded = static_cast<long long>(std::ceil(value));
    if (rounded <= 0 || rounded > static_cast<long long>(kMaxPreviewRenderTargetDimension)) return 0;
    return static_cast<std::uint32_t>(rounded);
}

std::vector<std::uint8_t> copy_bytes(const void* source, std::size_t bytes) {
    std::vector<std::uint8_t> result(bytes);
    if (bytes != 0) std::memcpy(result.data(), source, bytes);
    return result;
}

PreviewSceneFrame make_scene_frame(const EditorModelPreviewSnapshot& snapshot,
                                   const EditorModelPreviewSceneState& scene,
                                   const render::EditorViewportRect& viewport) noexcept {
    PreviewSceneFrame frame;
    const math::Vec3 center{
        (snapshot.minX + snapshot.maxX) * 0.5f,
        (snapshot.minY + snapshot.maxY) * 0.5f,
        (snapshot.minZ + snapshot.maxZ) * 0.5f};
    const math::Vec3 extent{snapshot.maxX - snapshot.minX,
                            snapshot.maxY - snapshot.minY,
                            snapshot.maxZ - snapshot.minZ};
    const float radius = std::max(1.0e-4f, 0.5f * math::Length(extent));
    const auto rotation = math::FromAxisAngle({0.0f, 1.0f, 0.0f}, scene.camera.yaw) *
        math::FromAxisAngle({1.0f, 0.0f, 0.0f}, scene.camera.pitch);
    const float distance = std::max(0.75f, scene.camera.distance) * radius;
    const auto forward = math::Normalize(math::Rotate(rotation, {0.0f, 0.0f, 1.0f}));
    const auto up = math::Normalize(math::Rotate(rotation, {0.0f, 1.0f, 0.0f}));
    const auto eye = center - forward * distance;
    const float aspect = std::max(1.0e-4f, viewport.width / std::max(1.0f, viewport.height));
    const float nearPlane = std::max(0.001f, radius * 0.01f);
    const float farPlane = std::max(100.0f, distance + radius * 8.0f);
    frame.viewProjection = math::Multiply(math::Perspective(0.85f, aspect, nearPlane, farPlane),
                                          math::LookAt(eye, center, up));
    frame.cameraPositionAndFlags = {eye.x, eye.y, eye.z, 0.0f};
    return frame;
}

PreviewMaterialFrame make_material_frame(const EditorModelPreviewSnapshot& snapshot,
                                         std::int32_t materialIndex) noexcept {
    PreviewMaterialFrame frame;
    if (!snapshot.materials || materialIndex < 0 ||
        static_cast<std::size_t>(materialIndex) >= snapshot.materials->size()) return frame;
    const auto& material = (*snapshot.materials)[static_cast<std::size_t>(materialIndex)];
    const auto& source = material.baseColorFactor;
    for (std::size_t index = 0; index < frame.baseColor.size(); ++index)
        if (math::IsFinite(source[index])) frame.baseColor[index] = std::clamp(source[index], 0.0f, 1.0f);
    if (math::IsFinite(material.metallicFactor)) frame.metallic = std::clamp(material.metallicFactor, 0.0f, 1.0f);
    if (math::IsFinite(material.roughnessFactor)) frame.roughness = std::clamp(material.roughnessFactor, 0.04f, 1.0f);
    return frame;
}

} // namespace

void EditorModelPreviewRenderer::clear_pipeline(render::Renderer& renderer) noexcept {
    if (compositeMaterial_) renderer.destroy_resource(compositeMaterial_);
    if (compositePipeline_) renderer.destroy_resource(compositePipeline_);
    if (compositeFragmentShader_) renderer.destroy_resource(compositeFragmentShader_);
    if (compositeVertexShader_) renderer.destroy_resource(compositeVertexShader_);
    if (material_) renderer.destroy_resource(material_);
    if (pipeline_) renderer.destroy_resource(pipeline_);
    if (fragmentShader_) renderer.destroy_resource(fragmentShader_);
    if (vertexShader_) renderer.destroy_resource(vertexShader_);
    material_ = {};
    compositeMaterial_ = {};
    compositePipeline_ = {};
    compositeFragmentShader_ = {};
    compositeVertexShader_ = {};
    compositeTextureId_ = 0;
    compositeSamplerId_ = 0;
    materialTextureId_ = 0;
    materialSamplerId_ = 0;
    materialNormalTextureId_ = 0;
    normalSamplerId_ = 0;
    materialMetallicRoughnessTextureId_ = 0;
    metallicRoughnessSamplerId_ = 0;
    pipeline_ = {};
    fragmentShader_ = {};
    vertexShader_ = {};
    pipelineColorFormat_.clear();
}

void EditorModelPreviewRenderer::clear(render::Renderer& renderer) noexcept {
    clear_pipeline(renderer);
    if (baseColorSampler_) renderer.destroy_resource(baseColorSampler_);
    if (baseColorTexture_) renderer.destroy_resource(baseColorTexture_);
    if (normalSampler_) renderer.destroy_resource(normalSampler_);
    if (normalTexture_) renderer.destroy_resource(normalTexture_);
    if (metallicRoughnessSampler_) renderer.destroy_resource(metallicRoughnessSampler_);
    if (metallicRoughnessTexture_) renderer.destroy_resource(metallicRoughnessTexture_);
    if (offscreenDepth_) renderer.destroy_resource(offscreenDepth_);
    if (offscreenColor_) renderer.destroy_resource(offscreenColor_);
    if (materialBuffer_) renderer.destroy_resource(materialBuffer_);
    if (sceneBuffer_) renderer.destroy_resource(sceneBuffer_);
    if (indexBuffer_) renderer.destroy_resource(indexBuffer_);
    if (vertexBuffer_) renderer.destroy_resource(vertexBuffer_);
    materialBuffer_ = {};
    baseColorSampler_ = {};
    baseColorTexture_ = {};
    normalSampler_ = {};
    normalTexture_ = {};
    metallicRoughnessSampler_ = {};
    metallicRoughnessTexture_ = {};
    offscreenDepth_ = {};
    offscreenColor_ = {};
    sceneBuffer_ = {};
    indexBuffer_ = {};
    vertexBuffer_ = {};
    vertexBytes_ = 0;
    indexBytes_ = 0;
    geometryRevision_ = 0;
    projectionKey_ = 0;
    materialRevision_ = 0;
    materialIndex_ = -2;
    materialTextureRole_ = EditorModelPreviewTextureRole::BaseColor;
    textureRevision_ = 0;
    normalTextureRevision_ = 0;
    metallicRoughnessTextureRevision_ = 0;
    actualBaseColorTexture_ = false;
    actualNormalTexture_ = false;
    actualMetallicRoughnessTexture_ = false;
    offscreenWidth_ = 0;
    offscreenHeight_ = 0;
    offscreenColorFormat_.clear();
    offscreenDepthFormat_.clear();
}

EditorModelPreviewRenderState EditorModelPreviewRenderer::render(
    render::Renderer& renderer,
    const EditorModelPreviewSnapshot& snapshot,
    const EditorModelPreviewSceneState& scene,
    std::int32_t materialIndex,
    std::shared_ptr<const ui::UiImageSnapshot> textureSnapshot,
    EditorModelPreviewTextureRole textureRole) {
    EditorModelPreviewRenderState state;
    const auto capabilities = renderer.capabilities();
    state.backend = capabilities.api;
    state.vertexCount = snapshot.vertexCount;
    state.triangleCount = snapshot.triangleCount;
    state.geometryRevision = snapshot.revision;
    state.projectionRevision = scene.projectionRevision;

    if (!snapshot.valid() || !scene.valid()) {
        state.status = "GPU model preview unavailable: model geometry or camera state is invalid";
        return state;
    }
    if (!capabilities.deviceReady) {
        state.status = "GPU model preview unavailable: " + backend_label(capabilities.api) + " device is not ready";
        return state;
    }
    if (!capabilities.supportsEditorModelRendering) {
        state.status = "GPU model preview fallback: " + backend_label(capabilities.api) +
            " shader path is pending; retained/WIC preview remains active";
        return state;
    }
    if (!capabilities.supportsEditorViewportScissor || !renderer.editor_viewport().enabled ||
        !renderer.editor_viewport().viewport.valid()) {
        state.status = "GPU model preview fallback: editor viewport seam is unavailable";
        return state;
    }
    if (!capabilities.supportsEditorOffscreenTarget) {
        state.status = "GPU model preview fallback: editor offscreen target path is unavailable";
        return state;
    }
    const auto& viewport = renderer.editor_viewport().viewport;
    const auto targetWidth = preview_target_dimension(viewport.width);
    const auto targetHeight = preview_target_dimension(viewport.height);
    if (targetWidth == 0 || targetHeight == 0) {
        state.status = "GPU model preview rejected: editor viewport exceeds the bounded offscreen target limit";
        return state;
    }
    // D3D11's editor swapchain is BGRA8. The preview target deliberately uses
    // the same format so the pass-local composite has no format conversion or
    // implicit scene-target dependency.
    const std::string colorFormat = "bgra8";
    const auto* targetDescription = renderer.resource_description(offscreenColor_);
    const auto* targetTexture = targetDescription ? std::get_if<render::TextureDesc>(targetDescription) : nullptr;
    const bool targetCompatible = targetTexture && targetTexture->width == targetWidth &&
        targetTexture->height == targetHeight && targetTexture->format == colorFormat &&
        targetTexture->renderTarget;
    if (!targetCompatible) {
        if (offscreenColor_) renderer.destroy_resource(offscreenColor_);
        render::TextureDesc description;
        description.width = targetWidth;
        description.height = targetHeight;
        description.format = colorFormat;
        description.renderTarget = true;
        description.retainCpuCopy = false;
        offscreenColor_ = renderer.create_texture(description);
        offscreenWidth_ = offscreenColor_ ? targetWidth : 0;
        offscreenHeight_ = offscreenColor_ ? targetHeight : 0;
        offscreenColorFormat_ = offscreenColor_ ? colorFormat : std::string{};
    }
    if (!offscreenColor_) {
        state.status = "GPU model preview offscreen target creation failed: " + renderer.last_error();
        return state;
    }
    state.offscreenTargetReady = true;
    const std::string depthFormat = "d24s8";
    const auto* depthDescription = renderer.resource_description(offscreenDepth_);
    const auto* depthTexture = depthDescription ? std::get_if<render::TextureDesc>(depthDescription) : nullptr;
    const bool depthCompatible = offscreenDepth_ && depthTexture &&
        depthTexture->width == targetWidth && depthTexture->height == targetHeight &&
        depthTexture->format == depthFormat && depthTexture->depthStencil;
    if (!depthCompatible) {
        if (offscreenDepth_) renderer.destroy_resource(offscreenDepth_);
        render::TextureDesc description;
        description.width = targetWidth;
        description.height = targetHeight;
        description.format = depthFormat;
        description.retainCpuCopy = false;
        description.depthStencil = true;
        offscreenDepth_ = renderer.create_depth_stencil(description);
        offscreenDepthFormat_ = offscreenDepth_ ? depthFormat : std::string{};
    }
    if (!offscreenDepth_) {
        state.status = "GPU model preview depth target creation failed: " + renderer.last_error();
        return state;
    }
    state.depthTargetReady = true;

    if (snapshot.vertices->size() > kMaxPreviewVertices || snapshot.indices->size() > kMaxPreviewIndices ||
        snapshot.vertices->size() > std::numeric_limits<std::size_t>::max() / sizeof(math::Vec3) ||
        snapshot.indices->size() > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t)) {
        state.status = "GPU model preview rejected: geometry exceeds the bounded upload limit";
        return state;
    }
    for (const auto& vertex : *snapshot.vertices) {
        if (!finite_vec3(vertex)) {
            state.status = "GPU model preview rejected: geometry contains non-finite vertices";
            return state;
        }
    }
    for (const auto index : *snapshot.indices) {
        if (index >= snapshot.vertices->size()) {
            state.status = "GPU model preview rejected: index buffer references a vertex outside the snapshot";
            return state;
        }
    }

    std::vector<PreviewVertex> previewVertices;
    previewVertices.reserve(snapshot.vertices->size());
    const bool hasTextureCoordinates = snapshot.textureCoordinates &&
        snapshot.textureCoordinates->size() == snapshot.vertices->size();
    const bool hasNormals = snapshot.normals && snapshot.normals->size() == snapshot.vertices->size();
    for (std::size_t index = 0; index < snapshot.vertices->size(); ++index) {
        const auto& position = (*snapshot.vertices)[index];
        const auto fallback = math::Vec2{position.x * 0.5f + 0.5f, position.y * 0.5f + 0.5f};
        const auto coordinate = hasTextureCoordinates ? (*snapshot.textureCoordinates)[index] : fallback;
        if (!math::IsFinite(coordinate.x) || !math::IsFinite(coordinate.y)) {
            state.status = "GPU model preview rejected: texture coordinates contain a non-finite value";
            return state;
        }
        const auto normal = hasNormals ? (*snapshot.normals)[index] : math::Vec3{0.0f, 0.0f, 1.0f};
        if (!math::IsFinite(normal.x) || !math::IsFinite(normal.y) || !math::IsFinite(normal.z) ||
            math::LengthSquared(normal) <= math::Epsilon) {
            state.status = "GPU model preview rejected: normals contain an invalid vector";
            return state;
        }
        previewVertices.push_back({position, coordinate, math::Normalize(normal)});
    }
    const auto vertexBytes = previewVertices.size() * sizeof(PreviewVertex);
    const auto indexBytes = snapshot.indices->size() * sizeof(std::uint32_t);
    const auto vertexData = copy_bytes(previewVertices.data(), vertexBytes);
    const auto indexData = copy_bytes(snapshot.indices->data(), indexBytes);
    if (!valid_buffer_description(renderer, vertexBuffer_, vertexBytes, true, false)) {
        if (vertexBuffer_) renderer.destroy_resource(vertexBuffer_);
        render::BufferDesc description;
        description.size = vertexBytes;
        description.stride = sizeof(PreviewVertex);
        description.vertexBuffer = true;
        description.initialData = vertexData;
        vertexBuffer_ = renderer.create_buffer(description);
        vertexBytes_ = vertexBuffer_ ? vertexBytes : 0;
        geometryRevision_ = 0;
    } else if (geometryRevision_ != snapshot.revision) {
        if (!renderer.update_buffer({vertexBuffer_, 0, vertexData})) {
            state.status = "GPU model preview upload failed for vertices: " + renderer.last_error();
            return state;
        }
    }
    if (!vertexBuffer_) {
        state.status = "GPU model preview upload failed for vertices: " + renderer.last_error();
        return state;
    }
    if (!valid_buffer_description(renderer, indexBuffer_, indexBytes, false, true)) {
        if (indexBuffer_) renderer.destroy_resource(indexBuffer_);
        render::BufferDesc description;
        description.size = indexBytes;
        description.stride = sizeof(std::uint32_t);
        description.indexBuffer = true;
        description.initialData = indexData;
        indexBuffer_ = renderer.create_buffer(description);
        indexBytes_ = indexBuffer_ ? indexBytes : 0;
        geometryRevision_ = 0;
    } else if (geometryRevision_ != snapshot.revision) {
        if (!renderer.update_buffer({indexBuffer_, 0, indexData})) {
            state.status = "GPU model preview upload failed for indices: " + renderer.last_error();
            return state;
        }
    }
    if (!indexBuffer_) {
        state.status = "GPU model preview upload failed for indices: " + renderer.last_error();
        return state;
    }
    geometryRevision_ = snapshot.revision;
    state.geometryUploaded = true;
    state.textureCoordinatesApplied = hasTextureCoordinates;
    state.normalsApplied = hasNormals;

    const auto sceneFrame = make_scene_frame(snapshot, scene, renderer.editor_viewport().viewport);
    const bool hasMaterial = snapshot.materials && materialIndex >= 0 &&
        static_cast<std::size_t>(materialIndex) < snapshot.materials->size();
    const bool hasTextureSnapshot = textureSnapshot && textureSnapshot->valid() &&
        textureSnapshot->width <= 512u && textureSnapshot->height <= 512u;
    const bool useBaseColorTexture = textureRole == EditorModelPreviewTextureRole::BaseColor &&
        hasTextureSnapshot;
    const bool useNormalTexture = textureRole == EditorModelPreviewTextureRole::Normal &&
        hasTextureSnapshot;
    const bool useMetallicRoughnessTexture =
        textureRole == EditorModelPreviewTextureRole::MetallicRoughness && hasTextureSnapshot;
    auto materialFrame = make_material_frame(snapshot, materialIndex);
    materialFrame.normalMapEnabled = useNormalTexture ? 1.0f : 0.0f;
    materialFrame.metallicRoughnessTextureEnabled = useMetallicRoughnessTexture ? 1.0f : 0.0f;
    const auto projectionKey = projection_key(snapshot, scene, renderer.editor_viewport().viewport);
    if (!valid_buffer_description(renderer, sceneBuffer_, sizeof(sceneFrame), false, false)) {
        if (sceneBuffer_) renderer.destroy_resource(sceneBuffer_);
        render::BufferDesc description;
        description.size = sizeof(sceneFrame);
        description.uniformBuffer = true;
        description.initialData = copy_bytes(&sceneFrame, sizeof(sceneFrame));
        sceneBuffer_ = renderer.create_buffer(description);
        projectionKey_ = 0;
    }
    if (!sceneBuffer_) {
        state.status = "GPU model preview upload failed for camera frame: " + renderer.last_error();
        return state;
    }
    if (projectionKey_ != projectionKey) {
        if (!renderer.update_buffer({sceneBuffer_, 0, copy_bytes(&sceneFrame, sizeof(sceneFrame))})) {
            state.status = "GPU model preview upload failed for camera frame: " + renderer.last_error();
            return state;
        }
        projectionKey_ = projectionKey;
    }
    if (!valid_buffer_description(renderer, materialBuffer_, sizeof(materialFrame), false, false)) {
        if (materialBuffer_) renderer.destroy_resource(materialBuffer_);
        render::BufferDesc description;
        description.size = sizeof(materialFrame);
        description.uniformBuffer = true;
        description.initialData = copy_bytes(&materialFrame, sizeof(materialFrame));
        materialBuffer_ = renderer.create_buffer(description);
        materialRevision_ = 0;
        materialIndex_ = -2;
        materialTextureRole_ = EditorModelPreviewTextureRole::BaseColor;
    }
    if (!materialBuffer_) {
        state.status = "GPU model preview upload failed for material frame: " + renderer.last_error();
        return state;
    }
    if (materialRevision_ != snapshot.revision || materialIndex_ != materialIndex ||
        materialTextureRole_ != textureRole) {
        if (!renderer.update_buffer({materialBuffer_, 0, copy_bytes(&materialFrame, sizeof(materialFrame))})) {
            state.status = "GPU model preview upload failed for material frame: " + renderer.last_error();
            return state;
        }
        materialRevision_ = snapshot.revision;
        materialIndex_ = materialIndex;
        materialTextureRole_ = textureRole;
    }

    const std::vector<std::uint8_t> fallbackBaseColor{255u, 255u, 255u, 255u};
    // UiImageSnapshot stores BGRA bytes. A flat tangent-space normal is RGB
    // (128, 128, 255), therefore its BGRA representation is (255, 128, 128).
    const std::vector<std::uint8_t> fallbackNormal{255u, 128u, 128u, 255u};
    const auto& baseColorBytes = useBaseColorTexture ? textureSnapshot->bgraPremultiplied : fallbackBaseColor;
    const auto& normalBytes = useNormalTexture ? textureSnapshot->bgraPremultiplied : fallbackNormal;
    const auto& metallicRoughnessBytes =
        useMetallicRoughnessTexture ? textureSnapshot->bgraPremultiplied : fallbackBaseColor;
    if (baseColorBytes.size() > 512u * 512u * 4u || normalBytes.size() > 512u * 512u * 4u ||
        metallicRoughnessBytes.size() > 512u * 512u * 4u) {
        state.status = "GPU model preview texture rejected: decoded image exceeds the 512x512 upload limit";
        return state;
    }
    const auto upload_texture = [&](render::ResourceHandle& handle, std::uint64_t& revision,
                                    const std::vector<std::uint8_t>& bytes, std::uint32_t width,
                                    std::uint32_t height, std::uint64_t wantedRevision,
                                    const char* label) {
        const auto* description = renderer.resource_description(handle);
        const auto* existing = description ? std::get_if<render::TextureDesc>(description) : nullptr;
        const bool compatible = existing && existing->width == width && existing->height == height &&
            existing->format == "bgra8" && !existing->renderTarget;
        if (!compatible) {
            if (handle) renderer.destroy_resource(handle);
            render::TextureDesc textureDescription;
            textureDescription.width = width;
            textureDescription.height = height;
            textureDescription.format = "bgra8";
            textureDescription.initialData = bytes;
            textureDescription.retainCpuCopy = true;
            handle = renderer.create_texture(textureDescription);
            revision = 0;
        }
        if (!handle) {
            state.status = std::string("GPU model preview ") + label + " texture creation failed: " +
                renderer.last_error();
            return false;
        }
        if (revision != wantedRevision) {
            if (!renderer.update_texture({handle, 0, 0, width, height,
                                          static_cast<std::size_t>(width) * 4u, bytes})) {
                state.status = std::string("GPU model preview ") + label + " texture upload failed: " +
                    renderer.last_error();
                return false;
            }
            revision = wantedRevision;
        }
        return true;
    };
    const auto textureWidth = useBaseColorTexture ? textureSnapshot->width : 1u;
    const auto textureHeight = useBaseColorTexture ? textureSnapshot->height : 1u;
    const auto normalWidth = useNormalTexture ? textureSnapshot->width : 1u;
    const auto normalHeight = useNormalTexture ? textureSnapshot->height : 1u;
    const auto metallicRoughnessWidth = useMetallicRoughnessTexture ? textureSnapshot->width : 1u;
    const auto metallicRoughnessHeight = useMetallicRoughnessTexture ? textureSnapshot->height : 1u;
    if (!upload_texture(baseColorTexture_, textureRevision_, baseColorBytes, textureWidth, textureHeight,
                        useBaseColorTexture ? textureSnapshot->revision : 0u, "base color") ||
        !upload_texture(normalTexture_, normalTextureRevision_, normalBytes, normalWidth, normalHeight,
                        useNormalTexture ? textureSnapshot->revision : 0u, "normal") ||
        !upload_texture(metallicRoughnessTexture_, metallicRoughnessTextureRevision_,
                        metallicRoughnessBytes, metallicRoughnessWidth, metallicRoughnessHeight,
                        useMetallicRoughnessTexture ? textureSnapshot->revision : 0u,
                        "metallic/roughness")) return state;
    if (!baseColorSampler_) {
        render::SamplerDesc samplerDescription;
        samplerDescription.filter = "linear";
        samplerDescription.addressU = "clamp";
        samplerDescription.addressV = "clamp";
        samplerDescription.addressW = "clamp";
        baseColorSampler_ = renderer.create_sampler(samplerDescription);
    }
    if (!baseColorSampler_) {
        state.status = "GPU model preview sampler creation failed: " + renderer.last_error();
        return state;
    }
    if (!normalSampler_) {
        render::SamplerDesc samplerDescription;
        samplerDescription.filter = "linear";
        samplerDescription.addressU = "clamp";
        samplerDescription.addressV = "clamp";
        samplerDescription.addressW = "clamp";
        normalSampler_ = renderer.create_sampler(samplerDescription);
    }
    if (!normalSampler_) {
        state.status = "GPU model preview normal sampler creation failed: " + renderer.last_error();
        return state;
    }
    if (!metallicRoughnessSampler_) {
        render::SamplerDesc samplerDescription;
        samplerDescription.filter = "linear";
        samplerDescription.addressU = "clamp";
        samplerDescription.addressV = "clamp";
        samplerDescription.addressW = "clamp";
        metallicRoughnessSampler_ = renderer.create_sampler(samplerDescription);
    }
    if (!metallicRoughnessSampler_) {
        state.status = "GPU model preview metallic/roughness sampler creation failed: " + renderer.last_error();
        return state;
    }
    actualBaseColorTexture_ = useBaseColorTexture;
    actualNormalTexture_ = useNormalTexture;
    actualMetallicRoughnessTexture_ = useMetallicRoughnessTexture;

    if (!pipeline_ || pipelineColorFormat_ != colorFormat) {
        clear_pipeline(renderer);
        render::ShaderDesc vertexDescription;
        vertexDescription.stage = render::ShaderStage::Vertex;
        vertexDescription.name = "editor_model_preview_vertex";
        vertexDescription.source = std::string(kPreviewVertexShader);
        vertexDescription.profile = "vs_5_0";
        vertexDescription.sourceKind = render::ShaderSourceKind::Source;
        vertexDescription.revision = 1;
        vertexShader_ = renderer.create_shader(vertexDescription);
        render::ShaderDesc fragmentDescription;
        fragmentDescription.stage = render::ShaderStage::Fragment;
        fragmentDescription.name = "editor_model_preview_fragment";
        fragmentDescription.source = std::string(kPreviewFragmentShader);
        fragmentDescription.profile = "ps_5_0";
        fragmentDescription.sourceKind = render::ShaderSourceKind::Source;
        fragmentDescription.revision = 1;
        fragmentShader_ = renderer.create_shader(fragmentDescription);
        if (!vertexShader_ || !fragmentShader_) {
            state.status = "GPU model preview shader creation failed: " + renderer.last_error();
            clear_pipeline(renderer);
            return state;
        }
        render::PipelineDesc pipelineDescription;
        pipelineDescription.name = "editor_model_preview_pipeline_" + colorFormat;
        pipelineDescription.vertexShader = vertexShader_.id;
        pipelineDescription.fragmentShader = fragmentShader_.id;
        pipelineDescription.depthTest = true;
        pipelineDescription.depthWrite = true;
        pipelineDescription.vertexInput = true;
        pipelineDescription.vertexTextureCoordinates = true;
        pipelineDescription.vertexNormals = true;
        // Imported preview meshes may have either winding (and the Inspector
        // is a two-sided inspection surface). Avoid dropping the entire
        // asset when a provider's winding is not yet normalized.
        pipelineDescription.cullMode = "none";
        pipelineDescription.fillMode = "solid";
        pipelineDescription.topology = "triangle";
        pipelineDescription.colorFormat = colorFormat;
        pipelineDescription.depthFormat = depthFormat;
        pipeline_ = renderer.create_pipeline(pipelineDescription);
        if (!pipeline_) {
            state.status = "GPU model preview pipeline creation failed: " + renderer.last_error();
            clear_pipeline(renderer);
            return state;
        }
        render::ShaderDesc compositeVertexDescription;
        compositeVertexDescription.stage = render::ShaderStage::Vertex;
        compositeVertexDescription.name = "editor_model_preview_composite_vertex";
        compositeVertexDescription.source = std::string(kPreviewCompositeVertexShader);
        compositeVertexDescription.profile = "vs_5_0";
        compositeVertexDescription.sourceKind = render::ShaderSourceKind::Source;
        compositeVertexDescription.revision = 1;
        compositeVertexShader_ = renderer.create_shader(compositeVertexDescription);
        render::ShaderDesc compositeFragmentDescription;
        compositeFragmentDescription.stage = render::ShaderStage::Fragment;
        compositeFragmentDescription.name = "editor_model_preview_composite_fragment";
        compositeFragmentDescription.source = std::string(kPreviewCompositeFragmentShader);
        compositeFragmentDescription.profile = "ps_5_0";
        compositeFragmentDescription.sourceKind = render::ShaderSourceKind::Source;
        compositeFragmentDescription.revision = 1;
        compositeFragmentShader_ = renderer.create_shader(compositeFragmentDescription);
        if (!compositeVertexShader_ || !compositeFragmentShader_) {
            state.status = "GPU model preview composite shader creation failed: " + renderer.last_error();
            clear_pipeline(renderer);
            return state;
        }
        render::PipelineDesc compositePipelineDescription;
        compositePipelineDescription.name = "editor_model_preview_composite_pipeline_" + colorFormat;
        compositePipelineDescription.vertexShader = compositeVertexShader_.id;
        compositePipelineDescription.fragmentShader = compositeFragmentShader_.id;
        compositePipelineDescription.depthTest = false;
        compositePipelineDescription.depthWrite = false;
        compositePipelineDescription.vertexInput = false;
        compositePipelineDescription.cullMode = "none";
        compositePipelineDescription.fillMode = "solid";
        compositePipelineDescription.topology = "triangle";
        compositePipelineDescription.colorFormat = colorFormat;
        compositePipelineDescription.depthFormat = "none";
        compositePipeline_ = renderer.create_pipeline(compositePipelineDescription);
        if (!compositePipeline_) {
            state.status = "GPU model preview composite pipeline creation failed: " + renderer.last_error();
            clear_pipeline(renderer);
            return state;
        }
        pipelineColorFormat_ = colorFormat;
    }

    if (!material_ || materialTextureId_ != baseColorTexture_.id || materialSamplerId_ != baseColorSampler_.id ||
        materialNormalTextureId_ != normalTexture_.id || normalSamplerId_ != normalSampler_.id ||
        materialMetallicRoughnessTextureId_ != metallicRoughnessTexture_.id ||
        metallicRoughnessSamplerId_ != metallicRoughnessSampler_.id) {
        if (material_) renderer.destroy_resource(material_);
        material_ = {};
        render::MaterialDesc materialDescription;
        materialDescription.name = "editor_model_preview_material";
        materialDescription.pipeline = pipeline_;
        materialDescription.bindings = {
            {"BaseColorTexture", baseColorTexture_, render::DescriptorType::Texture, 0, 0, 1, false, {}},
            {"BaseColorSampler", baseColorSampler_, render::DescriptorType::Sampler, 1, 0, 1, false, {}},
            {"NormalTexture", normalTexture_, render::DescriptorType::Texture, 5, 0, 1, false, {}},
            {"NormalSampler", normalSampler_, render::DescriptorType::Sampler, 6, 0, 1, false, {}},
            {"MetallicRoughnessTexture", metallicRoughnessTexture_, render::DescriptorType::Texture, 7, 0, 1, false, {}},
            {"MetallicRoughnessSampler", metallicRoughnessSampler_, render::DescriptorType::Sampler, 8, 0, 1, false, {}},
            {"SceneFrame", sceneBuffer_, render::DescriptorType::UniformBuffer, 2, 0, 1, false, {}},
            {"PreviewMaterial", materialBuffer_, render::DescriptorType::UniformBuffer, 4, 0, 1, false, {}}};
        material_ = renderer.create_material(materialDescription);
        if (!material_) {
            state.status = "GPU model preview material creation failed: " + renderer.last_error();
            clear_pipeline(renderer);
            return state;
        }
        materialTextureId_ = baseColorTexture_.id;
        materialSamplerId_ = baseColorSampler_.id;
        materialNormalTextureId_ = normalTexture_.id;
        normalSamplerId_ = normalSampler_.id;
        materialMetallicRoughnessTextureId_ = metallicRoughnessTexture_.id;
        metallicRoughnessSamplerId_ = metallicRoughnessSampler_.id;
    }
    if (!compositeMaterial_ || compositeTextureId_ != offscreenColor_.id ||
        compositeSamplerId_ != baseColorSampler_.id) {
        if (compositeMaterial_) renderer.destroy_resource(compositeMaterial_);
        compositeMaterial_ = {};
        render::MaterialDesc compositeDescription;
        compositeDescription.name = "editor_model_preview_composite_material";
        compositeDescription.pipeline = compositePipeline_;
        compositeDescription.bindings = {
            {"SourceTexture", offscreenColor_, render::DescriptorType::Texture, 0, 0, 1, false, {}},
            {"SourceSampler", baseColorSampler_, render::DescriptorType::Sampler, 1, 0, 1, false, {}}};
        compositeMaterial_ = renderer.create_material(compositeDescription);
        if (!compositeMaterial_) {
            state.status = "GPU model preview composite material creation failed: " + renderer.last_error();
            clear_pipeline(renderer);
            return state;
        }
        compositeTextureId_ = offscreenColor_.id;
        compositeSamplerId_ = baseColorSampler_.id;
    }

    const auto import_resource = [&](render::ResourceHandle handle) {
        const auto* description = renderer.resource_description(handle);
        if (!description) return false;
        renderer.graph().import_resource(handle, *description);
        return true;
    };
    if (!import_resource(vertexBuffer_) || !import_resource(indexBuffer_) ||
        !import_resource(sceneBuffer_) || !import_resource(materialBuffer_) ||
        !import_resource(offscreenColor_) || !import_resource(offscreenDepth_) ||
        !import_resource(baseColorTexture_) || !import_resource(baseColorSampler_) ||
        !import_resource(normalTexture_) || !import_resource(normalSampler_) ||
        !import_resource(metallicRoughnessTexture_) || !import_resource(metallicRoughnessSampler_) ||
        !import_resource(material_) || !import_resource(compositeMaterial_)) {
        state.status = "GPU model preview graph import failed: persistent resource description is missing";
        return state;
    }
    const auto indexCount = static_cast<std::uint32_t>(snapshot.indices->size());
    renderer.graph().add_pass("editor_model_preview", {
        {offscreenColor_, render::ResourceUsage::ColorAttachment0},
        {offscreenDepth_, render::ResourceUsage::DepthStencil},
        {material_, render::ResourceUsage::ShaderRead},
        {baseColorTexture_, render::ResourceUsage::ShaderRead},
        {baseColorSampler_, render::ResourceUsage::ShaderRead},
        {normalTexture_, render::ResourceUsage::ShaderRead},
        {normalSampler_, render::ResourceUsage::ShaderRead},
        {metallicRoughnessTexture_, render::ResourceUsage::ShaderRead},
        {metallicRoughnessSampler_, render::ResourceUsage::ShaderRead},
        {sceneBuffer_, render::ResourceUsage::UniformBuffer},
        {materialBuffer_, render::ResourceUsage::UniformBuffer},
        {vertexBuffer_, render::ResourceUsage::VertexBuffer},
        {indexBuffer_, render::ResourceUsage::IndexBuffer}},
        [material = material_, scene = sceneBuffer_, parameters = materialBuffer_,
         vertex = vertexBuffer_, index = indexBuffer_, indexCount,
         offscreen = offscreenColor_, depth = offscreenDepth_](auto& backend, const auto&) {
            if (!backend.bind_editor_render_target(offscreen, depth, true)) return;
            backend.bind_material(material);
            backend.bind_uniform_buffer(scene, 2, 0);
            backend.bind_uniform_buffer(parameters, 4, 0);
            backend.draw_mesh({vertex, index, indexCount, math::Mat4::Identity()});
        }, render::RenderQueue::Graphics, true, false);
    renderer.graph().add_pass("editor_model_preview_composite", {
        {offscreenColor_, render::ResourceUsage::ShaderRead},
        {compositeMaterial_, render::ResourceUsage::ShaderRead},
        {baseColorSampler_, render::ResourceUsage::ShaderRead}},
        [material = compositeMaterial_, offscreen = offscreenColor_](auto& backend, const auto&) {
            if (!backend.bind_editor_render_target({}, {}, false)) return;
            backend.bind_material(material);
            backend.draw_sprite({offscreen, {0.0f, 0.0f}, {1.0f, 1.0f}, 0.0f});
        }, render::RenderQueue::Graphics, true, false);

    state.rendererReady = true;
    state.materialApplied = true;
    state.materialFactorsApplied = hasMaterial;
    state.textureRoleApplied = hasTextureSnapshot;
    state.baseColorTextureSampled = actualBaseColorTexture_;
    state.normalTextureSampled = actualNormalTexture_;
    state.metallicRoughnessTextureSampled = actualMetallicRoughnessTexture_;
    state.textureSampled = actualBaseColorTexture_ || actualNormalTexture_ ||
        actualMetallicRoughnessTexture_;
    state.offscreenTargetReady = true;
    state.offscreenCompositeApplied = true;
    state.status = hasMaterial
        ? (actualMetallicRoughnessTexture_
            ? "GPU lit preview ready (D3D11); offscreen target composited, metallic/roughness texture sampled, factors applied"
            : actualNormalTexture_
            ? "GPU lit preview ready (D3D11); offscreen target composited, metallic/roughness factors applied, normal texture sampled"
            : actualBaseColorTexture_
                ? "GPU lit preview ready (D3D11); offscreen target composited, metallic/roughness factors applied, base color texture sampled"
                : "GPU lit preview ready (D3D11); offscreen target composited, metallic/roughness factors applied, texture artifact unavailable")
        : (actualMetallicRoughnessTexture_
            ? "GPU geometry preview ready (D3D11); offscreen target composited, metallic/roughness texture sampled"
            : actualNormalTexture_
            ? "GPU geometry preview ready (D3D11); offscreen target composited, normal texture sampled"
            : actualBaseColorTexture_
                ? "GPU geometry preview ready (D3D11); offscreen target composited, base color texture sampled"
                : "GPU geometry preview ready (D3D11); offscreen target composited, base color applied, texture artifact unavailable");
    state.status += "; depth-tested";
    return state;
}

} // namespace shinkou::editor

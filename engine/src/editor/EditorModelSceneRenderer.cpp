#include "shinkou/editor/EditorModelSceneRenderer.h"

#include "shinkou/render/Renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace shinkou::editor {
namespace {

constexpr std::size_t kMaxSceneVertices = 2'000'000;
constexpr std::size_t kMaxSceneIndices = 6'000'000;

struct SceneFrame {
    math::Mat4 viewProjection{math::Mat4::Identity()};
    std::array<float, 4> cameraPositionAndFlags{};
};

struct ObjectFrame {
    math::Mat4 model{math::Mat4::Identity()};
};

struct DrawItem {
    render::ResourceHandle material{};
    render::ResourceHandle vertexBuffer{};
    render::ResourceHandle indexBuffer{};
    render::ResourceHandle objectBuffer{};
    std::uint32_t indexCount{0};
};

static_assert(sizeof(SceneFrame) % 16u == 0u);
static_assert(sizeof(ObjectFrame) % 16u == 0u);

constexpr std::string_view kSceneVertexShader = R"hlsl(
cbuffer SceneFrame : register(b2)
{
    float4x4 viewProjection;
    float4 cameraPositionAndFlags;
};

cbuffer ObjectFrame : register(b3)
{
    float4x4 model;
};

struct VertexInput
{
    float3 position : POSITION;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
};

VertexOutput main(VertexInput input)
{
    VertexOutput output;
    output.worldPosition = mul(model, float4(input.position, 1.0f)).xyz;
    output.position = mul(viewProjection, float4(output.worldPosition, 1.0f));
    return output;
}
)hlsl";

constexpr std::string_view kSceneFragmentShader = R"hlsl(
struct FragmentInput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
};

float4 main(FragmentInput input) : SV_Target
{
    float3 dx = ddx(input.worldPosition);
    float3 dy = ddy(input.worldPosition);
    float3 normal = normalize(cross(dx, dy));
    const float3 lightDirection = normalize(float3(0.35f, 0.65f, 0.75f));
    const float diffuse = 0.2f + 0.8f * saturate(dot(normal, lightDirection));
    const float3 baseColor = float3(0.24f, 0.62f, 0.95f);
    return float4(baseColor * diffuse, 1.0f);
}
)hlsl";

bool finite_matrix(const math::Mat4& value) noexcept {
    for (float component : value.m) if (!math::IsFinite(component)) return false;
    return true;
}

bool finite_vec3(const math::Vec3& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

std::vector<std::uint8_t> copy_bytes(const void* source, std::size_t bytes) {
    std::vector<std::uint8_t> output(bytes);
    if (bytes != 0) std::memcpy(output.data(), source, bytes);
    return output;
}

bool valid_buffer_description(const render::Renderer& renderer,
                              render::ResourceHandle handle, std::size_t bytes,
                              bool vertex, bool index) noexcept {
    const auto* description = renderer.resource_description(handle);
    const auto* buffer = description ? std::get_if<render::BufferDesc>(description) : nullptr;
    return buffer && buffer->size >= bytes && buffer->vertexBuffer == vertex &&
        buffer->indexBuffer == index;
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

} // namespace

void EditorModelSceneRenderer::clear_pipeline(render::Renderer& renderer) noexcept {
    if (pipeline_) renderer.destroy_resource(pipeline_);
    if (fragmentShader_) renderer.destroy_resource(fragmentShader_);
    if (vertexShader_) renderer.destroy_resource(vertexShader_);
    pipeline_ = {};
    fragmentShader_ = {};
    vertexShader_ = {};
    pipelineColorFormat_.clear();
}

void EditorModelSceneRenderer::clear(render::Renderer& renderer) noexcept {
    for (auto& [assetId, geometry] : geometry_) {
        (void)assetId;
        if (geometry.vertexBuffer) renderer.destroy_resource(geometry.vertexBuffer);
        if (geometry.indexBuffer) renderer.destroy_resource(geometry.indexBuffer);
    }
    for (auto& [objectId, buffer] : objectBuffers_) {
        (void)objectId;
        if (buffer) renderer.destroy_resource(buffer);
    }
    geometry_.clear();
    objectBuffers_.clear();
    if (sceneBuffer_) renderer.destroy_resource(sceneBuffer_);
    sceneBuffer_ = {};
    sceneRevision_ = 0;
    clear_pipeline(renderer);
}

EditorModelSceneRenderState EditorModelSceneRenderer::render(
    render::Renderer& renderer,
    const std::vector<EditorModelSceneInstance>& instances,
    const math::Mat4& viewProjection,
    math::Vec3 cameraPosition) {
    EditorModelSceneRenderState state;
    const auto capabilities = renderer.capabilities();
    state.backend = capabilities.api;
    if (instances.empty()) {
        state.rendererReady = true;
        state.status = "GPU model scene idle: no AssetId-backed model references";
        return state;
    }
    if (!capabilities.deviceReady) {
        state.status = "GPU model scene unavailable: " + backend_label(capabilities.api) +
            " device is not ready";
        return state;
    }
    if (!capabilities.supportsEditorModelRendering) {
        state.status = "GPU model scene fallback: " + backend_label(capabilities.api) +
            " scene shader path is pending; references remain in the World";
        return state;
    }
    if (!renderer.editor_viewport().enabled ||
        !renderer.editor_viewport().viewport.valid() ||
        !capabilities.supportsEditorViewportScissor) {
        state.status = "GPU model scene fallback: editor viewport seam is unavailable";
        return state;
    }
    if (!finite_matrix(viewProjection) || !finite_vec3(cameraPosition)) {
        state.status = "GPU model scene rejected: camera frame contains non-finite values";
        return state;
    }

    const std::string colorFormat = "bgra8";
    if (!pipeline_ || pipelineColorFormat_ != colorFormat) {
        clear_pipeline(renderer);
        render::ShaderDesc vertexDescription;
        vertexDescription.stage = render::ShaderStage::Vertex;
        vertexDescription.name = "editor_model_scene_vertex";
        vertexDescription.source = std::string(kSceneVertexShader);
        vertexDescription.profile = "vs_5_0";
        vertexDescription.revision = 1;
        vertexShader_ = renderer.create_shader(vertexDescription);

        render::ShaderDesc fragmentDescription;
        fragmentDescription.stage = render::ShaderStage::Fragment;
        fragmentDescription.name = "editor_model_scene_fragment";
        fragmentDescription.source = std::string(kSceneFragmentShader);
        fragmentDescription.profile = "ps_5_0";
        fragmentDescription.revision = 1;
        fragmentShader_ = renderer.create_shader(fragmentDescription);
        if (!vertexShader_ || !fragmentShader_) {
            state.status = "GPU model scene shader creation failed: " + renderer.last_error();
            clear_pipeline(renderer);
            return state;
        }

        render::PipelineDesc pipelineDescription;
        pipelineDescription.name = "editor_model_scene_pipeline_" + colorFormat;
        pipelineDescription.vertexShader = vertexShader_.id;
        pipelineDescription.fragmentShader = fragmentShader_.id;
        pipelineDescription.depthTest = false;
        pipelineDescription.depthWrite = false;
        pipelineDescription.vertexInput = true;
        pipelineDescription.vertexTextureCoordinates = false;
        pipelineDescription.vertexNormals = false;
        pipelineDescription.cullMode = "none";
        pipelineDescription.fillMode = "solid";
        pipelineDescription.topology = "triangle";
        pipelineDescription.colorFormat = colorFormat;
        pipelineDescription.depthFormat = "none";
        pipeline_ = renderer.create_pipeline(pipelineDescription);
        if (!pipeline_) {
            state.status = "GPU model scene pipeline creation failed: " + renderer.last_error();
            clear_pipeline(renderer);
            return state;
        }
        pipelineColorFormat_ = colorFormat;
    }
    state.pipelineReady = true;

    SceneFrame sceneFrame;
    sceneFrame.viewProjection = viewProjection;
    sceneFrame.cameraPositionAndFlags = {cameraPosition.x, cameraPosition.y, cameraPosition.z, 0.0f};
    if (!valid_buffer_description(renderer, sceneBuffer_, sizeof(SceneFrame), false, false)) {
        if (sceneBuffer_) renderer.destroy_resource(sceneBuffer_);
        render::BufferDesc description;
        description.size = sizeof(SceneFrame);
        description.uniformBuffer = true;
        description.initialData = copy_bytes(&sceneFrame, sizeof(SceneFrame));
        sceneBuffer_ = renderer.create_buffer(description);
        sceneRevision_ = 0;
    }
    if (!sceneBuffer_) {
        state.status = "GPU model scene camera upload failed: " + renderer.last_error();
        return state;
    }
    {
        if (!renderer.update_buffer({sceneBuffer_, 0, copy_bytes(&sceneFrame, sizeof(SceneFrame))})) {
            state.status = "GPU model scene camera upload failed: " + renderer.last_error();
            return state;
        }
        // The scene frame changes with editor camera motion. Uploading this
        // small bounded buffer once per frame keeps the preview responsive;
        // model geometry remains cached by AssetId and revision.
        ++sceneRevision_;
    }
    state.sceneBufferReady = true;

    std::vector<DrawItem> draws;
    draws.reserve(std::min<std::size_t>(instances.size(), 256u));
    std::unordered_set<assets::AssetId> uploadedAssets;
    for (const auto& instance : instances) {
        if (instance.assetId == 0 || instance.objectId == 0 || !instance.snapshot ||
            !instance.snapshot->valid() || !finite_matrix(instance.model)) {
            ++state.rejectedInstances;
            continue;
        }
        const auto& snapshot = *instance.snapshot;
        if (snapshot.vertices->size() > kMaxSceneVertices ||
            snapshot.indices->size() > kMaxSceneIndices ||
            snapshot.vertices->size() > std::numeric_limits<std::size_t>::max() / sizeof(math::Vec3) ||
            snapshot.indices->size() > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t)) {
            ++state.rejectedInstances;
            continue;
        }
        bool validGeometry = true;
        for (const auto& vertex : *snapshot.vertices) {
            if (!finite_vec3(vertex)) { validGeometry = false; break; }
        }
        if (validGeometry) for (const auto index : *snapshot.indices) {
            if (index >= snapshot.vertices->size()) { validGeometry = false; break; }
        }
        if (!validGeometry) {
            ++state.rejectedInstances;
            continue;
        }

        auto geometryIt = geometry_.find(instance.assetId);
        if (geometryIt == geometry_.end()) geometryIt = geometry_.emplace(instance.assetId, GeometryResource{}).first;
        auto& geometry = geometryIt->second;
        const auto vertexBytes = snapshot.vertices->size() * sizeof(math::Vec3);
        const auto indexBytes = snapshot.indices->size() * sizeof(std::uint32_t);
        const auto vertexData = copy_bytes(snapshot.vertices->data(), vertexBytes);
        const auto indexData = copy_bytes(snapshot.indices->data(), indexBytes);
        if (!valid_buffer_description(renderer, geometry.vertexBuffer, vertexBytes, true, false)) {
            if (geometry.vertexBuffer) renderer.destroy_resource(geometry.vertexBuffer);
            render::BufferDesc description;
            description.size = vertexBytes;
            description.stride = sizeof(math::Vec3);
            description.vertexBuffer = true;
            description.initialData = vertexData;
            geometry.vertexBuffer = renderer.create_buffer(description);
            geometry.vertexBytes = geometry.vertexBuffer ? vertexBytes : 0;
            geometry.revision = 0;
        } else if (geometry.revision != snapshot.revision) {
            if (!renderer.update_buffer({geometry.vertexBuffer, 0, vertexData})) {
                ++state.rejectedInstances;
                continue;
            }
        }
        if (!valid_buffer_description(renderer, geometry.indexBuffer, indexBytes, false, true)) {
            if (geometry.indexBuffer) renderer.destroy_resource(geometry.indexBuffer);
            render::BufferDesc description;
            description.size = indexBytes;
            description.stride = sizeof(std::uint32_t);
            description.indexBuffer = true;
            description.initialData = indexData;
            geometry.indexBuffer = renderer.create_buffer(description);
            geometry.indexBytes = geometry.indexBuffer ? indexBytes : 0;
            geometry.revision = 0;
        } else if (geometry.revision != snapshot.revision) {
            if (!renderer.update_buffer({geometry.indexBuffer, 0, indexData})) {
                ++state.rejectedInstances;
                continue;
            }
        }
        if (!geometry.vertexBuffer || !geometry.indexBuffer) {
            ++state.rejectedInstances;
            continue;
        }
        geometry.revision = snapshot.revision;
        geometry.indexCount = static_cast<std::uint32_t>(snapshot.indices->size());
        uploadedAssets.insert(instance.assetId);

        ObjectFrame objectFrame;
        objectFrame.model = instance.model;
        auto objectIt = objectBuffers_.find(instance.objectId);
        if (objectIt == objectBuffers_.end()) objectIt = objectBuffers_.emplace(instance.objectId, render::ResourceHandle{}).first;
        auto& objectBuffer = objectIt->second;
        if (!valid_buffer_description(renderer, objectBuffer, sizeof(ObjectFrame), false, false)) {
            if (objectBuffer) renderer.destroy_resource(objectBuffer);
            render::BufferDesc description;
            description.size = sizeof(ObjectFrame);
            description.uniformBuffer = true;
            description.initialData = copy_bytes(&objectFrame, sizeof(ObjectFrame));
            objectBuffer = renderer.create_buffer(description);
        } else if (!renderer.update_buffer({objectBuffer, 0, copy_bytes(&objectFrame, sizeof(ObjectFrame))})) {
            ++state.rejectedInstances;
            continue;
        }
        if (!objectBuffer) {
            ++state.rejectedInstances;
            continue;
        }
        auto import_resource = [&](render::ResourceHandle handle) {
            const auto* description = renderer.resource_description(handle);
            if (!description) return false;
            renderer.graph().import_resource(handle, *description);
            return true;
        };
        if (!import_resource(geometry.vertexBuffer) || !import_resource(geometry.indexBuffer) ||
            !import_resource(sceneBuffer_) || !import_resource(objectBuffer)) {
            ++state.rejectedInstances;
            continue;
        }
        const auto material = renderer.graph().create_material({
            "editor_model_scene_material_" + std::to_string(instance.objectId), pipeline_,
            {{"SceneFrame", sceneBuffer_, render::DescriptorType::UniformBuffer, 2, 0, 1, false, {}},
             {"ObjectFrame", objectBuffer, render::DescriptorType::UniformBuffer, 3, 0, 1, false, {}}}, false});
        if (!material) {
            ++state.rejectedInstances;
            continue;
        }
        draws.push_back({material, geometry.vertexBuffer, geometry.indexBuffer, objectBuffer, geometry.indexCount});
        ++state.submittedInstances;
    }

    state.uploadedAssets = uploadedAssets.size();
    if (draws.empty()) {
        state.status = "GPU model scene waiting: no valid model geometry is ready";
        return state;
    }
    std::vector<render::ResourceAccess> accesses{{sceneBuffer_, render::ResourceUsage::UniformBuffer}};
    for (const auto& draw : draws) {
        accesses.push_back({draw.material, render::ResourceUsage::ShaderRead});
        accesses.push_back({draw.vertexBuffer, render::ResourceUsage::VertexBuffer});
        accesses.push_back({draw.indexBuffer, render::ResourceUsage::IndexBuffer});
        accesses.push_back({draw.objectBuffer, render::ResourceUsage::UniformBuffer});
    }
    renderer.graph().add_pass("editor_model_scene", std::move(accesses),
        [draws = std::move(draws), scene = sceneBuffer_](auto& backend, const auto&) {
            if (!backend.bind_editor_render_target({}, {}, false)) return;
            for (const auto& draw : draws) {
                backend.bind_material(draw.material);
                backend.bind_uniform_buffer(scene, 2, 0);
                backend.bind_uniform_buffer(draw.objectBuffer, 3, 0);
                backend.draw_mesh({draw.vertexBuffer, draw.indexBuffer, draw.indexCount, math::Mat4::Identity()});
            }
        }, render::RenderQueue::Graphics, true, false);
    state.rendererReady = true;
    state.frameRevision = sceneRevision_;
    state.status = "GPU model scene ready (D3D11): " + std::to_string(state.submittedInstances) +
        " AssetId-backed instance(s) submitted";
    return state;
}

void EditorModelSceneRenderer::prune(render::Renderer& renderer,
                                     const std::vector<assets::AssetId>& activeAssets,
                                     const std::vector<std::uint64_t>& activeObjects) noexcept {
    const std::unordered_set<assets::AssetId> assets(activeAssets.begin(), activeAssets.end());
    for (auto it = geometry_.begin(); it != geometry_.end();) {
        if (assets.find(it->first) != assets.end()) { ++it; continue; }
        if (it->second.vertexBuffer) renderer.destroy_resource(it->second.vertexBuffer);
        if (it->second.indexBuffer) renderer.destroy_resource(it->second.indexBuffer);
        it = geometry_.erase(it);
    }
    const std::unordered_set<std::uint64_t> objects(activeObjects.begin(), activeObjects.end());
    for (auto it = objectBuffers_.begin(); it != objectBuffers_.end();) {
        if (objects.find(it->first) != objects.end()) { ++it; continue; }
        if (it->second) renderer.destroy_resource(it->second);
        it = objectBuffers_.erase(it);
    }
}

} // namespace shinkou::editor

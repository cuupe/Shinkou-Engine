#include "shinkou/render/RenderScene.h"
#include "shinkou/components/CameraComponent.h"
#include "shinkou/render/Renderer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace shinkou::render {
namespace {
struct FrustumCorners {
    std::array<math::Vec3, 8> points{};
};

math::Mat4 transform_matrix(const math::Transform &transform) {
    return math::TransformMatrix(transform);
}

math::Vec3 light_direction(const LightRenderItem &light) {
    const auto matrix = transform_matrix(light.transform);
    return math::Normalize(math::TransformPoint(matrix, {0.0f, 0.0f, 1.0f}) -
                           light.transform.position);
}

FrustumCorners cascade_corners(const RenderView &view, float nearDistance, float farDistance) {
    const float aspect = view.projection.orthographic
                             ? 1.0f
                             : (view.projectionMatrix.m[5] > 0.0001f
                                    ? view.projectionMatrix.m[5] / view.projectionMatrix.m[0]
                                    : 1.0f);
    const float nearHeight =
        view.projection.orthographic
            ? view.projection.orthographicSize
            : 2.0f * nearDistance * std::tan(view.projection.verticalFieldOfView * 0.5f);
    const float farHeight =
        view.projection.orthographic
            ? view.projection.orthographicSize
            : 2.0f * farDistance * std::tan(view.projection.verticalFieldOfView * 0.5f);
    const float nearWidth = nearHeight * aspect;
    const float farWidth = farHeight * aspect;
    const auto forward =
        math::Normalize(math::TransformPoint(transform_matrix(view.transform), {0.0f, 0.0f, 1.0f}) -
                        view.transform.position);
    const auto right =
        math::Normalize(math::TransformPoint(transform_matrix(view.transform), {1.0f, 0.0f, 0.0f}) -
                        view.transform.position);
    const auto up =
        math::Normalize(math::TransformPoint(transform_matrix(view.transform), {0.0f, 1.0f, 0.0f}) -
                        view.transform.position);
    const auto nearCenter = view.transform.position + forward * nearDistance;
    const auto farCenter = view.transform.position + forward * farDistance;
    return {{{nearCenter + up * (nearHeight * 0.5f) - right * (nearWidth * 0.5f),
              nearCenter + up * (nearHeight * 0.5f) + right * (nearWidth * 0.5f),
              nearCenter - up * (nearHeight * 0.5f) - right * (nearWidth * 0.5f),
              nearCenter - up * (nearHeight * 0.5f) + right * (nearWidth * 0.5f),
              farCenter + up * (farHeight * 0.5f) - right * (farWidth * 0.5f),
              farCenter + up * (farHeight * 0.5f) + right * (farWidth * 0.5f),
              farCenter - up * (farHeight * 0.5f) - right * (farWidth * 0.5f),
              farCenter - up * (farHeight * 0.5f) + right * (farWidth * 0.5f)}}};
}

math::Mat4 view_matrix(const math::Transform &transform) {
    const math::Quat inverse{-transform.rotation.x, -transform.rotation.y, -transform.rotation.z,
                             transform.rotation.w};
    return math::Multiply(math::Rotation(inverse), math::Translation(transform.position * -1.0f));
}

bool sphere_in_view(const math::Mat4 &viewProjection, math::Vec3 center, float radius) {
    const float x = center.x * viewProjection.m[0] + center.y * viewProjection.m[4] +
                    center.z * viewProjection.m[8] + viewProjection.m[12];
    const float y = center.x * viewProjection.m[1] + center.y * viewProjection.m[5] +
                    center.z * viewProjection.m[9] + viewProjection.m[13];
    const float z = center.x * viewProjection.m[2] + center.y * viewProjection.m[6] +
                    center.z * viewProjection.m[10] + viewProjection.m[14];
    const float w = center.x * viewProjection.m[3] + center.y * viewProjection.m[7] +
                    center.z * viewProjection.m[11] + viewProjection.m[15];
    if (w <= 0.0001f)
        return false;
    const float clipRadius =
        radius * std::max({std::abs(viewProjection.m[0]), std::abs(viewProjection.m[5]),
                           std::abs(viewProjection.m[10]), 1.0f});
    return x >= -w - clipRadius && x <= w + clipRadius && y >= -w - clipRadius &&
           y <= w + clipRadius && z >= -clipRadius && z <= w + clipRadius;
}

bool resource_matches(const Renderer &renderer, ResourceHandle handle, ResourceKind kind,
                      ResourceDesc *description) {
    if (!handle || handle.kind != kind)
        return false;
    const auto *found = renderer.resource_description(handle);
    if (!found)
        return false;
    if (description)
        *description = resource_metadata(*found);
    return true;
}

bool index_range_matches(const ResourceDesc &description, std::uint32_t firstIndex,
                         std::uint32_t indexCount) noexcept {
    const auto *buffer = std::get_if<BufferDesc>(&description);
    constexpr std::size_t indexBytes = sizeof(std::uint32_t);
    if (!buffer || indexCount == 0 ||
        firstIndex > std::numeric_limits<std::size_t>::max() / indexBytes ||
        indexCount > std::numeric_limits<std::size_t>::max() / indexBytes)
        return false;
    const auto firstByte = static_cast<std::size_t>(firstIndex) * indexBytes;
    const auto countBytes = static_cast<std::size_t>(indexCount) * indexBytes;
    return firstByte <= buffer->size && countBytes <= buffer->size - firstByte;
}

bool is_instance_binding(const DescriptorBinding &binding) {
    return binding.type == DescriptorType::StructuredBuffer &&
           (binding.name == "instances" || binding.name == "instanceModels" ||
            binding.name == "instanceBuffer");
}

bool has_structured_instance_binding(const MeshRenderItem &mesh) {
    const auto *material = std::get_if<MaterialDesc>(&mesh.materialDescription);
    if (!material)
        return false;
    return std::any_of(material->bindings.begin(), material->bindings.end(), is_instance_binding);
}

struct GpuMeshItem {
    MeshRenderItem item;
    ResourceHandle objectBuffer{};
};

struct GpuInstanceData {
    math::Mat4 model{math::Mat4::Identity()};
};

struct GpuDrawArguments {
    std::uint32_t indexCount{0};
    std::uint32_t instanceCount{0};
    std::uint32_t firstIndex{0};
    std::int32_t vertexOffset{0};
    std::uint32_t firstInstance{0};
};

std::uint64_t gpu_batch_key(const MeshRenderItem &mesh) {
    std::uint64_t key = 1469598103934665603ull;
    const auto append = [&key](std::uint32_t value) {
        key ^= value;
        key *= 1099511628211ull;
    };
    append(mesh.material.id);
    append(mesh.vertexBuffer.id);
    append(mesh.indexBuffer.id);
    return key;
}
} // namespace

std::vector<GpuLightData> make_gpu_light_data(const std::vector<LightRenderItem> &source) {
    std::vector<GpuLightData> lights;
    lights.reserve(source.size());
    for (const auto &item : source) {
        GpuLightData data;
        data.position[0] = item.transform.position.x;
        data.position[1] = item.transform.position.y;
        data.position[2] = item.transform.position.z;
        data.position[3] = static_cast<float>(item.light.type);
        const auto forward =
            math::TransformPoint(transform_matrix(item.transform), {0.0f, 0.0f, 1.0f}) -
            item.transform.position;
        const auto direction = math::Normalize(forward);
        data.direction[0] = direction.x;
        data.direction[1] = direction.y;
        data.direction[2] = direction.z;
        data.direction[3] = item.light.outerCone;
        data.colorIntensity[0] = item.light.color.x;
        data.colorIntensity[1] = item.light.color.y;
        data.colorIntensity[2] = item.light.color.z;
        data.colorIntensity[3] = item.light.intensity;
        data.parameters[0] = item.light.range;
        data.parameters[1] = item.light.innerCone;
        data.parameters[2] = item.light.castsShadow ? 1.0f : 0.0f;
        data.parameters[3] = static_cast<float>(item.entity.id);
        lights.push_back(data);
    }
    return lights;
}

RenderView make_render_view(const CameraComponent &camera, const math::Transform &pose,
                            float aspectRatio, Entity entity) {
    RenderView view;
    view.camera = entity;
    view.cameraAvailable = true;
    view.transform = pose;
    view.projection = camera;
    view.viewMatrix = view_matrix(pose);
    view.projectionMatrix =
        camera.orthographic
            ? math::Orthographic(-camera.orthographicSize * aspectRatio * .5f,
                                 camera.orthographicSize * aspectRatio * .5f,
                                 -camera.orthographicSize * .5f, camera.orthographicSize * .5f,
                                 camera.nearPlane, camera.farPlane)
            : math::Perspective(camera.verticalFieldOfView, aspectRatio, camera.nearPlane,
                                camera.farPlane);
    view.viewProjection = view.projectionMatrix * view.viewMatrix;
    return view;
}
RenderView extract_render_view(const World &world, float aspectRatio) {
    RenderView view;
    std::int64_t priority = std::numeric_limits<std::int64_t>::min();
    world.each_game_object([&](const GameObject &object) {
        const auto *camera = object.get_component<components::CameraComponent>();
        const auto *transform = object.get_component<components::TransformComponent>();
        if (!camera || !camera->enabled() || !camera->active || !object.active_in_hierarchy() ||
            !transform || (view.has_camera() && camera->priority <= priority))
            return;
        math::Transform pose;
        if (!math::Decompose(transform->world_matrix(), pose))
            return;
        pose.scale = {1, 1, 1};
        CameraComponent projection;
        projection.verticalFieldOfView = camera->fieldOfView * math::Pi / 180;
        projection.nearPlane = camera->nearPlane;
        projection.farPlane = camera->farPlane;
        projection.orthographic = camera->orthographic;
        projection.orthographicSize = camera->orthographicSize;
        view = make_render_view(projection, pose, aspectRatio);
        priority = camera->priority;
    });
    world.ecs().each<CameraComponent, TransformComponent>(
        [&](Entity entity, const auto &camera, const auto &transform) {
            if (view.has_camera() || !camera.active || !transform.visible)
                return;
            math::Transform pose = transform.local;
            // Runtime world matrices live in chunk memory; prefer them so camera
            // extraction does not touch the authoring hierarchy.
            if (const auto *chunkMatrix = world.ecs().try_get<ecs::LocalToWorld>(entity)) {
                math::Transform decomposed;
                if (math::Decompose(chunkMatrix->matrix, decomposed)) {
                    decomposed.scale = {1, 1, 1};
                    pose = decomposed;
                }
            }
            view = make_render_view(camera, pose, aspectRatio, entity);
        });
    return view;
}

void RenderScene::extract(const World &world, const Renderer &renderer, float aspectRatio,
                          const RenderView *overrideView) {
    meshes_.clear();
    shadowMeshes_.clear();
    sprites_.clear();
    lights_.clear();
    view_ = {};
    stats_ = {};
    // GameObject transforms are authoritative for ECS-backed editor meshes.
    // world.find_object is a flat entity-index lookup, so extraction needs no
    // per-frame owner map and no hashing per mesh.
    view_ = overrideView ? *overrideView : extract_render_view(world, aspectRatio);

    world.ecs().each<MeshRendererComponent, TransformComponent>(
        [&](Entity entity, const MeshRendererComponent &mesh, const TransformComponent &transform) {
            ++stats_.extractedMeshes;
            const auto *owner = world.find_object(entity);
            if (!mesh.visible || !transform.visible || mesh.indexCount == 0 ||
                (owner && !owner->active_in_hierarchy())) {
                ++stats_.culledMeshes;
                return;
            }
            MeshRenderItem item;
            item.entity = entity;
            item.vertexBuffer = mesh.vertexBuffer;
            item.indexBuffer = mesh.indexBuffer;
            item.material = mesh.material;
            item.indexCount = mesh.indexCount;
            item.firstIndex = mesh.firstIndex;
            item.vertexOffset = mesh.vertexOffset;
            // Chunk world matrix first: this is the DOTS-style path and needs no
            // authoring lookup.  Entities without one (raw ECS scene entities)
            // fall back to their local transform / GameObject matrix.
            if (const auto *chunkMatrix = world.ecs().try_get<ecs::LocalToWorld>(entity)) {
                item.transform = chunkMatrix->matrix;
            } else {
                item.transform = transform_matrix(transform.local);
                if (owner)
                    if (const auto *spatial =
                            owner->get_component<components::TransformComponent>())
                        item.transform = spatial->world_matrix();
            }
            if (!resource_matches(renderer, item.vertexBuffer, ResourceKind::Buffer,
                                  &item.vertexDescription) ||
                !resource_matches(renderer, item.indexBuffer, ResourceKind::Buffer,
                                  &item.indexDescription) ||
                !resource_matches(renderer, item.material, ResourceKind::Material,
                                  &item.materialDescription) ||
                !index_range_matches(item.indexDescription, item.firstIndex, item.indexCount))
                return;
            // Camera visibility cannot cull shadow casters: an off-screen object
            // can still cast a shadow into the visible camera frustum.
            shadowMeshes_.push_back({item.vertexBuffer, item.indexBuffer, item.indexCount,
                                     item.transform, item.firstIndex, item.vertexOffset});
            if (view_.has_camera() && view_.projection.farPlane > 0.0f) {
                const auto distance = math::Length(math::TransformPoint(item.transform, {0, 0, 0}) -
                                                   view_.transform.position);
                if (distance > view_.projection.farPlane + mesh.boundsRadius) {
                    ++stats_.culledMeshes;
                    return;
                }
                const auto worldCenter = math::TransformPoint(item.transform, mesh.boundsCenter);
                const auto scale =
                    math::Vec3{math::Length(math::TransformVector(item.transform, {1, 0, 0})),
                               math::Length(math::TransformVector(item.transform, {0, 1, 0})),
                               math::Length(math::TransformVector(item.transform, {0, 0, 1}))};
                const auto worldRadius =
                    mesh.boundsRadius * std::max({scale.x, scale.y, scale.z, 0.001f});
                if (!sphere_in_view(view_.viewProjection, worldCenter, worldRadius)) {
                    ++stats_.culledMeshes;
                    return;
                }
            }
            item.gpuDriven = mesh.gpuDriven && has_structured_instance_binding(item);
            item.boundsCenter = mesh.boundsCenter;
            item.boundsRadius = std::max(0.001f, mesh.boundsRadius);
            meshes_.push_back(std::move(item));
            ++stats_.visibleMeshes;
        });

    world.ecs().each<SpriteRendererComponent, TransformComponent>(
        [&](Entity entity, const SpriteRendererComponent &sprite,
            const TransformComponent &transform) {
            if (!sprite.visible || !transform.visible)
                return;
            SpriteRenderItem item;
            item.entity = entity;
            item.texture = sprite.texture;
            item.material = sprite.material;
            item.position = {transform.local.position.x, transform.local.position.y};
            if (const auto *chunkMatrix = world.ecs().try_get<ecs::LocalToWorld>(entity)) {
                item.position = {chunkMatrix->matrix.m[12], chunkMatrix->matrix.m[13]};
            }
            item.size = sprite.size;
            item.rotation = sprite.rotation;
            if (view_.has_camera() && view_.projection.farPlane > 0.0f) {
                const auto distance =
                    math::Length(transform.local.position - view_.transform.position);
                if (distance > view_.projection.farPlane)
                    return;
            }
            if (!resource_matches(renderer, item.texture, ResourceKind::Texture2D,
                                  &item.textureDescription) ||
                !resource_matches(renderer, item.material, ResourceKind::Material,
                                  &item.materialDescription))
                return;
            sprites_.push_back(std::move(item));
            ++stats_.extractedSprites;
        });

    world.ecs().each<LightComponent, TransformComponent>(
        [&](Entity entity, const LightComponent &light, const TransformComponent &transform) {
            if (!light.visible || !transform.visible)
                return;
            if (lights_.size() >= 256)
                return;
            math::Transform pose = transform.local;
            if (const auto *chunkMatrix = world.ecs().try_get<ecs::LocalToWorld>(entity)) {
                math::Transform decomposed;
                if (math::Decompose(chunkMatrix->matrix, decomposed))
                    pose = decomposed;
            }
            lights_.push_back({entity, light, pose});
        });
}

void ForwardRenderer::build(Renderer &renderer, const RenderScene &scene,
                            ResourceHandle colorTarget, const TextureDesc &colorDescription,
                            ResourceHandle depthTarget, const TextureDesc &depthDescription,
                            const GpuDrivenSettings &gpuDriven, RenderGraph *graphOverride,
                            const RenderView *viewOverride, bool preserveDepth) {
    auto &graph = graphOverride ? *graphOverride : renderer.graph();
    const auto firstPass = graph.passes().size();
    const auto &view = viewOverride ? *viewOverride : scene.view();
    SceneFrameData frameData;
    frameData.viewProjection = view.viewProjection;
    frameData.lightingParameters[0] = static_cast<float>(scene.lights().size());
    frameData.cameraPositionAndFlags[0] = view.transform.position.x;
    frameData.cameraPositionAndFlags[1] = view.transform.position.y;
    frameData.cameraPositionAndFlags[2] = view.transform.position.z;
    frameData.cameraPositionAndFlags[3] = view.has_camera() ? 1.0f : 0.0f;
    std::vector<std::uint8_t> frameBytes(sizeof(frameData));
    std::memcpy(frameBytes.data(), &frameData, frameBytes.size());
    if (!sceneBuffer_) {
        sceneBufferDescription_ = {sizeof(frameData), sizeof(float) * 4, false, false, frameBytes};
        sceneBufferDescription_.uniformBuffer = true;
        sceneBuffer_ = renderer.create_buffer(sceneBufferDescription_);
    } else if (!renderer.update_buffer({sceneBuffer_, 0, frameBytes})) {
        return;
    }

    const auto lights = make_gpu_light_data(scene.lights());
    // Keep one deterministic zero light record when the scene has no lights.
    // Materials can therefore retain a stable structured-buffer binding and
    // never observe the previous frame's light data.
    const auto lightRecordCount = std::max<std::size_t>(1, lights.size());
    std::vector<std::uint8_t> lightBytes(lightRecordCount * sizeof(GpuLightData), 0);
    if (!lights.empty())
        std::memcpy(lightBytes.data(), lights.data(), lightBytes.size());
    if (!lightBuffer_ || lightBufferDescription_.size < lightBytes.size()) {
        if (lightBuffer_)
            renderer.destroy_resource(lightBuffer_);
        lightBufferDescription_ = {};
        lightBufferDescription_.size = lightBytes.size();
        lightBufferDescription_.stride = sizeof(GpuLightData);
        lightBufferDescription_.initialData = lightBytes;
        lightBufferDescription_.structuredBuffer = true;
        lightBuffer_ = renderer.create_buffer(lightBufferDescription_);
        if (!lightBuffer_)
            return;
    } else {
        lightBufferDescription_.initialData = lightBytes;
        if (!renderer.update_buffer({lightBuffer_, 0, lightBytes}))
            return;
    }

    graph.import_texture(colorTarget, colorDescription);
    graph.import_resource(sceneBuffer_, sceneBufferDescription_);
    if (lightBuffer_)
        graph.import_resource(lightBuffer_, lightBufferDescription_);
    if (!identityObjectBuffer_) {
        ObjectFrameData objectData;
        std::vector<std::uint8_t> objectBytes(sizeof(objectData));
        std::memcpy(objectBytes.data(), &objectData, objectBytes.size());
        identityObjectDescription_ = {sizeof(objectData), sizeof(float) * 4, false, false,
                                      objectBytes};
        identityObjectDescription_.uniformBuffer = true;
        identityObjectBuffer_ = renderer.create_buffer(identityObjectDescription_);
    }
    graph.import_resource(identityObjectBuffer_, identityObjectDescription_);
    if (depthTarget)
        graph.import_resource(depthTarget, depthDescription);

    const bool currentShadows =
        shadowGeneration_ == graph.generation() && graph.contains_resource(shadowFrame_);
    const auto bindShadows = [&](MaterialDesc &material) {
        if (!currentShadows)
            return;
        for (auto &binding : material.bindings) {
            if (binding.name == "ShadowAtlas" && binding.type == DescriptorType::Texture)
                binding.resource = shadowAtlas_;
            if (binding.name == "ShadowFrame" && binding.type == DescriptorType::UniformBuffer)
                binding.resource = shadowFrame_;
        }
    };

    const auto append_material_dependencies = [&](const MaterialDesc &material,
                                                  std::vector<ResourceAccess> &accesses) {
        const auto append = [&](ResourceHandle handle, ResourceUsage usage) {
            if (!handle)
                return;
            if (!graph.contains_resource(handle)) {
                const auto *description = renderer.resource_description(handle);
                if (description)
                    graph.import_resource(handle, *description);
            }
            if (std::none_of(accesses.begin(), accesses.end(), [&](const auto &existing) {
                    return existing.resource.id == handle.id;
                }))
                accesses.push_back({handle, usage});
        };
        append(material.pipeline, ResourceUsage::ShaderRead);
        for (const auto &binding : material.bindings) {
            const auto usage = binding.type == DescriptorType::UniformBuffer
                                   ? ResourceUsage::UniformBuffer
                                   : ResourceUsage::ShaderRead;
            if (binding.resources.empty())
                append(binding.resource, usage);
            else
                for (const auto handle : binding.resources)
                    append(handle, usage);
        }
    };

    std::unordered_set<Entity, EntityHash> seenObjects;
    const auto objectBufferFor = [&](const MeshRenderItem &mesh) {
        ObjectFrameData objectData;
        objectData.model = mesh.transform;
        std::vector<std::uint8_t> objectBytes(sizeof(objectData));
        std::memcpy(objectBytes.data(), &objectData, objectBytes.size());
        const auto existing = objectBuffers_.find(mesh.entity);
        if (existing == objectBuffers_.end()) {
            const auto description =
                BufferDesc{sizeof(objectData), sizeof(float) * 4, false, false, objectBytes};
            auto uniformDescription = description;
            uniformDescription.uniformBuffer = true;
            const auto handle = renderer.create_buffer(uniformDescription);
            objectBuffers_.emplace(mesh.entity, handle);
            return handle;
        }
        renderer.update_buffer({existing->second, 0, objectBytes});
        return existing->second;
    };

    std::vector<const MeshRenderItem *> sortedMeshes;
    sortedMeshes.reserve(scene.meshes().size());
    for (const auto &mesh : scene.meshes())
        sortedMeshes.push_back(&mesh);
    std::sort(sortedMeshes.begin(), sortedMeshes.end(), [](const auto *lhs, const auto *rhs) {
        if (lhs->material.id != rhs->material.id)
            return lhs->material.id < rhs->material.id;
        if (lhs->vertexBuffer.id != rhs->vertexBuffer.id)
            return lhs->vertexBuffer.id < rhs->vertexBuffer.id;
        if (lhs->indexBuffer.id != rhs->indexBuffer.id)
            return lhs->indexBuffer.id < rhs->indexBuffer.id;
        if (lhs->gpuDriven != rhs->gpuDriven)
            return lhs->gpuDriven < rhs->gpuDriven;
        return lhs->entity.id < rhs->entity.id;
    });
    std::unordered_set<std::uint64_t> seenGpuBatches;
    // A scene target is shared by all material/mesh batches.  Only the first
    // graphics pass may clear it; clearing on every batch makes the last
    // material group erase all previously rendered objects.
    bool colorTargetCleared = false;
    for (std::size_t offset = 0; offset < sortedMeshes.size();) {
        const auto material = sortedMeshes[offset]->material;
        const auto vertexBuffer = sortedMeshes[offset]->vertexBuffer;
        const auto indexBuffer = sortedMeshes[offset]->indexBuffer;
        const bool gpuDrivenMesh = sortedMeshes[offset]->gpuDriven;
        std::vector<GpuMeshItem> batch;
        while (offset < sortedMeshes.size() && sortedMeshes[offset]->material.id == material.id &&
               sortedMeshes[offset]->vertexBuffer.id == vertexBuffer.id &&
               sortedMeshes[offset]->indexBuffer.id == indexBuffer.id &&
               sortedMeshes[offset]->gpuDriven == gpuDrivenMesh) {
            const auto &mesh = *sortedMeshes[offset++];
            if (!gpuDrivenMesh)
                seenObjects.insert(mesh.entity);
            batch.push_back({mesh, gpuDrivenMesh ? ResourceHandle{} : objectBufferFor(mesh)});
            graph.import_resource(mesh.vertexBuffer, mesh.vertexDescription);
            graph.import_resource(mesh.indexBuffer, mesh.indexDescription);
            graph.import_resource(mesh.material, mesh.materialDescription);
            if (!gpuDrivenMesh)
                if (const auto *objectDescription =
                        renderer.resource_description(batch.back().objectBuffer)) {
                    graph.import_resource(batch.back().objectBuffer, *objectDescription);
                }
        }
        if (gpuDrivenMesh && !batch.empty()) {
            const auto key = gpu_batch_key(batch.front().item);
            seenGpuBatches.insert(key);
            std::vector<std::uint8_t> instanceBytes(batch.size() * sizeof(GpuInstanceData));
            for (std::size_t index = 0; index < batch.size(); ++index) {
                GpuInstanceData instance;
                instance.model = batch[index].item.transform;
                std::memcpy(instanceBytes.data() + index * sizeof(instance), &instance,
                            sizeof(instance));
            }
            std::vector<std::uint8_t> argumentBytes(batch.size() * sizeof(GpuDrawArguments));
            for (std::size_t index = 0; index < batch.size(); ++index) {
                const GpuDrawArguments arguments{
                    batch[index].item.indexCount, 1u, batch[index].item.firstIndex,
                    batch[index].item.vertexOffset, static_cast<std::uint32_t>(index)};
                std::memcpy(argumentBytes.data() + index * sizeof(arguments), &arguments,
                            sizeof(arguments));
            }
            auto gpuIt = gpuBatches_.find(key);
            if (gpuIt == gpuBatches_.end() ||
                gpuIt->second.instanceDescription.size < instanceBytes.size() ||
                gpuIt->second.argumentDescription.size < argumentBytes.size()) {
                if (gpuIt != gpuBatches_.end()) {
                    renderer.destroy_resource(gpuIt->second.instanceBuffer);
                    renderer.destroy_resource(gpuIt->second.argumentBuffer);
                }
                GpuBatchResources resources;
                resources.key = key;
                resources.instanceDescription = {instanceBytes.size(),
                                                 sizeof(GpuInstanceData),
                                                 false,
                                                 false,
                                                 instanceBytes,
                                                 false,
                                                 true,
                                                 false};
                resources.argumentDescription = {argumentBytes.size(),
                                                 sizeof(GpuDrawArguments),
                                                 false,
                                                 false,
                                                 argumentBytes,
                                                 true,
                                                 false,
                                                 gpuDriven.enabled && gpuDriven.cullingPipeline};
                resources.instanceBuffer = renderer.create_buffer(resources.instanceDescription);
                resources.argumentBuffer = renderer.create_buffer(resources.argumentDescription);
                renderer.update_buffer({resources.argumentBuffer, 0, argumentBytes});
                gpuIt = gpuBatches_.emplace(key, std::move(resources)).first;
            } else {
                gpuIt->second.instanceDescription.initialData = instanceBytes;
                gpuIt->second.argumentDescription.initialData = argumentBytes;
                renderer.update_buffer({gpuIt->second.instanceBuffer, 0, instanceBytes});
                renderer.update_buffer({gpuIt->second.argumentBuffer, 0, argumentBytes});
            }
            graph.import_resource(gpuIt->second.instanceBuffer, gpuIt->second.instanceDescription);
            graph.import_resource(gpuIt->second.argumentBuffer, gpuIt->second.argumentDescription);
            if (gpuDriven.enabled && gpuDriven.cullingPipeline) {
                const auto *cullingDescription =
                    renderer.resource_description(gpuDriven.cullingPipeline);
                if (!cullingDescription)
                    continue;
                graph.import_resource(gpuDriven.cullingPipeline, *cullingDescription);
                const auto cullingMaterial = graph.create_material(
                    {"gpu_culling_" + std::to_string(key),
                     gpuDriven.cullingPipeline,
                     {{"instances",
                       gpuIt->second.instanceBuffer,
                       DescriptorType::StructuredBuffer,
                       0,
                       0,
                       1,
                       false,
                       {}},
                      {"arguments",
                       gpuIt->second.argumentBuffer,
                       DescriptorType::StorageBuffer,
                       1,
                       0,
                       1,
                       false,
                       {}},
                      {"scene", sceneBuffer_, DescriptorType::UniformBuffer, 2, 0, 1, false, {}}},
                     false});
                graph.add_pass(
                    "gpu_cull_" + std::to_string(key),
                    {{cullingMaterial, ResourceUsage::ShaderRead},
                     {gpuIt->second.instanceBuffer, ResourceUsage::ShaderRead},
                     {sceneBuffer_, ResourceUsage::UniformBuffer},
                     {gpuIt->second.argumentBuffer, ResourceUsage::StorageWrite}},
                    [cullingMaterial, sceneBuffer = sceneBuffer_,
                     count = static_cast<std::uint32_t>(batch.size()),
                     groupSize = std::max(1u, gpuDriven.groupSize)](auto &backend, const auto &) {
                        backend.bind_material(cullingMaterial);
                        backend.bind_uniform_buffer(sceneBuffer, 2, 0);
                        backend.dispatch({(count + groupSize - 1u) / groupSize, 1, 1});
                    },
                    RenderQueue::Compute);
            }
            auto batchMaterial = std::get<MaterialDesc>(batch.front().item.materialDescription);
            bindShadows(batchMaterial);
            for (auto &binding : batchMaterial.bindings) {
                if (is_instance_binding(binding))
                    binding.resource = gpuIt->second.instanceBuffer;
                if (lightBuffer_ && binding.type == DescriptorType::StructuredBuffer &&
                    (binding.name == "lights" || binding.name == "lightBuffer"))
                    binding.resource = lightBuffer_;
            }
            const auto hasLightsBinding =
                std::any_of(batchMaterial.bindings.begin(), batchMaterial.bindings.end(),
                            [](const auto &binding) {
                                return binding.type == DescriptorType::StructuredBuffer &&
                                       (binding.name == "lights" || binding.name == "lightBuffer");
                            });
            const auto gpuMaterial = graph.create_material(batchMaterial);
            std::vector<ResourceAccess> accesses{
                {colorTarget, ResourceUsage::ColorAttachment},
                {gpuMaterial, ResourceUsage::ShaderRead},
                {sceneBuffer_, ResourceUsage::ShaderRead},
                {vertexBuffer, ResourceUsage::VertexBuffer},
                {indexBuffer, ResourceUsage::IndexBuffer},
                {gpuIt->second.instanceBuffer, ResourceUsage::ShaderRead},
                {gpuIt->second.argumentBuffer, ResourceUsage::IndirectArguments}};
            if (lightBuffer_ && hasLightsBinding)
                accesses.push_back({lightBuffer_, ResourceUsage::ShaderRead});
            if (depthTarget)
                accesses.push_back({depthTarget, ResourceUsage::DepthStencil});
            append_material_dependencies(batchMaterial, accesses);
            graph.add_pass(
                "gpu_mesh_batch_" + std::to_string(key), std::move(accesses),
                [gpuMaterial, sceneBuffer = sceneBuffer_,
                 draw = IndirectMeshDraw{vertexBuffer, indexBuffer, gpuIt->second.argumentBuffer,
                                         static_cast<std::uint32_t>(batch.size()),
                                         sizeof(GpuDrawArguments), 0}](auto &backend,
                                                                       const auto &) {
                    backend.bind_material(gpuMaterial);
                    backend.bind_uniform_buffer(sceneBuffer, 2, 0);
                    backend.draw_mesh_indirect(draw);
                },
                RenderQueue::Graphics, true, !colorTargetCleared);
            colorTargetCleared = true;
            continue;
        }
        auto batchMaterialDescription =
            std::get<MaterialDesc>(batch.front().item.materialDescription);
        bindShadows(batchMaterialDescription);
        for (auto &binding : batchMaterialDescription.bindings) {
            if (lightBuffer_ && binding.type == DescriptorType::StructuredBuffer &&
                (binding.name == "lights" || binding.name == "lightBuffer"))
                binding.resource = lightBuffer_;
        }
        const auto hasLightsBinding =
            std::any_of(batchMaterialDescription.bindings.begin(),
                        batchMaterialDescription.bindings.end(), [](const auto &binding) {
                            return binding.type == DescriptorType::StructuredBuffer &&
                                   (binding.name == "lights" || binding.name == "lightBuffer");
                        });
        const auto graphMaterial = graph.create_material(batchMaterialDescription);
        std::vector<ResourceAccess> accesses{{colorTarget, ResourceUsage::ColorAttachment},
                                             {graphMaterial, ResourceUsage::ShaderRead},
                                             {sceneBuffer_, ResourceUsage::ShaderRead}};
        if (lightBuffer_ && hasLightsBinding)
            accesses.push_back({lightBuffer_, ResourceUsage::ShaderRead});
        // A material batch may contain many entities sharing one mesh. The
        // graph contract allows one access entry per resource in a pass, so
        // register shared vertex/index buffers once while retaining every
        // per-object uniform buffer for the draw loop below.
        std::unordered_set<std::uint32_t> vertexResources;
        std::unordered_set<std::uint32_t> indexResources;
        vertexResources.reserve(batch.size());
        indexResources.reserve(batch.size());
        for (const auto &mesh : batch) {
            if (vertexResources.insert(mesh.item.vertexBuffer.id).second)
                accesses.push_back({mesh.item.vertexBuffer, ResourceUsage::VertexBuffer});
            if (indexResources.insert(mesh.item.indexBuffer.id).second)
                accesses.push_back({mesh.item.indexBuffer, ResourceUsage::IndexBuffer});
            accesses.push_back({mesh.objectBuffer, ResourceUsage::ShaderRead});
        }
        if (depthTarget)
            accesses.push_back({depthTarget, ResourceUsage::DepthStencil});
        append_material_dependencies(batchMaterialDescription, accesses);
        graph.add_pass(
            "mesh_batch_" + std::to_string(material.id), std::move(accesses),
            [batch = std::move(batch), graphMaterial, sceneBuffer = sceneBuffer_](auto &backend,
                                                                                  const auto &) {
                backend.bind_material(graphMaterial);
                backend.bind_uniform_buffer(sceneBuffer, 2, 0);
                for (const auto &mesh : batch) {
                    backend.bind_uniform_buffer(mesh.objectBuffer, 3, 0);
                    backend.draw_mesh({mesh.item.vertexBuffer, mesh.item.indexBuffer,
                                       mesh.item.indexCount, mesh.item.transform,
                                       mesh.item.firstIndex, mesh.item.vertexOffset});
                }
            },
            RenderQueue::Graphics, true, !colorTargetCleared);
        colorTargetCleared = true;
    }

    for (auto it = objectBuffers_.begin(); it != objectBuffers_.end();) {
        if (seenObjects.find(it->first) != seenObjects.end()) {
            ++it;
        } else {
            renderer.destroy_resource(it->second);
            it = objectBuffers_.erase(it);
        }
    }
    for (auto it = gpuBatches_.begin(); it != gpuBatches_.end();) {
        if (seenGpuBatches.find(it->first) != seenGpuBatches.end()) {
            ++it;
        } else {
            renderer.destroy_resource(it->second.instanceBuffer);
            renderer.destroy_resource(it->second.argumentBuffer);
            it = gpuBatches_.erase(it);
        }
    }

    std::vector<const SpriteRenderItem *> sortedSprites;
    sortedSprites.reserve(scene.sprites().size());
    for (const auto &sprite : scene.sprites())
        sortedSprites.push_back(&sprite);
    std::sort(sortedSprites.begin(), sortedSprites.end(), [](const auto *lhs, const auto *rhs) {
        if (lhs->material.id != rhs->material.id)
            return lhs->material.id < rhs->material.id;
        if (lhs->texture.id != rhs->texture.id)
            return lhs->texture.id < rhs->texture.id;
        return lhs->entity.id < rhs->entity.id;
    });
    for (std::size_t offset = 0; offset < sortedSprites.size();) {
        const auto material = sortedSprites[offset]->material;
        const auto texture = sortedSprites[offset]->texture;
        std::vector<SpriteRenderItem> batch;
        while (offset < sortedSprites.size() && sortedSprites[offset]->material.id == material.id &&
               sortedSprites[offset]->texture.id == texture.id) {
            const auto &sprite = *sortedSprites[offset++];
            batch.push_back(sprite);
            graph.import_texture(sprite.texture, std::get<TextureDesc>(sprite.textureDescription));
            graph.import_resource(sprite.material, sprite.materialDescription);
        }
        graph.add_pass(
            "sprite_batch_" + std::to_string(material.id) + "_" + std::to_string(texture.id),
            {{colorTarget, ResourceUsage::ColorAttachment},
             {texture, ResourceUsage::ShaderRead},
             {material, ResourceUsage::ShaderRead},
             {sceneBuffer_, ResourceUsage::ShaderRead},
             {identityObjectBuffer_, ResourceUsage::ShaderRead}},
            [batch = std::move(batch), material, sceneBuffer = sceneBuffer_,
             identityObjectBuffer = identityObjectBuffer_](auto &backend, const auto &) {
                backend.bind_material(material);
                backend.bind_uniform_buffer(sceneBuffer, 2, 0);
                backend.bind_uniform_buffer(identityObjectBuffer, 3, 0);
                for (const auto &sprite : batch) {
                    backend.draw_sprite(
                        {sprite.texture, sprite.position, sprite.size, sprite.rotation});
                }
            },
            RenderQueue::Graphics, true, !colorTargetCleared);
        colorTargetCleared = true;
    }
    if (!colorTargetCleared) {
        std::vector<ResourceAccess> clear{{colorTarget, ResourceUsage::ColorAttachment}};
        if (depthTarget)
            clear.push_back({depthTarget, ResourceUsage::DepthStencil});
        graph.add_pass("empty_scene_clear", std::move(clear), [](auto &, const auto &) {});
    }
    if (preserveDepth)
        for (auto i = firstPass; i < graph.passes().size(); ++i)
            graph.set_depth_clear(i, false);
}

void ForwardRenderer::build_depth(Renderer &renderer, RenderGraph &graph, const RenderScene &scene,
                                  const RenderView &view, ResourceHandle pipeline,
                                  ResourceHandle depth, const TextureDesc &description) {
    graph.import_resource(depth, description);
    if (const auto *resource = renderer.resource_description(pipeline))
        graph.import_resource(pipeline, *resource);
    const auto uniform = [&](const auto &value) {
        BufferDesc desc;
        desc.size = sizeof(value);
        desc.stride = 16;
        desc.uniformBuffer = true;
        desc.initialData.resize(sizeof(value));
        std::memcpy(desc.initialData.data(), &value, sizeof(value));
        return graph.create_buffer(desc);
    };
    SceneFrameData frame;
    frame.viewProjection = view.viewProjection;
    const auto camera = uniform(frame);
    struct Draw {
        MeshDraw mesh;
        ResourceHandle material;
    };
    std::vector<Draw> draws;
    std::vector<ResourceAccess> accesses{{depth, ResourceUsage::DepthStencil},
                                         {camera, ResourceUsage::UniformBuffer},
                                         {pipeline, ResourceUsage::ShaderRead}};
    const auto append = [&](ResourceHandle handle, ResourceUsage usage) {
        if (std::none_of(accesses.begin(), accesses.end(),
                         [&](const auto &item) { return item.resource.id == handle.id; }))
            accesses.push_back({handle, usage});
    };
    for (const auto &mesh : scene.meshes()) {
        graph.import_resource(mesh.vertexBuffer, mesh.vertexDescription);
        graph.import_resource(mesh.indexBuffer, mesh.indexDescription);
        const auto object = uniform(ObjectFrameData{mesh.transform});
        const auto material =
            graph.create_material({"depth_object",
                                   pipeline,
                                   {{"SceneFrame", camera, DescriptorType::UniformBuffer, 2},
                                    {"ObjectFrame", object, DescriptorType::UniformBuffer, 3}},
                                   false});
        append(object, ResourceUsage::UniformBuffer);
        append(material, ResourceUsage::ShaderRead);
        append(mesh.vertexBuffer, ResourceUsage::VertexBuffer);
        append(mesh.indexBuffer, ResourceUsage::IndexBuffer);
        draws.push_back({{mesh.vertexBuffer, mesh.indexBuffer, mesh.indexCount, mesh.transform,
                          mesh.firstIndex, mesh.vertexOffset},
                         material});
    }
    const auto index = graph.add_pass("scene_depth_prepass", std::move(accesses), {},
                                      RenderQueue::Graphics, true, true);
    graph.set_command_recorder(index, [draws = std::move(draws)](auto &commands, const auto &) {
        for (const auto &draw : draws) {
            commands.bind_material(draw.material);
            commands.draw_mesh(draw.mesh);
        }
    });
}

void ForwardRenderer::release(Renderer &renderer) {
    for (const auto handle : {sceneBuffer_, identityObjectBuffer_, lightBuffer_})
        if (handle)
            renderer.destroy_resource(handle);
    for (const auto &entry : objectBuffers_)
        if (entry.second)
            renderer.destroy_resource(entry.second);
    for (const auto &entry : gpuBatches_) {
        renderer.destroy_resource(entry.second.instanceBuffer);
        renderer.destroy_resource(entry.second.argumentBuffer);
    }
    objectBuffers_.clear();
    gpuBatches_.clear();
    shadowCascades_.clear();
    sceneBuffer_ = identityObjectBuffer_ = lightBuffer_ = shadowFrame_ = shadowAtlas_ = {};
    shadowGeneration_ = 0;
}

void ForwardRenderer::build_shadows(Renderer &renderer, const RenderScene &scene,
                                    const ShadowSettings &settings, ResourceHandle shadowPipeline,
                                    ResourceHandle shadowTarget,
                                    const TextureDesc &shadowDescription,
                                    RenderGraph *graphOverride) {
    shadowCascades_.clear();
    shadowFrame_ = shadowAtlas_ = {};
    shadowGeneration_ = 0;
    if (!settings.enabled || !shadowPipeline || !shadowTarget || scene.lights().empty())
        return;
    const auto cascadeCount = std::clamp(settings.cascadeCount, 1u, 8u);
    const auto nearPlane = std::max(0.001f, scene.view().projection.nearPlane);
    const auto farPlane = std::max(
        nearPlane + 0.001f, std::min(scene.view().projection.farPlane, settings.maxDistance));
    const auto directional =
        std::find_if(scene.lights().begin(), scene.lights().end(), [](const LightRenderItem &item) {
            return item.light.type == LightType::Directional && item.light.castsShadow;
        });
    if (directional == scene.lights().end())
        return;
    const auto direction = math::Normalize(light_direction(*directional));
    shadowCascades_.reserve(cascadeCount);
    for (std::uint32_t index = 0; index < cascadeCount; ++index) {
        const auto ratio = static_cast<float>(index + 1u) / static_cast<float>(cascadeCount);
        const auto previousRatio = static_cast<float>(index) / static_cast<float>(cascadeCount);
        ShadowCascade cascade;
        cascade.index = index;
        cascade.nearDistance = nearPlane * std::pow(farPlane / nearPlane, previousRatio);
        cascade.farDistance = nearPlane * std::pow(farPlane / nearPlane, ratio);
        const auto tileCount =
            static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<float>(cascadeCount))));
        cascade.atlasSize =
            std::max(1u, std::min(shadowDescription.width, shadowDescription.height) / tileCount);
        cascade.atlasX = (index % tileCount) * cascade.atlasSize;
        cascade.atlasY = (index / tileCount) * cascade.atlasSize;
        const auto corners =
            cascade_corners(scene.view(), cascade.nearDistance, cascade.farDistance);
        math::Vec3 center{};
        for (const auto point : corners.points)
            center = center + point;
        center = center / static_cast<float>(corners.points.size());
        float radius = 0.0f;
        for (const auto point : corners.points)
            radius = std::max(radius, math::Length(point - center));
        radius = std::max(radius, 1.0f) + settings.bias;
        const auto lightPosition = center - direction * (radius * 2.0f);
        const auto lightView = math::LookAt(lightPosition, center);
        math::Vec3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                           std::numeric_limits<float>::max()};
        math::Vec3 maximum{std::numeric_limits<float>::lowest(),
                           std::numeric_limits<float>::lowest(),
                           std::numeric_limits<float>::lowest()};
        for (const auto point : corners.points) {
            const auto lightPoint = math::TransformPoint(lightView, point);
            minimum.x = std::min(minimum.x, lightPoint.x);
            minimum.y = std::min(minimum.y, lightPoint.y);
            minimum.z = std::min(minimum.z, lightPoint.z);
            maximum.x = std::max(maximum.x, lightPoint.x);
            maximum.y = std::max(maximum.y, lightPoint.y);
            maximum.z = std::max(maximum.z, lightPoint.z);
        }
        const float extent = std::max({maximum.x - minimum.x, maximum.y - minimum.y, 1.0f});
        const float tileResolution = static_cast<float>(std::max(1u, cascade.atlasSize));
        const float texelSize = extent / tileResolution;
        auto lightCenter = (minimum + maximum) * 0.5f;
        if (texelSize > 0.00001f) {
            lightCenter.x = std::floor(lightCenter.x / texelSize) * texelSize;
            lightCenter.y = std::floor(lightCenter.y / texelSize) * texelSize;
        }
        minimum.x = lightCenter.x - extent * 0.5f;
        maximum.x = lightCenter.x + extent * 0.5f;
        minimum.y = lightCenter.y - extent * 0.5f;
        maximum.y = lightCenter.y + extent * 0.5f;
        minimum.z -= radius;
        maximum.z += radius;
        cascade.viewProjection = math::Multiply(
            math::Orthographic(minimum.x, maximum.x, minimum.y, maximum.y, minimum.z, maximum.z),
            lightView);
        shadowCascades_.push_back(cascade);
    }
    auto &graph = graphOverride ? *graphOverride : renderer.graph();
    graph.import_resource(shadowTarget, shadowDescription);
    if (const auto *pipelineDescription = renderer.resource_description(shadowPipeline)) {
        graph.import_resource(shadowPipeline, *pipelineDescription);
    }
    const auto make_uniform = [&](const auto &value) {
        BufferDesc description;
        description.size = sizeof(value);
        description.stride = 16;
        description.uniformBuffer = true;
        description.initialData.resize(sizeof(value));
        std::memcpy(description.initialData.data(), &value, sizeof(value));
        return graph.create_buffer(description);
    };
    ShadowFrameData shadowData;
    const auto forward =
        math::Normalize(math::TransformPoint(transform_matrix(scene.view().transform), {0, 0, 1}) -
                        scene.view().transform.position);
    const auto position = scene.view().transform.position;
    shadowData.cameraPosition = {position.x, position.y, position.z, 1};
    shadowData.cameraForward = {forward.x, forward.y, forward.z, 0};
    shadowData.settings = {static_cast<float>(cascadeCount), settings.bias,
                           1.0f / std::max(1u, shadowDescription.width),
                           static_cast<float>(std::distance(scene.lights().begin(), directional))};
    for (const auto &cascade : shadowCascades_) {
        shadowData.viewProjection[cascade.index] = cascade.viewProjection;
        shadowData.distances[cascade.index] = {cascade.nearDistance, cascade.farDistance, 0, 0};
        shadowData.atlasRect[cascade.index] = {
            static_cast<float>(cascade.atlasX) / std::max(1u, shadowDescription.width),
            static_cast<float>(cascade.atlasY) / std::max(1u, shadowDescription.height),
            static_cast<float>(cascade.atlasSize) / std::max(1u, shadowDescription.width),
            static_cast<float>(cascade.atlasSize) / std::max(1u, shadowDescription.height)};
    }
    shadowFrame_ = make_uniform(shadowData);
    shadowAtlas_ = shadowTarget;
    shadowGeneration_ = graph.generation();
    struct ShadowDraw {
        MeshDraw draw;
        ResourceHandle object;
    };
    std::vector<ShadowDraw> draws;
    for (const auto &draw : scene.shadow_meshes()) {
        const auto *vertex = renderer.resource_description(draw.vertexBuffer);
        const auto *index = renderer.resource_description(draw.indexBuffer);
        if (!vertex || !index)
            continue;
        graph.import_resource(draw.vertexBuffer, *vertex);
        graph.import_resource(draw.indexBuffer, *index);
        draws.push_back({draw, make_uniform(ObjectFrameData{draw.transform})});
    }
    for (const auto &cascade : shadowCascades_) {
        SceneFrameData frame;
        frame.viewProjection = cascade.viewProjection;
        const auto frameBuffer = make_uniform(frame);
        struct BoundDraw {
            MeshDraw draw;
            ResourceHandle material;
        };
        std::vector<BoundDraw> boundDraws;
        std::vector<ResourceAccess> accesses{{shadowTarget, ResourceUsage::DepthStencil},
                                             {frameBuffer, ResourceUsage::UniformBuffer}};
        const auto add_access = [&](ResourceHandle handle, ResourceUsage usage) {
            if (std::none_of(accesses.begin(), accesses.end(),
                             [&](const auto &value) { return value.resource.id == handle.id; }))
                accesses.push_back({handle, usage});
        };
        for (const auto &draw : draws) {
            const auto material = graph.create_material(
                {"shadow_cascade_material",
                 shadowPipeline,
                 {{"SceneFrame", frameBuffer, DescriptorType::UniformBuffer, 2},
                  {"ObjectFrame", draw.object, DescriptorType::UniformBuffer, 3}},
                 false});
            boundDraws.push_back({draw.draw, material});
            add_access(draw.draw.vertexBuffer, ResourceUsage::VertexBuffer);
            add_access(draw.draw.indexBuffer, ResourceUsage::IndexBuffer);
            add_access(draw.object, ResourceUsage::UniformBuffer);
            add_access(material, ResourceUsage::ShaderRead);
        }
        graph.add_pass(
            "shadow_cascade_" + std::to_string(cascade.index), std::move(accesses),
            [draws = std::move(boundDraws), cascade](auto &backend, const auto &) {
                backend.set_viewport(
                    static_cast<float>(cascade.atlasX), static_cast<float>(cascade.atlasY),
                    static_cast<float>(cascade.atlasSize), static_cast<float>(cascade.atlasSize));
                for (const auto &draw : draws) {
                    backend.bind_material(draw.material);
                    backend.draw_mesh(draw.draw);
                }
            },
            RenderQueue::Graphics, true, cascade.index == 0);
    }
}

ResourceHandle ForwardRenderer::prepare_present(Renderer &renderer) {
    SceneFrameData frameData;
    frameData.viewProjection = math::Mat4::Identity();
    frameData.cameraPositionAndFlags[3] = 2.0f;
    std::vector<std::uint8_t> frameBytes(sizeof(frameData));
    std::memcpy(frameBytes.data(), &frameData, frameBytes.size());
    if (sceneBuffer_)
        renderer.update_buffer({sceneBuffer_, 0, frameBytes});
    if (!identityObjectBuffer_) {
        ObjectFrameData objectData;
        std::vector<std::uint8_t> objectBytes(sizeof(objectData));
        std::memcpy(objectBytes.data(), &objectData, objectBytes.size());
        identityObjectDescription_ = {sizeof(objectData), sizeof(float) * 4, false, false,
                                      objectBytes};
        identityObjectDescription_.uniformBuffer = true;
        identityObjectBuffer_ = renderer.create_buffer(identityObjectDescription_);
    }
    return identityObjectBuffer_;
}
} // namespace shinkou::render

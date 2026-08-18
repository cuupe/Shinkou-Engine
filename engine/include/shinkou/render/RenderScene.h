#pragma once

#include "shinkou/Math.h"
#include "shinkou/World.h"
#include "shinkou/render/RenderTypes.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace shinkou::render {
class Renderer;

struct TransformComponent {
    math::Transform local{};
    bool visible{true};
};

struct CameraComponent {
    float verticalFieldOfView{math::Pi / 3.0f};
    float orthographicSize{10.0f};
    float nearPlane{0.05f};
    float farPlane{1000.0f};
    bool orthographic{false};
    bool active{true};
};

struct MeshRendererComponent {
    ResourceHandle vertexBuffer{};
    ResourceHandle indexBuffer{};
    ResourceHandle material{};
    std::uint32_t indexCount{0};
    bool visible{true};
    bool gpuDriven{false};
    math::Vec3 boundsCenter{};
    float boundsRadius{1.0f};
};

struct SpriteRendererComponent {
    ResourceHandle texture{};
    ResourceHandle material{};
    math::Vec2 size{1.0f, 1.0f};
    float rotation{0.0f};
    bool visible{true};
};

enum class LightType { Directional, Point, Spot };

struct LightComponent {
    LightType type{LightType::Directional};
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity{1.0f};
    float range{10.0f};
    float innerCone{0.5f};
    float outerCone{0.8f};
    bool castsShadow{false};
    bool visible{true};
};

struct ShadowSettings {
    std::uint32_t atlasSize{2048};
    std::uint32_t cascadeCount{4};
    float maxDistance{100.0f};
    float bias{0.001f};
    bool enabled{true};
};

struct ShadowCascade {
    std::uint32_t index{0};
    math::Mat4 viewProjection{math::Mat4::Identity()};
    float nearDistance{0.0f};
    float farDistance{0.0f};
    std::uint32_t atlasX{0};
    std::uint32_t atlasY{0};
    std::uint32_t atlasSize{0};
};

struct RenderView {
    Entity camera{};
    math::Transform transform{};
    CameraComponent projection{};
    math::Mat4 viewMatrix{math::Mat4::Identity()};
    math::Mat4 projectionMatrix{math::Mat4::Identity()};
    math::Mat4 viewProjection{math::Mat4::Identity()};
};

struct RenderSceneStats {
    std::size_t extractedMeshes{0};
    std::size_t visibleMeshes{0};
    std::size_t culledMeshes{0};
    std::size_t extractedSprites{0};
};

struct GpuDrivenSettings {
    bool enabled{false};
    ResourceHandle cullingPipeline{};
    std::uint32_t groupSize{64};
};

struct LightRenderItem {
    Entity entity{};
    LightComponent light{};
    math::Transform transform{};
};

struct SceneFrameData {
    math::Mat4 viewProjection{math::Mat4::Identity()};
    float cameraPositionAndFlags[4]{0.0f, 0.0f, 0.0f, 0.0f};
};

struct ObjectFrameData {
    math::Mat4 model{math::Mat4::Identity()};
};

struct MeshRenderItem {
    Entity entity{};
    ResourceHandle vertexBuffer{};
    ResourceHandle indexBuffer{};
    ResourceHandle material{};
    std::uint32_t indexCount{0};
    math::Mat4 transform{math::Mat4::Identity()};
    ResourceDesc vertexDescription{};
    ResourceDesc indexDescription{};
    ResourceDesc materialDescription{};
    bool gpuDriven{false};
    math::Vec3 boundsCenter{};
    float boundsRadius{1.0f};
};

struct SpriteRenderItem {
    Entity entity{};
    ResourceHandle texture{};
    ResourceHandle material{};
    math::Vec2 position{};
    math::Vec2 size{1.0f, 1.0f};
    float rotation{0.0f};
    ResourceDesc textureDescription{};
    ResourceDesc materialDescription{};
};

class RenderScene {
    std::vector<MeshRenderItem> meshes_;
    std::vector<SpriteRenderItem> sprites_;
    std::vector<LightRenderItem> lights_;
    RenderView view_{};
public:
    void extract(const World& world, const Renderer& renderer, float aspectRatio = 16.0f / 9.0f);
    const std::vector<MeshRenderItem>& meshes() const noexcept { return meshes_; }
    const std::vector<SpriteRenderItem>& sprites() const noexcept { return sprites_; }
    const std::vector<LightRenderItem>& lights() const noexcept { return lights_; }
    const RenderView& view() const noexcept { return view_; }
    const RenderSceneStats& stats() const noexcept { return stats_; }
private:
    RenderSceneStats stats_{};
};

class ForwardRenderer {
    ResourceHandle sceneBuffer_{};
    BufferDesc sceneBufferDescription_{};
    ResourceHandle identityObjectBuffer_{};
    BufferDesc identityObjectDescription_{};
    std::unordered_map<Entity, ResourceHandle, EntityHash> objectBuffers_;
    struct GpuBatchResources {
        std::uint64_t key{0};
        ResourceHandle instanceBuffer{};
        ResourceHandle argumentBuffer{};
        BufferDesc instanceDescription{};
        BufferDesc argumentDescription{};
    };
    std::unordered_map<std::uint64_t, GpuBatchResources> gpuBatches_;
    ResourceHandle lightBuffer_{};
    BufferDesc lightBufferDescription_{};
    std::vector<ShadowCascade> shadowCascades_;
public:
    void build(Renderer& renderer, const RenderScene& scene,
               ResourceHandle colorTarget, const TextureDesc& colorDescription,
               ResourceHandle depthTarget = {}, const TextureDesc& depthDescription = {},
               const GpuDrivenSettings& gpuDriven = {});
    void build_shadows(Renderer& renderer, const RenderScene& scene, const ShadowSettings& settings,
                       ResourceHandle shadowPipeline, ResourceHandle shadowTarget,
                       const TextureDesc& shadowDescription);
    const std::vector<ShadowCascade>& shadow_cascades() const noexcept { return shadowCascades_; }
    ResourceHandle prepare_present(Renderer& renderer);
    ResourceHandle scene_buffer() const noexcept { return sceneBuffer_; }
    ResourceHandle light_buffer() const noexcept { return lightBuffer_; }
};
}

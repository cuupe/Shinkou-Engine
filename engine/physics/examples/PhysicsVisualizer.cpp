#include "shinkou/Engine.h"
#include "shinkou/render/RenderScene.h"

#if defined(SHINKOU_PHYSICS_VISUALIZER_WITH_IMGUI)
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_vulkan.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using shinkou::Entity;
using shinkou::World;
using shinkou::math::Quat;
using shinkou::math::Vec3;
using shinkou::physics::BodyId;
using shinkou::physics::BodyState;
using shinkou::physics::ContactEvent;
using shinkou::physics::ContactPhase;
using shinkou::physics::IPhysicsWorld;
using shinkou::physics::PhysicsMaterialDesc;
using shinkou::render::BufferDesc;
using shinkou::render::MaterialDesc;
using shinkou::render::PipelineDesc;
using shinkou::render::Renderer;
using shinkou::render::ResourceHandle;
using shinkou::render::ShaderDesc;
using shinkou::render::ShaderStage;
using shinkou::render::TextureDesc;

#ifndef SHINKOU_PHYSICS_SHADER_DIR
#define SHINKOU_PHYSICS_SHADER_DIR ""
#endif

enum class TestMode : std::uint8_t {
    RigidBodyDrop,
    Collision,
    DensePerformance
};

struct RenderBody {
    BodyId body{shinkou::physics::InvalidBodyId};
    Entity entity{};
    Entity shadowEntity{};
    ResourceHandle material{};
};

struct VisualState {
    TestMode mode{TestMode::RigidBodyDrop};
    std::vector<RenderBody> dynamicBodies;
    std::vector<BodyId> sceneBodies;
    std::vector<Entity> sceneEntities;
    BodyId primaryBody{shinkou::physics::InvalidBodyId};
    BodyId secondaryBody{shinkou::physics::InvalidBodyId};
    std::vector<ContactEvent> recentContacts;
    std::uint64_t totalContactEvents{0};
    float lastImpactSpeed{0.0f};
    BodyId lastContactA{shinkou::physics::InvalidBodyId};
    BodyId lastContactB{shinkou::physics::InvalidBodyId};
    ContactPhase lastContactPhase{ContactPhase::Begin};
};

struct TestParameters {
    TestMode mode{TestMode::RigidBodyDrop};
    float gravityY{-9.81f};
    float dropHeight{6.0f};
    float dropMass{1.0f};
    float dropSize{1.0f};
    float dropRestitution{0.35f};
    float dropFriction{0.5f};
    float collisionSpeed{7.0f};
    float collisionMassA{1.0f};
    float collisionMassB{1.0f};
    float collisionGap{4.0f};
    float collisionSize{1.2f};
    float collisionRestitution{0.6f};
    float collisionFriction{0.35f};
    int denseBodyCount{256};
    float denseSpacing{0.95f};
    float denseSize{0.85f};
    float denseRestitution{0.12f};
    float denseFriction{0.55f};
    bool simulationRunning{true};
};

struct SceneResources {
    ResourceHandle vertexBuffer{};
    ResourceHandle indexBuffer{};
    ResourceHandle sampler{};
    ResourceHandle groundMaterial{};
    ResourceHandle dropMaterial{};
    ResourceHandle collisionAMaterial{};
    ResourceHandle collisionBMaterial{};
    ResourceHandle markerMaterial{};
    ResourceHandle shadowMaterial{};
};

struct ControlActions {
    bool rebuild{false};
    bool launch{false};
    bool resetContactHistory{false};
};

const char* test_mode_name(TestMode mode) noexcept {
    switch (mode) {
    case TestMode::RigidBodyDrop: return "drop";
    case TestMode::Collision: return "collision";
    case TestMode::DensePerformance: return "dense-physx";
    }
    return "unknown";
}

std::vector<std::uint8_t> load_binary_shader(const std::string& name) {
    const std::string path = std::string(SHINKOU_PHYSICS_SHADER_DIR) + "/" + name;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg();
    if (size <= 0) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file ? bytes : std::vector<std::uint8_t>{};
}

const char* backend_name(shinkou::render::BackendApi api) noexcept {
    switch (api) {
    case shinkou::render::BackendApi::DirectX11: return "DirectX11";
    case shinkou::render::BackendApi::DirectX12: return "DirectX12";
    case shinkou::render::BackendApi::Vulkan: return "Vulkan";
    case shinkou::render::BackendApi::Null: return "Null";
    }
    return "Unknown";
}

void configure_shader(ShaderDesc& description, shinkou::render::BackendApi api,
                      const std::string& hlslSource, const std::string& binaryName,
                      const std::string& entryPoint, const std::string& profile) {
    description.entryPoint = entryPoint;
    if (api == shinkou::render::BackendApi::Vulkan) {
        description.sourceKind = shinkou::render::ShaderSourceKind::SpirV;
        description.bytecode = load_binary_shader(binaryName + ".spv");
    } else if (api == shinkou::render::BackendApi::DirectX12) {
        description.sourceKind = shinkou::render::ShaderSourceKind::Dxil;
        description.bytecode = load_binary_shader(binaryName + ".dxil");
    } else if (api == shinkou::render::BackendApi::DirectX11) {
        description.profile = profile;
        description.source = hlslSource;
    }
}

TextureDesc solid_texture(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    TextureDesc description;
    description.width = 1;
    description.height = 1;
    description.format = "rgba8";
    description.initialData = {red, green, blue, 255};
    return description;
}

ResourceHandle create_solid_texture(Renderer& renderer, std::uint8_t red,
                                    std::uint8_t green, std::uint8_t blue) {
    return renderer.create_texture(solid_texture(red, green, blue));
}

MaterialDesc material_description(const std::string& name, ResourceHandle pipeline,
                                  ResourceHandle texture, ResourceHandle sampler) {
    return {name, pipeline,
        {{"albedo", texture, shinkou::render::DescriptorType::Texture, 0, 0},
         {"sampler", sampler, shinkou::render::DescriptorType::Sampler, 1, 0}}, false};
}

Entity add_box_visual(World& world, const SceneResources& resources,
                      ResourceHandle material, Vec3 position, Vec3 scale,
                      Quat rotation = Quat::Identity(), float boundsRadius = 2.0f) {
    const auto entity = world.ecs().create();
    shinkou::render::TransformComponent transform;
    transform.local.position = position;
    transform.local.rotation = rotation;
    transform.local.scale = scale;
    world.ecs().emplace<shinkou::render::TransformComponent>(entity, transform);

    shinkou::render::MeshRendererComponent mesh;
    mesh.vertexBuffer = resources.vertexBuffer;
    mesh.indexBuffer = resources.indexBuffer;
    mesh.material = material;
    mesh.indexCount = 36;
    mesh.visible = true;
    mesh.boundsRadius = boundsRadius;
    world.ecs().emplace<shinkou::render::MeshRendererComponent>(entity, mesh);
    return entity;
}

void destroy_scene(World& world, IPhysicsWorld& physics, VisualState& visualState) {
    for (const auto body : visualState.sceneBodies) physics.destroy_body(body);
    for (const auto entity : visualState.sceneEntities) world.ecs().destroy(entity);
    visualState.dynamicBodies.clear();
    visualState.sceneBodies.clear();
    visualState.sceneEntities.clear();
    visualState.primaryBody = shinkou::physics::InvalidBodyId;
    visualState.secondaryBody = shinkou::physics::InvalidBodyId;
    visualState.recentContacts.clear();
    visualState.totalContactEvents = 0;
    visualState.lastImpactSpeed = 0.0f;
    visualState.lastContactA = shinkou::physics::InvalidBodyId;
    visualState.lastContactB = shinkou::physics::InvalidBodyId;
}

PhysicsMaterialDesc physics_material(float friction, float restitution) {
    PhysicsMaterialDesc material;
    material.staticFriction = friction;
    material.dynamicFriction = friction;
    material.restitution = restitution;
    return material;
}

BodyId add_physics_box(World& world, IPhysicsWorld& physics, VisualState& visualState,
                       const SceneResources& resources, Vec3 position, Vec3 halfExtents,
                       ResourceHandle material, bool dynamic, float mass,
                       const PhysicsMaterialDesc& physicsMaterial, Vec3 velocity = {},
                       Quat rotation = Quat::Identity(), std::uint64_t userData = 0,
                       bool createShadow = true) {
    shinkou::physics::BodyDesc body;
    body.position = position;
    body.velocity = velocity;
    body.rotation = rotation;
    body.halfExtents = halfExtents;
    body.mass = mass;
    body.dynamic = dynamic;
    body.userData = userData;
    auto shape = shinkou::physics::ShapeDesc::box(halfExtents);
    shape.material = physicsMaterial;
    body.shapes.push_back(shape);
    const auto bodyId = physics.create_body(body);
    if (bodyId == shinkou::physics::InvalidBodyId) return bodyId;

    const auto entity = add_box_visual(world, resources, material, position, halfExtents,
                                       rotation, std::max(halfExtents.x, std::max(halfExtents.y, halfExtents.z)) * 2.2f);
    visualState.sceneBodies.push_back(bodyId);
    visualState.sceneEntities.push_back(entity);
    if (dynamic) {
        Entity shadowEntity{};
        if (createShadow) {
        const Vec3 shadowHalfExtents{
            std::max(halfExtents.x * 1.05f, 0.12f),
            0.012f,
            std::max(halfExtents.z * 0.72f, 0.10f)};
            shadowEntity = add_box_visual(
                world, resources, resources.shadowMaterial,
                {position.x, 0.015f, position.z}, shadowHalfExtents,
                Quat::Identity(), std::max(shadowHalfExtents.x, shadowHalfExtents.z) * 2.2f);
            visualState.sceneEntities.push_back(shadowEntity);
        }
        visualState.dynamicBodies.push_back({bodyId, entity, shadowEntity, material});
    }
    return bodyId;
}

bool build_scene(World& world, IPhysicsWorld& physics, VisualState& visualState,
                 const SceneResources& resources, const TestParameters& parameters) {
    destroy_scene(world, physics, visualState);
    visualState.mode = parameters.mode;
    physics.set_gravity({0.0f, parameters.gravityY, 0.0f});

    const PhysicsMaterialDesc groundMaterial = physics_material(0.65f, 0.1f);
    Vec3 groundHalfExtents{6.0f, 0.25f, 3.0f};
    if (parameters.mode == TestMode::DensePerformance) {
        const int bodyCount = std::clamp(parameters.denseBodyCount, 32, 4096);
        const int gridSide = std::max(1, static_cast<int>(std::ceil(
            std::cbrt(static_cast<double>(bodyCount)))));
        const float spacing = std::clamp(parameters.denseSpacing, 0.75f, 1.2f);
        const float halfSize = std::clamp(parameters.denseSize * 0.5f, 0.25f, 0.6f);
        const float extent = std::max(6.0f,
                                      static_cast<float>(gridSide) * spacing * 0.60f + halfSize);
        groundHalfExtents = {extent, 0.25f, extent};
    }
    const auto ground = add_physics_box(world, physics, visualState, resources,
                                        {0.0f, -0.25f, 0.0f}, groundHalfExtents,
                                        resources.groundMaterial, false, 0.0f,
                                        groundMaterial, {}, Quat::Identity(), 9000u);
    if (ground == shinkou::physics::InvalidBodyId) return false;

    if (parameters.mode == TestMode::RigidBodyDrop) {
        const float halfSize = std::clamp(parameters.dropSize * 0.5f, 0.15f, 1.5f);
        const PhysicsMaterialDesc material = physics_material(parameters.dropFriction,
                                                               parameters.dropRestitution);
        visualState.primaryBody = add_physics_box(
            world, physics, visualState, resources,
            {0.0f, std::max(parameters.dropHeight, halfSize + 0.35f), 0.0f},
            {halfSize, halfSize, halfSize}, resources.dropMaterial, true,
            std::max(parameters.dropMass, 0.05f), material, {}, Quat::Identity(), 1u);
        if (visualState.primaryBody == shinkou::physics::InvalidBodyId) return false;
    } else if (parameters.mode == TestMode::Collision) {
        const float halfSize = std::clamp(parameters.collisionSize * 0.5f, 0.15f, 1.5f);
        const float centerOffset = std::clamp(parameters.collisionGap * 0.5f + halfSize,
                                              halfSize + 0.5f, 5.0f);
        const PhysicsMaterialDesc material = physics_material(parameters.collisionFriction,
                                                               parameters.collisionRestitution);
        visualState.primaryBody = add_physics_box(
            world, physics, visualState, resources,
            {-centerOffset, halfSize + 0.3f, 0.0f}, {halfSize, halfSize, halfSize},
            resources.collisionAMaterial, true, std::max(parameters.collisionMassA, 0.05f),
            material, {parameters.collisionSpeed, 0.0f, 0.0f}, Quat::Identity(), 101u);
        visualState.secondaryBody = add_physics_box(
            world, physics, visualState, resources,
            {centerOffset, halfSize + 0.3f, 0.0f}, {halfSize, halfSize, halfSize},
            resources.collisionBMaterial, true, std::max(parameters.collisionMassB, 0.05f),
            material, {-parameters.collisionSpeed, 0.0f, 0.0f}, Quat::Identity(), 102u);
        if (visualState.primaryBody == shinkou::physics::InvalidBodyId ||
            visualState.secondaryBody == shinkou::physics::InvalidBodyId) return false;
    } else {
        const int bodyCount = std::clamp(parameters.denseBodyCount, 32, 4096);
        const int gridSide = std::max(1, static_cast<int>(std::ceil(
            std::cbrt(static_cast<double>(bodyCount)))));
        const int layerArea = gridSide * gridSide;
        const float spacing = std::clamp(parameters.denseSpacing, 0.75f, 1.2f);
        const float halfSize = std::clamp(parameters.denseSize * 0.5f, 0.25f, 0.6f);
        const float center = static_cast<float>(gridSide - 1) * 0.5f;
        const PhysicsMaterialDesc material = physics_material(parameters.denseFriction,
                                                               parameters.denseRestitution);
        const ResourceHandle materials[] = {
            resources.dropMaterial, resources.collisionAMaterial, resources.collisionBMaterial};
        for (int index = 0; index < bodyCount; ++index) {
            const int layer = index / layerArea;
            const int inLayer = index % layerArea;
            const int row = inLayer / gridSide;
            const int column = inLayer % gridSide;
            const Vec3 position{
                (static_cast<float>(column) - center) * spacing,
                halfSize + 0.08f + static_cast<float>(layer) * spacing,
                (static_cast<float>(row) - center) * spacing};
            const auto body = add_physics_box(
                world, physics, visualState, resources, position,
                {halfSize, halfSize, halfSize}, materials[index % 3], true, 1.0f,
                material, {}, Quat::Identity(), static_cast<std::uint64_t>(index + 1), false);
            if (body == shinkou::physics::InvalidBodyId) return false;
        }
    }
    return true;
}

void sync_visuals(World& world, IPhysicsWorld& physics, const VisualState& visualState) {
    // The light is above and to the positive X/Z side of the scene. A shadow
    // is projected away from it onto the ground plane, so it is visibly
    // directional instead of being glued directly under the body.
    constexpr Vec3 lightDirection{0.45f, 1.0f, 0.35f};
    for (const auto& renderBody : visualState.dynamicBodies) {
        BodyState state;
        if (!physics.get_body_state(renderBody.body, state)) continue;
        auto* transform = world.ecs().try_get<shinkou::render::TransformComponent>(renderBody.entity);
        if (!transform) continue;
        transform->local.position = state.position;
        transform->local.rotation = state.rotation;

        if (!renderBody.shadowEntity) continue;
        auto* shadow = world.ecs().try_get<shinkou::render::TransformComponent>(renderBody.shadowEntity);
        if (!shadow) continue;

        // A thin dark box is deliberately used instead of a backend-specific shadow map:
        // it works on the existing DX11/Vulkan render paths and remains easy to inspect.
        const float bodyBottom = state.position.y - std::abs(transform->local.scale.y);
        const float heightAboveGround = std::max(0.0f, bodyBottom);
        const float heightFactor = std::clamp(heightAboveGround / 6.0f, 0.0f, 1.0f);
        const float shadowOffsetX = -lightDirection.x / lightDirection.y * heightAboveGround;
        const float shadowOffsetZ = -lightDirection.z / lightDirection.y * heightAboveGround;
        const float shadowDistance = std::sqrt(shadowOffsetX * shadowOffsetX + shadowOffsetZ * shadowOffsetZ);
        const float shadowAngle = std::atan2(shadowOffsetX, shadowOffsetZ);
        shadow->local.position = {
            state.position.x + shadowOffsetX,
            0.015f,
            state.position.z + shadowOffsetZ};
        shadow->local.rotation = shinkou::math::FromAxisAngle({0.0f, 1.0f, 0.0f}, shadowAngle);
        shadow->local.scale = {
            std::max(0.12f, std::abs(transform->local.scale.x) * (0.85f + 0.15f * heightFactor)),
            0.012f,
            std::max(0.10f, std::abs(transform->local.scale.z) * 0.72f + shadowDistance * 0.32f)};
    }
}

void freeze_dynamic_bodies(IPhysicsWorld& physics, const VisualState& visualState) {
    for (const auto& body : visualState.dynamicBodies) {
        physics.set_linear_velocity(body.body, {});
        physics.set_angular_velocity(body.body, {});
    }
}

void launch_test(IPhysicsWorld& physics, const VisualState& visualState,
                 const TestParameters& parameters) {
    if (parameters.mode == TestMode::RigidBodyDrop) {
        if (visualState.primaryBody != shinkou::physics::InvalidBodyId)
            physics.set_linear_velocity(visualState.primaryBody, {0.0f, 10.0f, 0.0f});
    } else if (parameters.mode == TestMode::Collision) {
        if (visualState.primaryBody != shinkou::physics::InvalidBodyId)
            physics.set_linear_velocity(visualState.primaryBody, {parameters.collisionSpeed, 0.0f, 0.0f});
        if (visualState.secondaryBody != shinkou::physics::InvalidBodyId)
            physics.set_linear_velocity(visualState.secondaryBody, {-parameters.collisionSpeed, 0.0f, 0.0f});
    } else {
        for (const auto& body : visualState.dynamicBodies)
            physics.set_linear_velocity(body.body, {0.0f, 1.5f, 0.0f});
    }
}

const char* contact_phase_name(ContactPhase phase) noexcept {
    switch (phase) {
    case ContactPhase::Begin: return "Begin";
    case ContactPhase::Persist: return "Persist";
    case ContactPhase::End: return "End";
    }
    return "Unknown";
}

#if defined(SHINKOU_PHYSICS_VISUALIZER_WITH_IMGUI)
void begin_imgui_frame(shinkou::Engine& engine, Renderer& renderer, float deltaSeconds) {
    auto& io = ImGui::GetIO();
    const auto& mouse = engine.input().mouse();
    const float dpiScale = std::clamp(engine.window().dpi_scale(), 0.75f, 2.0f);
    io.DisplaySize = ImVec2(static_cast<float>(engine.window().width()),
                            static_cast<float>(engine.window().height()));
    io.DeltaTime = std::max(deltaSeconds, 1.0f / 1000.0f);
    io.FontGlobalScale = dpiScale;
    io.MousePos = ImVec2(mouse.position.x, mouse.position.y);
    // SDL mouse button values are 1=left, 2=middle, 3=right, while ImGui
    // expects 0=left, 1=right, 2=middle. Do not copy the array by index.
    io.MouseDown[0] = mouse.buttons[1];
    io.MouseDown[1] = mouse.buttons[3];
    io.MouseDown[2] = mouse.buttons[2];
    io.MouseDown[3] = mouse.buttons[4];
    io.MouseDown[4] = mouse.buttons[5];
    io.MouseWheel = mouse.wheel.y;
    io.MouseWheelH = mouse.wheel.x;
    if (renderer.capabilities().api == shinkou::render::BackendApi::DirectX11)
        ImGui_ImplDX11_NewFrame();
    else if (renderer.capabilities().api == shinkou::render::BackendApi::DirectX12)
        ImGui_ImplDX12_NewFrame();
    else if (renderer.capabilities().api == shinkou::render::BackendApi::Vulkan)
        ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
}

ControlActions draw_imgui_controls(shinkou::Engine& engine, IPhysicsWorld& physics, VisualState& visualState,
                                   TestParameters& parameters) {
    ControlActions actions;
    const auto slider = [&actions](const char* label, float* value, float minimum,
                                   float maximum, const char* format) {
        ImGui::SliderFloat(label, value, minimum, maximum, format);
        // Apply once when the user releases the slider, instead of rebuilding
        // the physics world on every pixel of a drag operation.
        if (ImGui::IsItemDeactivatedAfterEdit()) actions.rebuild = true;
    };
    const float dpiScale = std::clamp(engine.window().dpi_scale(), 0.75f, 2.0f);
    const float displayWidth = static_cast<float>(engine.window().width());
    const float displayHeight = static_cast<float>(engine.window().height());
    const float margin = 16.0f * dpiScale;
    const float availableWidth = std::max(240.0f * dpiScale, displayWidth - margin * 2.0f);
    const float availableHeight = std::max(240.0f * dpiScale, displayHeight - margin * 2.0f);
    const float panelWidth = std::min(availableWidth,
                                      std::clamp(displayWidth * 0.30f,
                                                 360.0f * dpiScale, 560.0f * dpiScale));
    ImGui::SetNextWindowPos(ImVec2(margin, margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelWidth, availableHeight), ImGuiCond_Always);
    ImGui::Begin("Physics Tests");
    ImGui::Text("Backend: %s", physics.backend_name());
    ImGui::Text("Window: %ux%u  DPI: %.2f", engine.window().width(), engine.window().height(), dpiScale);
    ImGui::Text("This sample has three independent PhysX scenes.");
    ImGui::Separator();

    int mode = parameters.mode == TestMode::RigidBodyDrop
        ? 0 : (parameters.mode == TestMode::Collision ? 1 : 2);
    ImGui::TextUnformatted("Test mode");
    if (ImGui::RadioButton("Rigid body drop", &mode, 0)) actions.rebuild = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("Collision", &mode, 1)) actions.rebuild = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("Dense PhysX pile", &mode, 2)) actions.rebuild = true;
    const auto requestedMode = mode == 0
        ? TestMode::RigidBodyDrop
        : (mode == 1 ? TestMode::Collision : TestMode::DensePerformance);
    if (requestedMode != parameters.mode) {
        parameters.mode = requestedMode;
        actions.rebuild = true;
    }

    ImGui::SeparatorText("World");
    slider("Gravity Y", &parameters.gravityY, -30.0f, 0.0f, "%.2f m/s^2");
    ImGui::Checkbox("Run simulation", &parameters.simulationRunning);

    if (parameters.mode == TestMode::RigidBodyDrop) {
        ImGui::SeparatorText("Rigid body drop parameters");
        slider("Initial height", &parameters.dropHeight, 1.0f, 12.0f, "%.2f m");
        slider("Mass", &parameters.dropMass, 0.1f, 20.0f, "%.2f kg");
        slider("Cube size", &parameters.dropSize, 0.3f, 2.5f, "%.2f m");
        slider("Restitution", &parameters.dropRestitution, 0.0f, 1.0f, "%.2f");
        slider("Friction", &parameters.dropFriction, 0.0f, 1.0f, "%.2f");
        ImGui::TextWrapped("One dynamic rigid body falls under gravity and resolves contact with the static floor.");
    } else if (parameters.mode == TestMode::Collision) {
        ImGui::SeparatorText("Collision parameters");
        slider("Impact speed", &parameters.collisionSpeed, 0.5f, 20.0f, "%.2f m/s");
        slider("Mass A", &parameters.collisionMassA, 0.1f, 20.0f, "%.2f kg");
        slider("Mass B", &parameters.collisionMassB, 0.1f, 20.0f, "%.2f kg");
        slider("Initial gap", &parameters.collisionGap, 0.5f, 10.0f, "%.2f m");
        slider("Cube size", &parameters.collisionSize, 0.3f, 2.5f, "%.2f m");
        slider("Restitution", &parameters.collisionRestitution, 0.0f, 1.0f, "%.2f");
        slider("Friction", &parameters.collisionFriction, 0.0f, 1.0f, "%.2f");
        ImGui::TextWrapped("Two independent dynamic bodies travel toward each other. The contact event and post-impact velocity are measured.");
    } else {
        ImGui::SeparatorText("Dense PhysX performance parameters");
        ImGui::SliderInt("Dynamic bodies", &parameters.denseBodyCount, 32, 4096);
        if (ImGui::IsItemDeactivatedAfterEdit()) actions.rebuild = true;
        slider("Grid spacing", &parameters.denseSpacing, 0.75f, 1.2f, "%.2f m");
        slider("Cube size", &parameters.denseSize, 0.5f, 1.2f, "%.2f m");
        slider("Restitution", &parameters.denseRestitution, 0.0f, 1.0f, "%.2f");
        slider("Friction", &parameters.denseFriction, 0.0f, 1.0f, "%.2f");
        ImGui::TextWrapped("A real PhysX pile is generated in a 3D grid. Gravity, broadphase, contacts, friction and restitution are solved by PhysX.");
    }

    ImGui::Separator();
    if (ImGui::Button("Apply parameters / Reset test", ImVec2(-1.0f, 0.0f))) actions.rebuild = true;
    const char* launchLabel = parameters.mode == TestMode::RigidBodyDrop
        ? "Launch body"
        : (parameters.mode == TestMode::Collision ? "Fire collision" : "Kick dense pile");
    if (ImGui::Button(launchLabel))
        actions.launch = true;
    ImGui::SameLine();
    if (ImGui::Button("Clear contact history")) actions.resetContactHistory = true;

    ImGui::SeparatorText("Live result");
    ImGui::Text("Simulation: %s", parameters.simulationRunning ? "RUNNING" : "PAUSED");
    ImGui::TextDisabled("Release a slider to apply and restart the test.");
    const auto& statistics = physics.statistics();
    ImGui::Text("Bodies: %zu   Dynamic: %zu", statistics.bodyCount, statistics.dynamicBodyCount);
    ImGui::Text("Steps: %llu   Simulated: %.2f s", static_cast<unsigned long long>(statistics.simulationSteps),
                statistics.simulatedSeconds);
    ImGui::Text("Contacts this step: %zu   Total events: %llu", statistics.lastContactEventCount,
                static_cast<unsigned long long>(visualState.totalContactEvents));
    if (visualState.mode == TestMode::RigidBodyDrop) {
        BodyState state;
        if (physics.get_body_state(visualState.primaryBody, state)) {
            ImGui::Text("Height: %.3f m", state.position.y);
            ImGui::Text("Vertical speed: %.3f m/s", state.linearVelocity.y);
            ImGui::Text("Awake: %s", state.awake ? "yes" : "no");
        }
    } else if (visualState.mode == TestMode::Collision) {
        BodyState a;
        BodyState b;
        if (physics.get_body_state(visualState.primaryBody, a) &&
            physics.get_body_state(visualState.secondaryBody, b)) {
            ImGui::Text("A velocity: %.3f m/s", a.linearVelocity.x);
            ImGui::Text("B velocity: %.3f m/s", b.linearVelocity.x);
            ImGui::Text("Relative speed: %.3f m/s", std::abs(a.linearVelocity.x - b.linearVelocity.x));
        }
    } else {
        ImGui::Text("Dense pile: %d dynamic bodies", parameters.denseBodyCount);
        ImGui::Text("PhysX contacts: %zu", statistics.lastContactEventCount);
        ImGui::Text("Awake bodies: %zu", statistics.activeDynamicBodyCount);
    }
    if (visualState.lastContactA != shinkou::physics::InvalidBodyId) {
        ImGui::Text("Last contact: #%u <-> #%u (%s)", visualState.lastContactA,
                    visualState.lastContactB, contact_phase_name(visualState.lastContactPhase));
        ImGui::Text("Impact speed: %.3f m/s", visualState.lastImpactSpeed);
    } else {
        ImGui::TextDisabled("Last contact: waiting for collision");
    }
    ImGui::End();
    return actions;
}
#endif

bool valid_resources(std::initializer_list<ResourceHandle> handles) {
    return std::all_of(handles.begin(), handles.end(), [](ResourceHandle handle) {
        return static_cast<bool>(handle);
    });
}
}

int main(int argc, char** argv) {
    std::uint64_t requestedFrames = 0;
    bool denseRequested = false;
    int requestedDenseBodies = 256;
    auto requestedBackend =
#if defined(_WIN32)
        shinkou::render::BackendApi::DirectX11;
#else
        shinkou::render::BackendApi::Vulkan;
#endif
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--frames" && index + 1 < argc) requestedFrames = std::strtoull(argv[++index], nullptr, 10);
        else if (argument == "--dense") denseRequested = true;
        else if (argument == "--bodies" && index + 1 < argc) {
            denseRequested = true;
            requestedDenseBodies = std::clamp(
                static_cast<int>(std::strtol(argv[++index], nullptr, 10)), 32, 4096);
        }
        else if (argument == "--dx11") requestedBackend = shinkou::render::BackendApi::DirectX11;
        else if (argument == "--dx12") requestedBackend = shinkou::render::BackendApi::DirectX12;
        else if (argument == "--vulkan") requestedBackend = shinkou::render::BackendApi::Vulkan;
        else if (argument == "--null") requestedBackend = shinkou::render::BackendApi::Null;
    }

    shinkou::EngineConfig config;
    config.renderBackend = requestedBackend;
    config.window.title = "Shinkou Physics Tests";
    config.window.width = 1280;
    config.window.height = 720;
    config.editor = false;
    config.enablePhysics = true;
    config.targetFrameRate = 60.0;
    config.physics.backend = shinkou::physics::PhysicsBackend::PhysX;
    config.physics.useFixedTimeStep = true;
    config.physics.fixedTimeStep = 1.0f / 60.0f;
    config.physics.maxSubSteps = 8;
    config.physics.enableContactEvents = true;

    shinkou::Engine engine(config);
    if (!engine.initialize()) {
        std::cerr << "physics tests initialization failed\n";
        return 1;
    }
    auto& renderer = engine.renderer();
    auto& world = engine.world();
    auto* physicsPtr = engine.physics();
    if (!physicsPtr) {
        std::cerr << "physics visualizer failed to enable the physics component\n";
        engine.shutdown();
        return 2;
    }
    auto& physics = *physicsPtr;
    const auto activeBackend = renderer.capabilities().api;

    if (!physics.is_available() || physics.backend() != shinkou::physics::PhysicsBackend::PhysX) {
        std::cerr << "physics visualizer requires the real PhysX backend; fallback Simple is not accepted\n"
                  << "  active physics backend: " << physics.backend_name() << '\n'
                  << "  error: " << physics.last_error() << '\n';
        engine.shutdown();
        return 2;
    }

    if (activeBackend == shinkou::render::BackendApi::Null) {
        std::cout << "physics tests\n"
                  << "  renderer: Null (headless)\n"
                  << "  physics backend: " << physics.backend_name() << '\n';
        shinkou::physics::BodyDesc ground;
        ground.position = {0.0f, -0.25f, 0.0f};
        ground.halfExtents = {6.0f, 0.25f, 3.0f};
        ground.dynamic = false;
        ground.shapes.push_back(shinkou::physics::ShapeDesc::box(ground.halfExtents));
        shinkou::physics::BodyDesc body;
        body.position = {0.0f, 5.0f, 0.0f};
        body.halfExtents = {0.5f, 0.5f, 0.5f};
        body.shapes.push_back(shinkou::physics::ShapeDesc::box(body.halfExtents));
        if (physics.create_body(ground) == shinkou::physics::InvalidBodyId ||
            physics.create_body(body) == shinkou::physics::InvalidBodyId) {
            std::cerr << "headless physics body creation failed: " << physics.last_error() << '\n';
            engine.shutdown();
            return 2;
        }
        if (!engine.run(requestedFrames)) {
            engine.shutdown();
            return 3;
        }
        std::cout << "  bodies: " << physics.statistics().bodyCount
                  << "  contacts: " << physics.statistics().lastContactEventCount
                  << "  simulation steps: " << physics.statistics().simulationSteps << '\n';
        engine.shutdown();
        return 0;
    }

#if !defined(SHINKOU_PHYSICS_VISUALIZER_WITH_IMGUI)
    std::cerr << "physics tests require ImGui; configure with SHINKOU_FETCH_DEPENDENCIES=ON\n";
    engine.shutdown();
    return 4;
#else
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendPlatformName = "ShinkouInput";
    ImGui::GetIO().BackendRendererName = backend_name(activeBackend);
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui::GetStyle().FrameRounding = 4.0f;
    if (!renderer.initialize_imgui()) {
        std::cerr << "ImGui renderer initialization failed: " << renderer.last_error() << '\n';
        ImGui::DestroyContext();
        engine.shutdown();
        return 5;
    }
#endif

    TextureDesc colorDescription;
    colorDescription.width = engine.window().width();
    colorDescription.height = engine.window().height();
    colorDescription.format = "bgra8";
    colorDescription.renderTarget = true;
    TextureDesc depthDescription;
    depthDescription.width = colorDescription.width;
    depthDescription.height = colorDescription.height;
    depthDescription.format = "d24s8";
    depthDescription.depthStencil = true;
    auto colorTarget = renderer.create_texture(colorDescription);
    auto depthTarget = renderer.create_depth_stencil(depthDescription);
    const auto sampler = renderer.create_sampler({"linear", "clamp", "clamp", "clamp", 1.0f, false});

    const std::string meshHlsl = R"(
cbuffer SceneFrame : register(b2) { float4x4 viewProjection; float4 cameraPositionAndFlags; };
cbuffer ObjectFrame : register(b3) { float4x4 model; };
struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; float3 worldPosition : TEXCOORD1; };
VertexOutput MeshVS(float3 position : POSITION) {
    VertexOutput output;
    const float3 worldPosition = mul(model, float4(position, 1.0)).xyz;
    output.position = mul(viewProjection, float4(worldPosition, 1.0));
    output.uv = position.xy * 0.5 + 0.5;
    output.worldPosition = worldPosition;
    return output;
}
Texture2D gTexture : register(t0);
SamplerState gSampler : register(s1);
float4 MeshPS(VertexOutput input) : SV_Target {
    const float3 dx = ddx(input.worldPosition);
    const float3 dy = ddy(input.worldPosition);
    const float3 normal = normalize(cross(dx, dy));
    const float3 lightDirection = normalize(float3(0.35, 0.65, 0.75));
    const float diffuse = 0.22 + 0.78 * saturate(dot(normal, lightDirection));
    const float4 albedo = gTexture.Sample(gSampler, input.uv);
    return float4(albedo.rgb * diffuse, albedo.a);
}
    )";
    const std::string presentHlsl = R"(
struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VertexOutput PresentVS(uint vertexId : SV_VertexID) {
    const float2 positions[6] = {
        float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0),
        float2(-1.0, -1.0), float2(1.0, 1.0), float2(1.0, -1.0)
    };
    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    const float2 screenUv = positions[vertexId] * 0.5 + 0.5;
    output.uv = float2(screenUv.x, 1.0 - screenUv.y);
    return output;
}
Texture2D gTexture : register(t0);
SamplerState gSampler : register(s1);
float4 PresentPS(VertexOutput input) : SV_Target { return gTexture.Sample(gSampler, input.uv); }
    )";

    ShaderDesc meshVertexDescription;
    meshVertexDescription.stage = ShaderStage::Vertex;
    meshVertexDescription.name = "physics_tests_mesh_vs";
    configure_shader(meshVertexDescription, activeBackend, meshHlsl, "physics_visualizer_mesh_vs", "MeshVS", "vs_5_0");
    ShaderDesc fragmentDescription;
    fragmentDescription.stage = ShaderStage::Fragment;
    fragmentDescription.name = "physics_tests_mesh_ps";
    configure_shader(fragmentDescription, activeBackend, meshHlsl, "physics_visualizer_mesh_ps", "MeshPS", "ps_5_0");
    ShaderDesc presentVertexDescription;
    presentVertexDescription.stage = ShaderStage::Vertex;
    presentVertexDescription.name = "physics_tests_present_vs";
    configure_shader(presentVertexDescription, activeBackend, presentHlsl, "physics_visualizer_present_vs", "PresentVS", "vs_5_0");
    ShaderDesc presentFragmentDescription;
    presentFragmentDescription.stage = ShaderStage::Fragment;
    presentFragmentDescription.name = "physics_tests_present_ps";
    configure_shader(presentFragmentDescription, activeBackend, presentHlsl, "physics_visualizer_present_ps", "PresentPS", "ps_5_0");
    const auto meshVertexShader = renderer.create_shader(meshVertexDescription);
    const auto fragmentShader = renderer.create_shader(fragmentDescription);
    const auto presentVertexShader = renderer.create_shader(presentVertexDescription);
    const auto presentFragmentShader = renderer.create_shader(presentFragmentDescription);
    PipelineDesc meshPipelineDescription;
    meshPipelineDescription.name = "physics_tests_mesh_pipeline";
    meshPipelineDescription.vertexShader = meshVertexShader.id;
    meshPipelineDescription.fragmentShader = fragmentShader.id;
    meshPipelineDescription.depthTest = true;
    meshPipelineDescription.depthWrite = true;
    meshPipelineDescription.vertexInput = true;
    meshPipelineDescription.cullMode = "none";
    meshPipelineDescription.colorFormat = "bgra8";
    meshPipelineDescription.depthFormat = "d24s8";
    PipelineDesc presentPipelineDescription;
    presentPipelineDescription.name = "physics_tests_present_pipeline";
    presentPipelineDescription.vertexShader = presentVertexShader.id;
    presentPipelineDescription.fragmentShader = presentFragmentShader.id;
    presentPipelineDescription.vertexInput = false;
    presentPipelineDescription.colorFormat = "bgra8";
    const auto meshPipeline = renderer.create_pipeline(meshPipelineDescription);
    const auto presentPipeline = renderer.create_pipeline(presentPipelineDescription);

    const auto groundTexture = create_solid_texture(renderer, 40, 78, 102);
    const auto dropTexture = create_solid_texture(renderer, 245, 128, 40);
    const auto collisionATexture = create_solid_texture(renderer, 35, 196, 224);
    const auto collisionBTexture = create_solid_texture(renderer, 246, 70, 170);
    const auto markerTexture = create_solid_texture(renderer, 248, 204, 70);
    const auto shadowTexture = create_solid_texture(renderer, 10, 14, 22);
    const auto groundMaterial = renderer.create_material(material_description("physics_test_ground", meshPipeline, groundTexture, sampler));
    const auto dropMaterial = renderer.create_material(material_description("physics_test_drop", meshPipeline, dropTexture, sampler));
    const auto collisionAMaterial = renderer.create_material(material_description("physics_test_collision_a", meshPipeline, collisionATexture, sampler));
    const auto collisionBMaterial = renderer.create_material(material_description("physics_test_collision_b", meshPipeline, collisionBTexture, sampler));
    const auto markerMaterial = renderer.create_material(material_description("physics_test_marker", meshPipeline, markerTexture, sampler));
    const auto shadowMaterial = renderer.create_material(material_description("physics_test_shadow", meshPipeline, shadowTexture, sampler));

    const float cubeVertices[] = {
        -1.0f, -1.0f, -1.0f, 1.0f, -1.0f, -1.0f,
        -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f,
        -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f,
        -1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f
    };
    const std::uint32_t cubeIndices[] = {
        0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6,
        0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7,
        0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5
    };
    BufferDesc vertexDescription;
    vertexDescription.size = sizeof(cubeVertices);
    vertexDescription.stride = sizeof(float) * 3;
    vertexDescription.vertexBuffer = true;
    vertexDescription.initialData.resize(sizeof(cubeVertices));
    std::memcpy(vertexDescription.initialData.data(), cubeVertices, sizeof(cubeVertices));
    BufferDesc indexDescription;
    indexDescription.size = sizeof(cubeIndices);
    indexDescription.stride = sizeof(std::uint32_t);
    indexDescription.indexBuffer = true;
    indexDescription.initialData.resize(sizeof(cubeIndices));
    std::memcpy(indexDescription.initialData.data(), cubeIndices, sizeof(cubeIndices));

    SceneResources resources;
    resources.vertexBuffer = renderer.create_buffer(vertexDescription);
    resources.indexBuffer = renderer.create_buffer(indexDescription);
    resources.sampler = sampler;
    resources.groundMaterial = groundMaterial;
    resources.dropMaterial = dropMaterial;
    resources.collisionAMaterial = collisionAMaterial;
    resources.collisionBMaterial = collisionBMaterial;
    resources.markerMaterial = markerMaterial;
    resources.shadowMaterial = shadowMaterial;
    if (!valid_resources({colorTarget, depthTarget, sampler, meshVertexShader, fragmentShader,
                          presentVertexShader, presentFragmentShader, meshPipeline, presentPipeline,
                          resources.vertexBuffer, resources.indexBuffer, groundMaterial, dropMaterial,
                          collisionAMaterial, collisionBMaterial, markerMaterial, shadowMaterial})) {
        std::cerr << "physics tests renderer resource creation failed: " << renderer.last_error() << '\n';
#if defined(SHINKOU_PHYSICS_VISUALIZER_WITH_IMGUI)
        renderer.shutdown_imgui();
        ImGui::DestroyContext();
#endif
        engine.shutdown();
        return 6;
    }

    VisualState visualState;
    TestParameters parameters;
    if (denseRequested) {
        parameters.mode = TestMode::DensePerformance;
        parameters.denseBodyCount = requestedDenseBodies;
    }
    if (!build_scene(world, physics, visualState, resources, parameters)) {
        std::cerr << "physics test scene creation failed: " << physics.last_error() << '\n';
#if defined(SHINKOU_PHYSICS_VISUALIZER_WITH_IMGUI)
        renderer.shutdown_imgui();
        ImGui::DestroyContext();
#endif
        engine.shutdown();
        return 7;
    }

    const auto cameraEntity = world.ecs().create();
    shinkou::render::TransformComponent cameraTransform;
    cameraTransform.local.position = denseRequested ? Vec3{0.0f, 6.0f, -24.0f}
                                                     : Vec3{0.0f, 3.0f, -17.0f};
    world.ecs().emplace<shinkou::render::TransformComponent>(cameraEntity, cameraTransform);
    shinkou::render::CameraComponent camera;
    camera.verticalFieldOfView = shinkou::math::Radians(55.0f);
    camera.nearPlane = 0.05f;
    camera.farPlane = 100.0f;
    world.ecs().emplace<shinkou::render::CameraComponent>(cameraEntity, camera);

    shinkou::render::RenderScene renderScene;
    shinkou::render::ForwardRenderer forwardRenderer;
    auto presentMaterial = renderer.create_material(
        material_description("physics_tests_present", presentPipeline, colorTarget, sampler));
    auto presentDescription = material_description("physics_tests_present", presentPipeline, colorTarget, sampler);

    // Engine::tick resizes the swapchain, but these persistent offscreen targets
    // belong to the sample and therefore need to be rebuilt explicitly. Keep the
    // old resources alive until the complete replacement (including the present
    // material) has been created successfully.
    auto resize_render_targets = [&](Renderer& activeRenderer,
                                     std::uint32_t width,
                                     std::uint32_t height) {
        if (width == 0 || height == 0 ||
            (width == colorDescription.width && height == colorDescription.height)) {
            return true;
        }

        TextureDesc nextColorDescription = colorDescription;
        nextColorDescription.width = width;
        nextColorDescription.height = height;
        TextureDesc nextDepthDescription = depthDescription;
        nextDepthDescription.width = width;
        nextDepthDescription.height = height;

        const auto nextColorTarget = activeRenderer.create_texture(nextColorDescription);
        if (!nextColorTarget) {
            std::cerr << "[render] failed to create resized color target "
                      << width << 'x' << height << ": " << activeRenderer.last_error() << '\n';
            return false;
        }
        const auto nextDepthTarget = activeRenderer.create_depth_stencil(nextDepthDescription);
        if (!nextDepthTarget) {
            activeRenderer.destroy_resource(nextColorTarget);
            std::cerr << "[render] failed to create resized depth target "
                      << width << 'x' << height << ": " << activeRenderer.last_error() << '\n';
            return false;
        }

        const auto nextPresentDescription = material_description(
            "physics_tests_present", presentPipeline, nextColorTarget, sampler);
        const auto nextPresentMaterial = activeRenderer.create_material(nextPresentDescription);
        if (!nextPresentMaterial) {
            activeRenderer.destroy_resource(nextDepthTarget);
            activeRenderer.destroy_resource(nextColorTarget);
            std::cerr << "[render] failed to create resized present material "
                      << width << 'x' << height << ": " << activeRenderer.last_error() << '\n';
            return false;
        }

        // The previous frame may still reference the old targets on an explicit
        // backend. Wait before releasing them, then switch every descriptor used
        // by the next render graph to the new size.
        if (activeRenderer.backend()) activeRenderer.backend()->wait_idle();
        activeRenderer.destroy_resource(presentMaterial);
        activeRenderer.destroy_resource(depthTarget);
        activeRenderer.destroy_resource(colorTarget);

        colorDescription = nextColorDescription;
        depthDescription = nextDepthDescription;
        colorTarget = nextColorTarget;
        depthTarget = nextDepthTarget;
        presentDescription = nextPresentDescription;
        presentMaterial = nextPresentMaterial;
        std::cout << "[render] resized targets to " << width << 'x' << height << '\n';
        return true;
    };

    engine.set_render_callback([&](Renderer& activeRenderer,
                                   World& activeWorld,
                                   shinkou::Seconds deltaSeconds,
                                   shinkou::FrameIndex frame) {
#if defined(SHINKOU_PHYSICS_VISUALIZER_WITH_IMGUI)
        begin_imgui_frame(engine, activeRenderer, static_cast<float>(deltaSeconds));
        const auto actions = draw_imgui_controls(engine, physics, visualState, parameters);
        if (actions.rebuild) {
            parameters.simulationRunning = true;
            if (!build_scene(activeWorld, physics, visualState, resources, parameters))
                std::cerr << "physics test scene rebuild failed: " << physics.last_error() << '\n';
        }
        physics.set_gravity({0.0f, parameters.gravityY, 0.0f});
        if (actions.launch) {
            parameters.simulationRunning = true;
            launch_test(physics, visualState, parameters);
        }
        if (!parameters.simulationRunning) freeze_dynamic_bodies(physics, visualState);
        if (actions.resetContactHistory) {
            visualState.totalContactEvents = 0;
            visualState.lastContactA = shinkou::physics::InvalidBodyId;
            visualState.lastContactB = shinkou::physics::InvalidBodyId;
        }
#endif

        const bool renderTargetsReady = resize_render_targets(
            activeRenderer, engine.window().width(), engine.window().height());

        visualState.recentContacts.clear();
        physics.drain_contact_events(visualState.recentContacts);
        visualState.totalContactEvents += visualState.recentContacts.size();
        for (const auto& event : visualState.recentContacts) {
            visualState.lastContactA = event.bodyA;
            visualState.lastContactB = event.bodyB;
            visualState.lastContactPhase = event.phase;
            visualState.lastImpactSpeed = shinkou::math::Length(event.impulse);
        }
        sync_visuals(activeWorld, physics, visualState);
        if (frame % 60u == 0u) {
            const auto& stats = physics.statistics();
            std::cout << "[physics] mode="
                      << test_mode_name(visualState.mode)
                      << " t=" << stats.simulatedSeconds
                      << " contacts=" << stats.lastContactEventCount
                      << " total-events=" << visualState.totalContactEvents << '\n';
        }

        if (renderTargetsReady) {
            renderScene.extract(activeWorld, activeRenderer,
                                static_cast<float>(colorDescription.width) /
                                static_cast<float>(std::max<std::uint32_t>(1u, colorDescription.height)));
            if (frame == 0u) {
                const auto& stats = renderScene.stats();
                std::cout << "[render] meshes=" << stats.extractedMeshes
                          << " visible=" << stats.visibleMeshes << " culled=" << stats.culledMeshes << '\n';
            }
            forwardRenderer.build(activeRenderer, renderScene, colorTarget, colorDescription,
                                  depthTarget, depthDescription);
            auto& graph = activeRenderer.graph();
            graph.import_resource(presentMaterial, presentDescription);
            graph.add_pass("physics_tests_present", {
                {colorTarget, shinkou::render::ResourceUsage::ShaderRead},
                {presentMaterial, shinkou::render::ResourceUsage::ShaderRead}
            }, [presentMaterial, colorTarget,
                width = static_cast<float>(colorDescription.width),
                height = static_cast<float>(colorDescription.height)](auto& backend, const auto&) {
                backend.set_render_target({});
                backend.set_viewport(0.0f, 0.0f, width, height);
                backend.bind_material(presentMaterial);
                backend.draw_sprite({colorTarget, {0.0f, 0.0f}, {width, height}, 0.0f});
            });
        }

#if defined(SHINKOU_PHYSICS_VISUALIZER_WITH_IMGUI)
        ImGui::Render();
        activeRenderer.set_imgui_draw_data(ImGui::GetDrawData());
#endif
    });

    std::cout << "physics tests\n"
              << "  renderer: " << backend_name(activeBackend) << '\n'
              << "  physics backend: " << physics.backend_name() << '\n'
              << "  UI: ImGui (Rigid body drop / Collision / Dense PhysX pile)\n";
    const bool runSucceeded = engine.run(requestedFrames);
    const auto stats = physics.statistics();
    std::cout << "  bodies: " << stats.bodyCount
              << "  contacts: " << stats.lastContactEventCount
              << "  total events: " << visualState.totalContactEvents
              << "  simulation steps: " << stats.simulationSteps << '\n';
    if (!renderer.last_error().empty())
        std::cerr << "physics tests renderer error: " << renderer.last_error() << '\n';
#if defined(SHINKOU_PHYSICS_VISUALIZER_WITH_IMGUI)
    renderer.shutdown_imgui();
    ImGui::DestroyContext();
#endif
    engine.shutdown();
    return runSucceeded ? 0 : 8;
}

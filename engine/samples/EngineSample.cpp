#include "shinkou/Engine.h"
#include "shinkou/physics/SimplePhysicsWorld.h"
#include "shinkou/render/RenderScene.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
#ifndef SHINKOU_SAMPLE_SHADER_DIR
#define SHINKOU_SAMPLE_SHADER_DIR ""
#endif

std::vector<std::uint8_t> load_binary_shader(const std::string& name) {
    const std::string path = std::string(SHINKOU_SAMPLE_SHADER_DIR) + "/" + name;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg();
    if (size <= 0) return {};
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return file ? data : std::vector<std::uint8_t>{};
}

struct Velocity { shinkou::math::Vec3 value{}; };
struct Position { shinkou::math::Vec3 value{}; };
struct DemoObject final : shinkou::SceneObject {
    float angle{0};
    void update(shinkou::Seconds dt) override { angle += dt; }
};
}

int main(int argc, char** argv) {
    shinkou::EngineConfig config;
    config.renderBackend = shinkou::render::BackendApi::Vulkan;
    bool recoverRequested = false;
    bool editorRequested = true;
    bool framesRequested = false;
    bool explicitBackend = false;
    std::uint64_t requestedFrames = 3;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        recoverRequested |= argument == "recover";
        editorRequested |= argument == "--editor";
        if (argument == "--sample" || argument == "--no-editor") editorRequested = false;
        if (argument == "dx11") {
            config.renderBackend = shinkou::render::BackendApi::DirectX11;
            explicitBackend = true;
        } else if (argument == "dx12") {
            config.renderBackend = shinkou::render::BackendApi::DirectX12;
            explicitBackend = true;
        } else if (argument == "vulkan") {
            config.renderBackend = shinkou::render::BackendApi::Vulkan;
            explicitBackend = true;
        }
    }
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--frames") {
            framesRequested = true;
            requestedFrames = std::strtoull(argv[i + 1], nullptr, 10);
        }
    }
    // The retained UIKit renderer currently has a native Windows compositor
    // only on D3D11. Keep the sample convenient: editor mode shows UI on a
    // normal double-click/"--editor" launch while explicit backend arguments
    // remain available for backend testing.
    if (editorRequested && !explicitBackend) config.renderBackend = shinkou::render::BackendApi::DirectX11;
    if (editorRequested && !framesRequested) requestedFrames = 0;
    config.editor = editorRequested;
    if (config.editor) config.window.title = "ShinkouEngine Editor";
    shinkou::Engine engine(config);
    if (!engine.initialize()) return 1;
    if (!engine.renderer().resize(config.window.width, config.window.height)) return 2;

    engine.world().add_object<DemoObject>();
    auto entity = engine.world().ecs().create();
    engine.world().ecs().emplace<Velocity>(entity, Velocity{shinkou::math::Vec3{1, 0, 0}});
    engine.world().ecs().emplace<Position>(entity, Position{});
    engine.world().add_system([](shinkou::World& world, shinkou::Seconds dt) {
        world.ecs().each<Position, Velocity>([dt](shinkou::Entity, Position& position, const Velocity& velocity) {
            position.value = position.value + velocity.value * dt;
        });
    });
    engine.physics().create_body({{0, 2, 0}, {0, 0, 0}, {0.5f, 0.5f, 0.5f}, 1, true});

    shinkou::render::BufferDesc uploadBuffer;
    uploadBuffer.size = 16;
    uploadBuffer.stride = 4;
    uploadBuffer.initialData = {0, 0, 0, 0};
    const auto persistentBuffer = engine.renderer().create_buffer(uploadBuffer);
    if (!engine.renderer().update_buffer({persistentBuffer, 0, {1, 2, 3, 4}})) {
        std::cerr << "buffer update failed: " << engine.renderer().last_error() << '\n';
        return 3;
    }
    engine.renderer().destroy_resource(persistentBuffer);

    shinkou::render::TextureDesc uploadTexture;
    uploadTexture.width = 2;
    uploadTexture.height = 2;
    uploadTexture.format = "rgba8";
    const auto persistentTexture = engine.renderer().create_texture(uploadTexture);
    shinkou::render::BindlessTableHandle bindlessTable{};
    if (!engine.renderer().update_texture({persistentTexture, 0, 0, 2, 2, 0,
        {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}})) {
        std::cerr << "texture upload failed: " << engine.renderer().last_error() << '\n';
        return 4;
    }
    if (engine.renderer().capabilities().supportsBindless) {
        bindlessTable = engine.renderer().create_bindless_table({4, true, shinkou::render::DescriptorType::Texture});
        if (!bindlessTable || !engine.renderer().update_bindless(bindlessTable, 0, persistentTexture)) {
            std::cerr << "bindless texture table update failed: " << engine.renderer().last_error() << '\n';
            return 6;
        }
    }

    const auto api = engine.renderer().capabilities().api;
    if (api == shinkou::render::BackendApi::DirectX11 || api == shinkou::render::BackendApi::DirectX12 || api == shinkou::render::BackendApi::Vulkan) {
        shinkou::render::TextureDesc mipTexture;
        mipTexture.width = 4;
        mipTexture.height = 4;
        mipTexture.mipLevels = 3;
        mipTexture.generateMips = true;
        const auto mipHandle = engine.renderer().create_texture(mipTexture);
        if (!engine.renderer().update_texture({mipHandle, 0, 0, 4, 4, 0, std::vector<std::uint8_t>(64, 128)}) ||
            !engine.renderer().generate_mips(mipHandle)) {
            std::cerr << "mipmap generation failed: " << engine.renderer().last_error() << '\n';
            return 5;
        }
        engine.renderer().destroy_resource(mipHandle);
    }

    const shinkou::render::TextureDesc colorDesc{config.window.width, config.window.height, 1, 1, "rgba8", true, false, {}};
    const shinkou::render::TextureDesc depthDesc{config.window.width, config.window.height, 1, 1, "d24s8", false, false, {}};
    const shinkou::render::TextureDesc spriteDesc{256, 256, 1, 1, "rgba8", false, false, {}};
    const float triangleVertices[] = {-0.6f, -0.5f, 0.0f, 0.0f, 0.6f, 0.0f, 0.6f, -0.5f, 0.0f};
    const std::uint32_t triangleIndices[] = {0, 1, 2};
    std::vector<std::uint8_t> vertexData(sizeof(triangleVertices));
    std::memcpy(vertexData.data(), triangleVertices, vertexData.size());
    std::vector<std::uint8_t> indexData(sizeof(triangleIndices));
    std::memcpy(indexData.data(), triangleIndices, indexData.size());
    const shinkou::render::BufferDesc vertexDesc{sizeof(triangleVertices), sizeof(float) * 3, true, false, vertexData};
    const shinkou::render::BufferDesc indexDesc{sizeof(triangleIndices), sizeof(std::uint32_t), false, true, indexData};
    const auto color = engine.renderer().create_texture(colorDesc);
    const auto depth = engine.renderer().create_depth_stencil(depthDesc);
    const auto spriteTexture = engine.renderer().create_texture(spriteDesc);
    const auto spriteSampler = engine.renderer().create_sampler({"linear", "repeat", "repeat", "repeat", 1.0f, false});
    const auto vertexBuffer = engine.renderer().create_buffer(vertexDesc);
    const auto indexBuffer = engine.renderer().create_buffer(indexDesc);
    shinkou::render::ShaderDesc vertexShaderDesc;
    vertexShaderDesc.stage = shinkou::render::ShaderStage::Vertex;
    vertexShaderDesc.name = "sample_vs";
    vertexShaderDesc.entryPoint = "VSMain";
    shinkou::render::ShaderDesc fragmentShaderDesc;
    fragmentShaderDesc.stage = shinkou::render::ShaderStage::Fragment;
    fragmentShaderDesc.name = "sample_ps";
    fragmentShaderDesc.entryPoint = "PSMain";
    if (api == shinkou::render::BackendApi::Vulkan) {
        vertexShaderDesc.sourceKind = shinkou::render::ShaderSourceKind::SpirV;
        fragmentShaderDesc.sourceKind = shinkou::render::ShaderSourceKind::SpirV;
        vertexShaderDesc.bytecode = load_binary_shader("sample_vs.spv");
        fragmentShaderDesc.bytecode = load_binary_shader("sample_ps.spv");
    } else if (api == shinkou::render::BackendApi::DirectX12) {
        vertexShaderDesc.sourceKind = shinkou::render::ShaderSourceKind::Dxil;
        fragmentShaderDesc.sourceKind = shinkou::render::ShaderSourceKind::Dxil;
        vertexShaderDesc.bytecode = load_binary_shader("sample_vs.dxil");
        fragmentShaderDesc.bytecode = load_binary_shader("sample_ps.dxil");
    } else if (api == shinkou::render::BackendApi::DirectX11) {
        vertexShaderDesc.entryPoint = "main";
        fragmentShaderDesc.entryPoint = "main";
        vertexShaderDesc.profile = "vs_5_0";
        fragmentShaderDesc.profile = "ps_5_0";
        vertexShaderDesc.source = R"(cbuffer SceneFrame : register(b2) { float4x4 viewProjection; float4 cameraPositionAndFlags; }; cbuffer ObjectFrame : register(b3) { float4x4 model; }; struct O { float4 p : SV_Position; }; O main(uint id : SV_VertexID) { float2 p[6] = { float2(-1,-1), float2(-1,1), float2(1,1), float2(-1,-1), float2(1,1), float2(1,-1) }; float4 localPosition=float4(p[id],0,1); float4 worldPosition=mul(localPosition,model); O o; o.p=cameraPositionAndFlags.w>1.5 ? localPosition : mul(worldPosition,viewProjection); return o; })";
        fragmentShaderDesc.source = R"(Texture2D gTexture : register(t0); SamplerState gSampler : register(s1); cbuffer SceneFrame : register(b2) { float4x4 viewProjection; float4 cameraPositionAndFlags; }; float4 main(float4 position : SV_Position) : SV_Target { float2 uv = position.xy / float2(1280.0,720.0); float sceneTint = 0.9 + 0.1 * (0.5 + 0.5 * sin(cameraPositionAndFlags.z)); return gTexture.Sample(gSampler, uv) * sceneTint; })";
    }
    const auto vertexShader = engine.renderer().create_shader(vertexShaderDesc);
    auto meshVertexShaderDesc = vertexShaderDesc;
    meshVertexShaderDesc.name = "sample_mesh_vs";
    if (api == shinkou::render::BackendApi::Vulkan) meshVertexShaderDesc.bytecode = load_binary_shader("sample_mesh_vs.spv");
    else if (api == shinkou::render::BackendApi::DirectX12) meshVertexShaderDesc.bytecode = load_binary_shader("sample_mesh_vs.dxil");
    else if (api == shinkou::render::BackendApi::DirectX11) meshVertexShaderDesc.source = R"(cbuffer SceneFrame : register(b2) { float4x4 viewProjection; float4 cameraPositionAndFlags; }; cbuffer ObjectFrame : register(b3) { float4x4 model; }; struct O { float4 p : SV_Position; }; O main(float3 position : POSITION) { O o; o.p=mul(mul(float4(position,1),model),viewProjection); return o; })";
    const auto meshVertexShader = engine.renderer().create_shader(meshVertexShaderDesc);
    const auto fragmentShader = engine.renderer().create_shader(fragmentShaderDesc);
    const auto pipeline = engine.renderer().create_pipeline({"sample_pipeline", vertexShader.id, fragmentShader.id, 0, false, false, false, false});
    const auto meshPipeline = engine.renderer().create_pipeline({"sample_mesh_pipeline", vertexShader.id, fragmentShader.id, 0, false, false, false, false});
    const auto material = engine.renderer().create_material({"sample_material", pipeline, {{"albedo", spriteTexture, shinkou::render::DescriptorType::Texture, 0, 0}, {"sampler", spriteSampler, shinkou::render::DescriptorType::Sampler, 1, 0}}, false});
    const auto presentMaterial = engine.renderer().create_material({"present_material", pipeline, {{"color", color, shinkou::render::DescriptorType::Texture, 0, 0}, {"sampler", spriteSampler, shinkou::render::DescriptorType::Sampler, 1, 0}}, false});

    auto meshEntity = engine.world().ecs().create();
    shinkou::render::TransformComponent meshTransform;
    meshTransform.local.position = {0.0f, 0.0f, 0.0f};
    engine.world().ecs().emplace<shinkou::render::TransformComponent>(meshEntity, meshTransform);
    const auto meshMaterial = engine.renderer().create_material({"mesh_material", meshPipeline, {{"albedo", spriteTexture, shinkou::render::DescriptorType::Texture, 0, 0}, {"sampler", spriteSampler, shinkou::render::DescriptorType::Sampler, 1, 0}}, false});
    engine.world().ecs().emplace<shinkou::render::MeshRendererComponent>(meshEntity,
        shinkou::render::MeshRendererComponent{vertexBuffer, indexBuffer, meshMaterial, 3, true});
    auto spriteEntity = engine.world().ecs().create();
    shinkou::render::TransformComponent spriteTransform;
    spriteTransform.local.position = {320.0f, 180.0f, 0.0f};
    engine.world().ecs().emplace<shinkou::render::TransformComponent>(spriteEntity, spriteTransform);
    engine.world().ecs().emplace<shinkou::render::SpriteRendererComponent>(spriteEntity,
        shinkou::render::SpriteRendererComponent{spriteTexture, material, {128.0f, 128.0f}, 0.0f, true});
    auto cameraEntity = engine.world().ecs().create();
    shinkou::render::TransformComponent cameraTransform;
    cameraTransform.local.position = {0.0f, 0.0f, -5.0f};
    engine.world().ecs().emplace<shinkou::render::TransformComponent>(cameraEntity, cameraTransform);
    engine.world().ecs().emplace<shinkou::render::CameraComponent>(cameraEntity, shinkou::render::CameraComponent{});

    shinkou::render::RenderScene renderScene;
    shinkou::render::ForwardRenderer forwardRenderer;
    const shinkou::render::MaterialDesc presentDescription{"present_material", pipeline,
        {{"color", color, shinkou::render::DescriptorType::Texture, 0, 0}, {"sampler", spriteSampler, shinkou::render::DescriptorType::Sampler, 1, 0}}, false};
    engine.set_render_callback([&, color, colorDesc, depth, depthDesc, presentMaterial, presentDescription](
        shinkou::render::Renderer& renderer, shinkou::World& world, shinkou::Seconds, shinkou::FrameIndex) {
        if (editorRequested) {
            renderer.graph().add_pass("editor_present", std::vector<shinkou::render::ResourceHandle>{},
                std::vector<shinkou::render::ResourceHandle>{}, [](auto&, const auto&) {});
            return;
        }
        renderScene.extract(world, renderer, static_cast<float>(colorDesc.width) / static_cast<float>(colorDesc.height));
        forwardRenderer.build(renderer, renderScene, color, colorDesc, depth, depthDesc);
        auto& graph = renderer.graph();
        graph.import_resource(presentMaterial, presentDescription);
        const auto presentObjectBuffer = forwardRenderer.prepare_present(renderer);
        if (const auto* description = renderer.resource_description(presentObjectBuffer)) {
            graph.import_resource(presentObjectBuffer, *description);
        }
        graph.add_pass("present", {
            {color, shinkou::render::ResourceUsage::ShaderRead},
            {forwardRenderer.scene_buffer(), shinkou::render::ResourceUsage::ShaderRead},
            {presentObjectBuffer, shinkou::render::ResourceUsage::ShaderRead}
        }, [presentMaterial, color, &forwardRenderer, presentObjectBuffer](auto& backend, const auto&) {
            backend.bind_material(presentMaterial);
            backend.bind_uniform_buffer(forwardRenderer.scene_buffer(), 2, 0);
            backend.bind_uniform_buffer(presentObjectBuffer, 3, 0);
            backend.draw_sprite({color, {0, 0}, {1280, 720}, 0.0f});
        });
    });

    if (!engine.run(requestedFrames)) {
        std::cerr << "engine run loop failed\n";
        return 8;
    }
    if (!engine.renderer().last_error().empty()) {
        std::cerr << "renderer submission reported an error: " << engine.renderer().last_error() << '\n';
        return 9;
    }
    if (recoverRequested) {
        if (!engine.renderer().recover()) {
            std::cerr << "renderer recovery failed: " << engine.renderer().last_error() << '\n';
            return 7;
        }
        if (engine.running() && !engine.run(1)) {
            std::cerr << "engine recovery frame failed\n";
            return 8;
        }
    }
    if (bindlessTable) engine.renderer().destroy_bindless_table(bindlessTable);
    engine.renderer().destroy_resource(persistentTexture);
    engine.renderer().destroy_resource(presentMaterial);
    engine.renderer().destroy_resource(material);
    engine.renderer().destroy_resource(meshMaterial);
    engine.renderer().destroy_resource(meshPipeline);
    engine.renderer().destroy_resource(meshVertexShader);
    engine.renderer().destroy_resource(pipeline);
    engine.renderer().destroy_resource(fragmentShader);
    engine.renderer().destroy_resource(vertexShader);
    engine.renderer().destroy_resource(indexBuffer);
    engine.renderer().destroy_resource(vertexBuffer);
    engine.renderer().destroy_resource(spriteTexture);
    engine.renderer().destroy_resource(spriteSampler);
    engine.renderer().destroy_resource(depth);
    engine.renderer().destroy_resource(color);
    const auto capabilities = engine.renderer().capabilities();
    const auto stats = engine.renderer().stats();
    std::cout << "device-ready=" << capabilities.deviceReady
              << " bindless=" << capabilities.supportsBindless
              << " frames=" << stats.frames
              << " passes=" << stats.passes
              << " draws=" << stats.drawCalls << "\n";

    const auto& renderGraph = engine.renderer().graph();
    const auto& diagnostics = renderGraph.diagnostics();
    std::cout << "render-graph passes=" << diagnostics.passCount
              << " resources=" << diagnostics.resourceCount
              << " physical-textures=" << diagnostics.physicalTextureCount
              << " aliased-textures=" << diagnostics.aliasedTextureCount
              << " transitions=" << diagnostics.plannedTransitionCount
              << " cross-queue=" << diagnostics.crossQueueDependencyCount
              << " culled=" << diagnostics.culledPassCount
              << " hdr-passes=" << diagnostics.hdrPassCount
              << " compile-ns=" << diagnostics.compileNanoseconds
              << " execute-ns=" << diagnostics.executeNanoseconds
              << " gpu-timing-delayed=" << (diagnostics.gpuTimingDelayed ? "true" : "false") << '\n';
    for (const auto& timing : renderGraph.pass_timings()) {
        std::cout << "pass " << timing.name
                  << " cpu-ns=" << timing.cpuNanoseconds
                  << " gpu-ns=" << timing.gpuNanoseconds
              << " draws=" << timing.drawCalls
                  << " barriers=" << timing.barriers
                  << " reads=" << timing.resourceReads
                  << " writes=" << timing.resourceWrites << '\n';
    }
    std::cout << "indirect-draws=" << stats.indirectDrawCalls
              << " dispatches=" << stats.dispatchCalls
              << " barriers=" << stats.barriers << '\n';
    std::cout << "ShinkouEngine sample initialized with OOP, ECS, physics and render graph support.\n";
    engine.shutdown();
    return 0;
}

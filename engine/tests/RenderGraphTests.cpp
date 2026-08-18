#include "shinkou/render/RenderGraph.h"
#include "shinkou/render/PostProcess.h"
#include "shinkou/render/Renderer.h"
#include "shinkou/render/RenderScene.h"
#include "shinkou/render/ShaderCompiler.h"
#include "shinkou/ecs/Registry.h"
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <vector>

int main() {
    shinkou::math::Transform transform;
    transform.position = {3.0f, 4.0f, 5.0f};
    transform.scale = {2.0f, 2.0f, 2.0f};
    const auto transformedOrigin = shinkou::math::TransformPoint(shinkou::math::TransformMatrix(transform), {});
    if (transformedOrigin.x != 3.0f || transformedOrigin.y != 4.0f || transformedOrigin.z != 5.0f) {
        std::cerr << "transform matrix conversion failed\n";
        return 14;
    }
    const auto projection = shinkou::math::Perspective(shinkou::math::Pi / 3.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    if (projection.m[0] <= 0.0f || projection.m[5] <= 0.0f || projection.m[11] != 1.0f) {
        std::cerr << "perspective matrix construction failed\n";
        return 15;
    }

    struct Position { int value{0}; };
    struct Velocity { int value{0}; };
    shinkou::ecs::Registry registry;
    const auto moving = registry.create();
    const auto staticEntity = registry.create();
    registry.emplace<Position>(moving, Position{1});
    registry.emplace<Velocity>(moving, Velocity{2});
    registry.emplace<Position>(staticEntity, Position{4});
    if (!registry.has<Position>(moving) || registry.has<Velocity>(staticEntity) ||
        registry.get<Position>(moving).value != 1) {
        std::cerr << "ECS component access failed\n";
        return 12;
    }
    registry.emplace_or_replace<Position>(moving, Position{3});
    int queryCount = 0;
    registry.each<Position, Velocity>([&](shinkou::Entity, Position& position, Velocity& velocity) {
        position.value += velocity.value;
        ++queryCount;
    });
    if (queryCount != 1 || registry.get<Position>(moving).value != 5) {
        std::cerr << "ECS multi-component query failed\n";
        return 13;
    }

    shinkou::render::RenderGraph graph;
    const auto texture = graph.create_texture({64, 64, 1, 1, "rgba8", true});
    graph.add_pass("producer", {}, {texture}, [](auto&, const auto&) {});
    graph.add_pass("consumer", {texture}, {}, [](auto&, const auto&) {});
    std::string error;
    if (!graph.compile(&error)) {
        std::cerr << "unexpected graph error: " << error << '\n';
        return 1;
    }
    if (graph.execution_order().size() != 2 || graph.execution_order()[0] != 0 || graph.execution_order()[1] != 1) {
        std::cerr << "write-after-read dependency was not scheduled" << '\n';
        return 2;
    }

    shinkou::render::RenderGraph invalid;
    invalid.add_pass("invalid", {{shinkou::render::ResourceHandle{999, shinkou::render::ResourceKind::Buffer}, shinkou::render::ResourceUsage::ShaderRead}}, [](auto&, const auto&) {});
    if (invalid.compile(&error) || error.find("unknown resource") == std::string::npos) {
        std::cerr << "unknown resource was not diagnosed" << '\n';
        return 3;
    }
    if (invalid.diagnostics().lastError.find("unknown resource") == std::string::npos) {
        std::cerr << "render graph diagnostic error was not retained" << '\n';
        return 24;
    }
    shinkou::render::RenderGraph wrongKind;
    const auto wrongKindTexture = wrongKind.create_texture({16, 16, 1, 1, "rgba8", true, false, {}});
    wrongKind.add_pass("wrong_kind", {shinkou::render::ResourceAccess{
        {wrongKindTexture.id, shinkou::render::ResourceKind::Buffer},
        shinkou::render::ResourceUsage::ShaderRead}}, [](auto&, const auto&) {});
    if (wrongKind.compile(&error) || error.find("wrong resource kind") == std::string::npos) {
        std::cerr << "resource kind mismatch was not diagnosed\n";
        return 87;
    }
    shinkou::render::RenderGraph importValidation;
    const shinkou::render::ResourceHandle importedBuffer{0x80000000u, shinkou::render::ResourceKind::Buffer};
    importValidation.import_resource(importedBuffer, shinkou::render::TextureDesc{16, 16, 1, 1, "rgba8", false, false, {}});
    if (importValidation.contains_resource(importedBuffer)) return 88;
    const auto invalidDepth = importValidation.create_depth_stencil({0, 16, 1, 1, "d24s8"});
    if (invalidDepth) return 89;
    const auto ownedTexture = importValidation.create_texture({8, 8, 1, 1, "rgba8", true, false, {}});
    importValidation.import_texture(ownedTexture, {16, 16, 1, 1, "rgba8", true, false, {}});
    if (!ownedTexture || importValidation.is_external_resource(ownedTexture) ||
        std::get<shinkou::render::TextureDesc>(importValidation.resources().back().description).width != 8) return 90;
    shinkou::render::RenderGraph invalidUsage;
    const auto invalidTexture = invalidUsage.create_texture({32, 32, 1, 1, "rgba8", true, false, {}});
    invalidUsage.add_pass("invalid_usage", {{invalidTexture, shinkou::render::ResourceUsage::VertexBuffer}},
        [](auto&, const auto&) {});
    if (invalidUsage.compile(&error) || error.find("incompatible usage") == std::string::npos) {
        std::cerr << "incompatible resource usage was not diagnosed\n";
        return 25;
    }
    shinkou::render::RenderGraph capabilityValidation;
    const auto nonTargetTexture = capabilityValidation.create_texture({16, 16, 1, 1, "rgba8", false, false, {}});
    capabilityValidation.add_pass("missing_target_capability", {{nonTargetTexture, shinkou::render::ResourceUsage::ColorAttachment}},
        [](auto&, const auto&) {});
    if (capabilityValidation.compile(&error) || error.find("renderTarget capability") == std::string::npos) {
        std::cerr << "render-target capability mismatch was not diagnosed\n";
        return 102;
    }
    shinkou::render::RenderGraph storageValidation;
    const auto nonStorageTexture = storageValidation.create_texture({16, 16, 1, 1, "rgba8", false, false, {}});
    storageValidation.add_pass("missing_storage_capability", {{nonStorageTexture, shinkou::render::ResourceUsage::StorageWrite}},
        [](auto&, const auto&) {}, shinkou::render::RenderQueue::Compute);
    if (storageValidation.compile(&error) || error.find("storage capability") == std::string::npos) {
        std::cerr << "storage capability mismatch was not diagnosed\n";
        return 103;
    }
    shinkou::render::RenderGraph duplicateAccess;
    const auto duplicateTexture = duplicateAccess.create_texture({32, 32, 1, 1, "rgba8", true, false, {}});
    duplicateAccess.add_pass("duplicate_access", {
        {duplicateTexture, shinkou::render::ResourceUsage::ShaderRead},
        {duplicateTexture, shinkou::render::ResourceUsage::ColorAttachment}}, [](auto&, const auto&) {});
    if (duplicateAccess.compile(&error) || error.find("more than once") == std::string::npos) {
        std::cerr << "duplicate resource access was not diagnosed\n";
        return 26;
    }
    shinkou::render::RenderGraph queueGraph;
    const auto queueBuffer = queueGraph.create_buffer({256, 16, false, false, {}});
    queueGraph.add_pass("copy_upload", {{queueBuffer, shinkou::render::ResourceUsage::CopyDestination}},
        [](auto&, const auto&) {}, shinkou::render::RenderQueue::Copy);
    queueGraph.add_pass("compute_read", {{queueBuffer, shinkou::render::ResourceUsage::StorageRead}},
        [](auto&, const auto&) {}, shinkou::render::RenderQueue::Compute);
    if (!queueGraph.compile(&error) || queueGraph.diagnostics().copyPassCount != 1 ||
        queueGraph.diagnostics().computePassCount != 1 || queueGraph.diagnostics().crossQueueDependencyCount != 1 ||
        queueGraph.queue_dependencies().size() != 1 || queueGraph.queue_batches().size() != 2 ||
        queueGraph.queue_batches()[1].waitBatches.size() != 1 || queueGraph.queue_batches()[1].waitBatches[0] != 0) {
        std::cerr << "render queue scheduling metadata was not compiled\n";
        return 27;
    }
    auto queueBackend = shinkou::render::create_backend(shinkou::render::BackendApi::Null);
    queueBackend->initialize({});
    queueGraph.execute(*queueBackend, &error);
    if (queueBackend->stats().queueSubmissions != queueGraph.queue_batches().size()) {
        std::cerr << "render queue batch execution boundaries were not honored\n";
        return 55;
    }
    shinkou::render::RenderGraph computeOnlyGraph;
    const auto computeOnlyBuffer = computeOnlyGraph.create_buffer({256, 16, false, false, {}, false, false, true});
    computeOnlyGraph.add_pass("compute_only", {{computeOnlyBuffer, shinkou::render::ResourceUsage::StorageWrite}},
        [](auto& backend, const auto&) { backend.dispatch({1, 1, 1}); }, shinkou::render::RenderQueue::Compute);
    if (!computeOnlyGraph.compile(&error) || computeOnlyGraph.queue_batches().size() != 1 ||
        computeOnlyGraph.queue_batches().front().queue != shinkou::render::RenderQueue::Compute) {
        std::cerr << "compute-only render graph scheduling metadata was not compiled\n";
        return 64;
    }
    auto computeOnlyBackend = shinkou::render::create_backend(shinkou::render::BackendApi::Null);
    computeOnlyBackend->initialize({});
    computeOnlyGraph.execute(*computeOnlyBackend, &error);
    if (computeOnlyBackend->stats().queueSubmissions != 1 || computeOnlyBackend->stats().dispatchCalls != 1) {
        std::cerr << "compute-only render graph execution contract failed\n";
        return 65;
    }
    shinkou::render::RenderGraph cullGraph;
    const auto cullTexture = cullGraph.create_texture({32, 32, 1, 1, "rgba8", true, false, {}});
    cullGraph.add_pass("dead_pass", {{cullTexture, shinkou::render::ResourceUsage::ColorAttachment}},
        [](auto&, const auto&) {}, shinkou::render::RenderQueue::Graphics, false);
    if (!cullGraph.compile(&error) || cullGraph.diagnostics().culledPassCount != 1 ||
        !cullGraph.execution_order().empty()) {
        std::cerr << "unused render pass was not culled\n";
        return 28;
    }
    shinkou::render::RenderGraph dependencyCullGraph;
    const auto dependencyIntermediate = dependencyCullGraph.create_texture({32, 32, 1, 1, "rgba8", true, false, {}});
    const auto dependencyOutput = dependencyCullGraph.create_texture({32, 32, 1, 1, "rgba8", true, false, {}});
    dependencyCullGraph.add_pass("dependency_producer", {{dependencyIntermediate, shinkou::render::ResourceUsage::ColorAttachment}},
        [](auto&, const auto&) {}, shinkou::render::RenderQueue::Graphics, false);
    dependencyCullGraph.add_pass("dependency_consumer", {
        {dependencyIntermediate, shinkou::render::ResourceUsage::ShaderRead},
        {dependencyOutput, shinkou::render::ResourceUsage::ColorAttachment}},
        [](auto&, const auto&) {}, shinkou::render::RenderQueue::Graphics, true);
    if (!dependencyCullGraph.compile(&error) || dependencyCullGraph.diagnostics().culledPassCount != 0 ||
        dependencyCullGraph.execution_order().size() != 2) {
        std::cerr << "live pass dependency was incorrectly culled\n";
        return 29;
    }
    shinkou::render::RenderGraph indirectGraph;
    const auto indirectArgs = indirectGraph.create_buffer({sizeof(std::uint32_t) * 5, sizeof(std::uint32_t) * 5,
        false, false, {}, true});
    const auto indirectVertex = indirectGraph.create_buffer({64, 16, true, false, {}});
    const auto indirectIndex = indirectGraph.create_buffer({64, sizeof(std::uint32_t), false, true, {}});
    indirectGraph.add_pass("indirect_draw", {
        {indirectArgs, shinkou::render::ResourceUsage::IndirectArguments},
        {indirectVertex, shinkou::render::ResourceUsage::VertexBuffer},
        {indirectIndex, shinkou::render::ResourceUsage::IndexBuffer}}, [indirectVertex, indirectIndex, indirectArgs](auto& backend, const auto&) {
            backend.draw_mesh_indirect({indirectVertex, indirectIndex, indirectArgs, 1});
        });
    if (!indirectGraph.compile(&error) || indirectGraph.diagnostics().plannedTransitionCount != 3) {
        std::cerr << "indirect draw resource plan was not compiled\n";
        return 34;
    }

    auto backend = shinkou::render::create_backend(shinkou::render::BackendApi::Null);
    if (!backend || !backend->initialize({})) return 4;
    graph.execute(*backend, &error);
    if (graph.pass_timings().size() != 2 || backend->stats().passes != 2 ||
        graph.diagnostics().passCount != 2 || graph.diagnostics().resourceCount != 1 ||
        graph.diagnostics().compileNanoseconds == 0 || graph.diagnostics().executeNanoseconds == 0 ||
        graph.diagnostics().plannedTransitionCount != 2 || graph.transitions().size() != 2 ||
        graph.pass_timings()[0].resourceWrites != 1 || graph.pass_timings()[1].resourceReads != 1) {
        std::cerr << "render pass diagnostics were not recorded" << '\n';
        return 5;
    }
    if (backend->stats().debugMarkers != 2) {
        std::cerr << "render pass debug markers were not recorded\n";
        return 53;
    }
    if (backend->stats().resourceDestroys == 0) {
        std::cerr << "transient render graph resources were not retired\n";
        return 63;
    }
    const shinkou::render::ResourceHandle directBuffer{0x51000001u, shinkou::render::ResourceKind::Buffer};
    if (!backend->create_resource(directBuffer, shinkou::render::BufferDesc{16}) ||
        backend->update_buffer({{directBuffer.id, shinkou::render::ResourceKind::Texture2D}, 0, {1, 2, 3, 4}}) ||
        backend->transition_resource({directBuffer.id, shinkou::render::ResourceKind::Texture2D}, shinkou::render::ResourceUsage::ShaderRead) ||
        backend->alias_resource({0x51000002u, shinkou::render::ResourceKind::Texture2D}, directBuffer)) {
        std::cerr << "direct backend resource kind contract failed\n";
        return 106;
    }
    backend->destroy_resource({directBuffer.id, shinkou::render::ResourceKind::Texture2D});
    if (!backend->transition_resource(directBuffer, shinkou::render::ResourceUsage::ShaderRead)) {
        std::cerr << "wrong-kind destroy corrupted the direct backend resource\n";
        return 107;
    }
    backend->destroy_resource(directBuffer);
    shinkou::render::RenderGraph mrtGraph;
    const auto mrt0 = mrtGraph.create_texture({32, 32, 1, 1, "rgba8", true, false, {}});
    const auto mrt1 = mrtGraph.create_texture({32, 32, 1, 1, "rgba16f", true, false, {}, true, "linear"});
    const auto mrtDepth = mrtGraph.create_depth_stencil({32, 32, 1, 1, "d24s8", false, false, {}});
    mrtGraph.add_pass("mrt", {{mrt0, shinkou::render::ResourceUsage::ColorAttachment0},
        {mrt1, shinkou::render::ResourceUsage::ColorAttachment1},
        {mrtDepth, shinkou::render::ResourceUsage::DepthStencil}}, [](auto&, const auto&) {});
    if (!mrtGraph.compile(&error) || mrtGraph.diagnostics().plannedTransitionCount != 3) {
        std::cerr << "MRT render graph contract failed\n";
        return 60;
    }

    auto shaderCompiler = shinkou::render::create_shader_compiler();
    shinkou::render::ShaderDesc hlsl;
    hlsl.name = "reflection_hlsl";
    hlsl.source = "Texture2D albedo : register(t3, space2); SamplerState linearSampler : register(s1);";
    const auto hlslReflection = shaderCompiler->compile(hlsl, shinkou::render::BackendApi::Null);
    if (!hlslReflection.valid || hlslReflection.bindings.size() != 2 ||
        hlslReflection.bindings[0].name != "albedo" || hlslReflection.bindings[0].slot != 3 ||
        hlslReflection.bindings[0].space != 2 || hlslReflection.bindings[0].type != "texture" ||
        hlslReflection.bindings[1].type != "sampler") {
        std::cerr << "HLSL shader reflection failed\n";
        return 7;
    }
    shinkou::render::ShaderDesc arrayReflection;
    arrayReflection.name = "reflection_array";
    arrayReflection.source = "Texture2D atlas[4] : register(t2, space1);";
    const auto arrayResult = shaderCompiler->compile(arrayReflection, shinkou::render::BackendApi::Null);
    if (!arrayResult.valid || arrayResult.bindings.size() != 1 || arrayResult.bindings[0].count != 4 || arrayResult.bindings[0].unbounded) {
        std::cerr << "fixed-size shader array reflection failed\n";
        return 63;
    }
    shinkou::render::ShaderDesc conflictReflection = hlsl;
    conflictReflection.name = "reflection_conflict";
    conflictReflection.source = "Texture2D first : register(t0); SamplerState second : register(t0);";
    const auto conflictResult = shaderCompiler->compile(conflictReflection, shinkou::render::BackendApi::Null);
    if (conflictResult.valid || conflictResult.layoutValid || conflictResult.diagnostics.empty()) {
        std::cerr << "conflicting shader layout was accepted\n";
        return 64;
    }
    const auto asyncReflection = shaderCompiler->compile_async(hlsl, shinkou::render::BackendApi::Null).get();
    if (!asyncReflection.valid || asyncReflection.bindings.size() != hlslReflection.bindings.size()) {
        std::cerr << "asynchronous shader compilation failed\n";
        return 56;
    }
    const auto cachedReflection = shaderCompiler->compile(hlsl, shinkou::render::BackendApi::Null);
    if (!cachedReflection.valid || !cachedReflection.cacheHit) {
        std::cerr << "shader disk or memory cache did not report a hit\n";
        return 78;
    }
    shinkou::render::ShaderDesc cullingReflection;
    cullingReflection.name = "gpu_culling_reflection";
    cullingReflection.stage = shinkou::render::ShaderStage::Compute;
    cullingReflection.source = "StructuredBuffer<InstanceData> instances : register(t0); RWStructuredBuffer<DrawArguments> arguments : register(u1);";
    const auto cullingShaderReflection = shaderCompiler->compile(cullingReflection, shinkou::render::BackendApi::Null);
    if (!cullingShaderReflection.valid || cullingShaderReflection.bindings.size() != 2 ||
        cullingShaderReflection.bindings[0].type != "structured_buffer" ||
        cullingShaderReflection.bindings[1].type != "storage_buffer") {
        std::cerr << "GPU culling shader reflection failed\n";
        return 50;
    }

    shinkou::render::ShaderDesc glsl;
    glsl.name = "reflection_glsl";
    glsl.source = "layout(set=4, binding=2) uniform sampler2D atlas[];";
    const auto glslReflection = shaderCompiler->compile(glsl, shinkou::render::BackendApi::Null);
    if (!glslReflection.valid || glslReflection.bindings.size() != 1 ||
        glslReflection.bindings[0].name != "atlas" || glslReflection.bindings[0].slot != 2 ||
        glslReflection.bindings[0].space != 4 || glslReflection.bindings[0].type != "texture") {
        std::cerr << "GLSL shader reflection failed\n";
        return 8;
    }

    std::vector<std::uint32_t> spirv = {0x07230203u, 0x00010000u, 0, 64, 0};
    const auto addInstruction = [&](std::uint16_t opcode, std::initializer_list<std::uint32_t> operands) {
        spirv.push_back((static_cast<std::uint32_t>(operands.size()) + 1u) << 16 | opcode);
        spirv.insert(spirv.end(), operands.begin(), operands.end());
    };
    addInstruction(71, {20, 33, 0});
    addInstruction(71, {20, 34, 2});
    addInstruction(25, {30});
    addInstruction(27, {31, 30});
    addInstruction(29, {32, 31});
    addInstruction(32, {40, 0, 32});
    addInstruction(59, {40, 20, 0});
    const std::string spirvName = "atlas";
    const std::size_t nameWordCount = (spirvName.size() + 1 + 3) / 4;
    spirv.push_back(static_cast<std::uint32_t>((2 + nameWordCount) << 16) | 5u);
    spirv.push_back(20);
    spirv.resize(spirv.size() + nameWordCount, 0);
    for (std::size_t index = 0; index < spirvName.size(); ++index) {
        spirv[spirv.size() - nameWordCount + index / 4] |=
            static_cast<std::uint32_t>(static_cast<unsigned char>(spirvName[index])) << ((index % 4) * 8);
    }
    shinkou::render::ShaderDesc spirvShader;
    spirvShader.sourceKind = shinkou::render::ShaderSourceKind::SpirV;
    spirvShader.bytecode.resize(spirv.size() * sizeof(std::uint32_t));
    std::memcpy(spirvShader.bytecode.data(), spirv.data(), spirvShader.bytecode.size());
    const auto spirvReflection = shaderCompiler->compile(spirvShader, shinkou::render::BackendApi::Vulkan);
    if (!spirvReflection.valid || spirvReflection.bindings.size() != 1 ||
        spirvReflection.bindings[0].name != "atlas" || spirvReflection.bindings[0].slot != 0 ||
        spirvReflection.bindings[0].space != 2 || spirvReflection.bindings[0].type != "texture") {
        std::cerr << "SPIR-V shader reflection failed\n";
        return 11;
    }
    std::vector<std::uint32_t> arraySpirv = {0x07230203u, 0x00010000u, 0, 80, 0};
    const auto addArrayInstruction = [&](std::uint16_t opcode, std::initializer_list<std::uint32_t> operands) {
        arraySpirv.push_back((static_cast<std::uint32_t>(operands.size()) + 1u) << 16 | opcode);
        arraySpirv.insert(arraySpirv.end(), operands.begin(), operands.end());
    };
    addArrayInstruction(71, {50, 33, 3});
    addArrayInstruction(71, {50, 34, 1});
    addArrayInstruction(25, {60});
    addArrayInstruction(27, {61, 60});
    addArrayInstruction(43, {60, 62, 4});
    addArrayInstruction(28, {63, 61, 62});
    addArrayInstruction(32, {64, 0, 63});
    addArrayInstruction(59, {64, 50, 0});
    shinkou::render::ShaderDesc spirvArrayShader;
    spirvArrayShader.sourceKind = shinkou::render::ShaderSourceKind::SpirV;
    spirvArrayShader.bytecode.resize(arraySpirv.size() * sizeof(std::uint32_t));
    std::memcpy(spirvArrayShader.bytecode.data(), arraySpirv.data(), spirvArrayShader.bytecode.size());
    const auto spirvArrayReflection = shaderCompiler->compile(spirvArrayShader, shinkou::render::BackendApi::Vulkan);
    if (!spirvArrayReflection.valid || spirvArrayReflection.bindings.size() != 1 ||
        spirvArrayReflection.bindings[0].count != 4 || spirvArrayReflection.bindings[0].unbounded) {
        std::cerr << "SPIR-V fixed array reflection failed\n";
        return 66;
    }

    shinkou::render::Renderer renderer(shinkou::render::BackendApi::Null);
    if (!renderer.initialize() || !renderer.recover() || !renderer.capabilities().deviceReady) {
        std::cerr << "renderer recovery failed" << '\n';
        return 6;
    }
    if (renderer.resize(0, 720) || renderer.last_error().find("dimensions") == std::string::npos) {
        std::cerr << "zero-sized renderer resize was not rejected\n";
        return 101;
    }
    auto uploadBackend = shinkou::render::create_backend(shinkou::render::BackendApi::Vulkan);
    if (uploadBackend && uploadBackend->initialize({nullptr, 1, 1, true, false})) {
        const std::vector<std::uint8_t> initialUpload(256, 7);
        const shinkou::render::ResourceHandle uploadBuffer{0x70000001u, shinkou::render::ResourceKind::Buffer};
        uploadBackend->create_resource(uploadBuffer, shinkou::render::BufferDesc{initialUpload.size(), 16, false, false, initialUpload});
        const std::vector<std::uint8_t> replacementUpload(32, 3);
        uploadBackend->begin_upload_batch();
        if (!uploadBackend->update_buffer({uploadBuffer, 64, replacementUpload})) {
            std::cerr << "Vulkan GPU-local buffer upload failed\n";
            return 57;
        }
        if (!uploadBackend->flush_upload_batch()) {
            std::cerr << "Vulkan upload batch flush failed\n";
            return 61;
        }
        uploadBackend->wait_idle();
        const shinkou::render::ResourceHandle uploadTexture{0x70000002u, shinkou::render::ResourceKind::Texture2D};
        const std::vector<std::uint8_t> textureUpload(4 * 4 * 4, 11);
        uploadBackend->create_resource(uploadTexture, shinkou::render::TextureDesc{4, 4, 1, 1, "rgba8", false, false, textureUpload});
        uploadBackend->destroy_resource(uploadTexture);
        uploadBackend->destroy_resource(uploadBuffer);
    }
    const shinkou::render::PipelineDesc cachedPipeline{"cached", 1, 2, 0, false, false, true};
    const auto firstPipeline = renderer.create_pipeline(cachedPipeline);
    const auto secondPipeline = renderer.create_pipeline(cachedPipeline);
    if (firstPipeline.id != secondPipeline.id) {
        std::cerr << "pipeline cache did not reuse an identical description\n";
        return 9;
    }
    renderer.destroy_resource(firstPipeline);
    const auto recreatedPipeline = renderer.create_pipeline(cachedPipeline);
    if (recreatedPipeline.id == firstPipeline.id) {
        std::cerr << "pipeline cache retained a destroyed resource\n";
        return 10;
    }
    const shinkou::render::ShaderDesc variantShader{
        shinkou::render::ShaderStage::Vertex, "variant_shader", "float4 main() : SV_Position { return 0; }",
        "main", "vs_5_0", shinkou::render::ShaderSourceKind::Source, {}, {"QUALITY=LOW"}, 1};
    const auto variantShaderA = renderer.create_shader(variantShader);
    const auto variantShaderB = renderer.create_shader(variantShader);
    if (variantShaderA.id != variantShaderB.id) {
        std::cerr << "identical shader variants were not cached\n";
        return 30;
    }
    auto changedVariant = variantShader;
    changedVariant.revision = 2;
    const auto variantShaderC = renderer.create_shader(changedVariant);
    if (variantShaderC.id == variantShaderA.id) {
        std::cerr << "shader revision did not create a new variant\n";
        return 31;
    }
    auto reloadDescription = variantShader;
    reloadDescription.source = "float4 main() : SV_Position { return float4(1, 0, 0, 1); }";
    reloadDescription.revision = 3;
    if (!renderer.reload_shader(variantShaderA, reloadDescription) ||
        renderer.resource_description(variantShaderA) == nullptr ||
        std::get<shinkou::render::ShaderDesc>(*renderer.resource_description(variantShaderA)).revision != 3) {
        std::cerr << "shader hot reload did not preserve the resource handle\n";
        return 69;
    }
    auto changedPipeline = cachedPipeline;
    changedPipeline.cullMode = "none";
    if (renderer.create_pipeline(changedPipeline).id == firstPipeline.id) {
        std::cerr << "pipeline fixed-function state was omitted from cache key\n";
        return 32;
    }
    const auto persistentBuffer = renderer.create_buffer({64, 16, false, false, std::vector<std::uint8_t>(64, 0)});
    if (renderer.resource_description({persistentBuffer.id, shinkou::render::ResourceKind::Texture2D}) != nullptr ||
        renderer.update_buffer({{persistentBuffer.id, shinkou::render::ResourceKind::Texture2D}, 0, {1, 2, 3, 4}})) {
        std::cerr << "persistent resource kind ownership validation failed\n";
        return 91;
    }
    const std::vector<std::uint8_t> bufferPatch(8, 9);
    if (!renderer.update_buffer({persistentBuffer, 16, bufferPatch})) {
        std::cerr << "persistent buffer update failed: " << renderer.last_error() << "\n";
        return 70;
    }
    const auto* persistedBufferDescription = renderer.resource_description(persistentBuffer);
    if (!persistedBufferDescription || std::get<shinkou::render::BufferDesc>(*persistedBufferDescription).initialData.size() != 64 ||
        std::get<shinkou::render::BufferDesc>(*persistedBufferDescription).initialData[16] != 9) {
        std::cerr << "persistent buffer CPU copy was not retained\n";
        return 71;
    }
    const auto persistentTexture = renderer.create_texture({4, 4, 1, 2, "rgba8", false, false, {}});
    const std::vector<std::uint8_t> mipPatch(2 * 2 * 4, 13);
    if (!renderer.update_texture({persistentTexture, 1, 0, 2, 2, 0, mipPatch})) {
        std::cerr << "persistent texture update failed: " << renderer.last_error() << "\n";
        return 72;
    }
    const auto* persistedTextureDescription = renderer.resource_description(persistentTexture);
    if (!persistedTextureDescription || std::get<shinkou::render::TextureDesc>(*persistedTextureDescription).initialSubresources.size() != 1 ||
        std::get<shinkou::render::TextureDesc>(*persistedTextureDescription).initialSubresources.front().data.front() != 13) {
        std::cerr << "persistent texture CPU copy was not retained\n";
        return 73;
    }
    const auto partialTexture = renderer.create_texture({4, 4, 1, 1, "rgba8", false, false, std::vector<std::uint8_t>(64, 1)});
    const std::vector<std::uint8_t> partialPatch(2 * 2 * 4, 7);
    if (!renderer.update_texture({partialTexture, 0, 0, 2, 2, 0, partialPatch, 1, 1})) {
        std::cerr << "persistent partial texture update failed\n";
        return 74;
    }
    const auto* partialDescription = renderer.resource_description(partialTexture);
    if (!partialDescription || std::get<shinkou::render::TextureDesc>(*partialDescription).initialSubresources.empty()) {
        std::cerr << "persistent partial texture CPU copy was not retained\n";
        return 75;
    }
    const auto& partialData = std::get<shinkou::render::TextureDesc>(*partialDescription).initialSubresources.front().data;
    if (partialData[0] != 1 || partialData[(1u * 4u + 1u) * 4u] != 7 || partialData[(3u * 4u + 3u) * 4u] != 1) {
        std::cerr << "persistent partial texture coordinates were not retained\n";
        return 76;
    }
    const auto generatedTexture = renderer.create_texture({4, 4, 1, 3, "rgba8", false, true, std::vector<std::uint8_t>(64, 4)});
    const auto* generatedDescription = renderer.resource_description(generatedTexture);
    if (!generatedDescription || std::get<shinkou::render::TextureDesc>(*generatedDescription).initialSubresources.size() < 2) {
        std::cerr << "generated mip CPU copies were not retained\n";
        return 77;
    }
    const auto runtimeTexture = renderer.create_texture({8, 8, 1, 1, "rgba8", false, false, {}});
    const auto material = renderer.create_material({"runtime_material", recreatedPipeline,
        {{"texture", runtimeTexture,
          shinkou::render::DescriptorType::Texture, 0, 0}}, false});
    const auto replacementTexture = renderer.create_texture({8, 8, 1, 1, "rgba8", false, false, {}});
    if (!renderer.update_material_binding(material, {"texture", replacementTexture,
        shinkou::render::DescriptorType::Texture, 0, 0})) {
        std::cerr << "runtime material binding update failed\n";
        return 16;
    }
    const auto* updatedMaterial = renderer.resource_description(material);
    if (!updatedMaterial || std::get<shinkou::render::MaterialDesc>(*updatedMaterial).bindings[0].resource.id != replacementTexture.id) {
        std::cerr << "runtime material binding was not persisted\n";
        return 17;
    }
    if (renderer.update_material(material, {"runtime_material", firstPipeline, {}, false})) {
        std::cerr << "material pipeline mutation was not rejected\n";
        return 18;
    }
    if (renderer.update_material(material, {"runtime_material", recreatedPipeline,
        {{"texture", replacementTexture, shinkou::render::DescriptorType::UniformBuffer, 0, 0}}, false})) {
        std::cerr << "material descriptor type mutation was not rejected\n";
        return 19;
    }
    if (renderer.create_material({"invalid_material", recreatedPipeline,
        {{"duplicateA", replacementTexture, shinkou::render::DescriptorType::Texture, 0, 0},
         {"duplicateB", replacementTexture, shinkou::render::DescriptorType::Texture, 0, 0}}, false})) {
        std::cerr << "duplicate material binding was not rejected\n";
        return 33;
    }
    const auto sampler = renderer.create_sampler({"nearest", "clamp", "clamp", "clamp", 1.0f, false});
    if (!sampler || sampler.kind != shinkou::render::ResourceKind::Sampler) {
        std::cerr << "sampler resource creation failed\n";
        return 20;
    }
    shinkou::render::ShaderDesc reflectedFragment;
    reflectedFragment.stage = shinkou::render::ShaderStage::Fragment;
    reflectedFragment.name = "material_layout_fragment";
    reflectedFragment.source = "Texture2D albedo : register(t0); SamplerState linear : register(s1); float4 main() : SV_Target { return 1; }";
    const auto reflectedFragmentHandle = renderer.create_shader(reflectedFragment);
    const auto layoutPipeline = renderer.create_pipeline({"material_layout_pipeline", variantShaderA.id,
        reflectedFragmentHandle.id, 0, false, false, false, false});
    const auto validLayoutMaterial = renderer.create_material({"valid_layout_material", layoutPipeline,
        {{"albedo", runtimeTexture, shinkou::render::DescriptorType::Texture, 0, 0},
         {"linear", sampler, shinkou::render::DescriptorType::Sampler, 1, 0}}, false});
    if (!validLayoutMaterial) {
        std::cerr << "valid reflected material layout was rejected\n";
        return 58;
    }
    if (renderer.create_material({"invalid_layout_material", layoutPipeline,
        {{"wrong", sampler, shinkou::render::DescriptorType::Sampler, 0, 0}}, false})) {
        std::cerr << "invalid reflected material layout was accepted\n";
        return 59;
    }
    shinkou::render::ShaderDesc arrayFragment;
    arrayFragment.stage = shinkou::render::ShaderStage::Fragment;
    arrayFragment.name = "material_array_fragment";
    arrayFragment.source = "Texture2D atlas[2] : register(t2); float4 main() : SV_Target { return atlas[0].Load(int3(0, 0, 0)); }";
    const auto arrayFragmentHandle = renderer.create_shader(arrayFragment);
    const auto arrayPipeline = renderer.create_pipeline({"material_array_pipeline", variantShaderA.id,
        arrayFragmentHandle.id, 0, false, false, false, false});
    const auto arrayTextureA = renderer.create_texture({8, 8, 1, 1, "rgba8", false, false, {}});
    const auto arrayTextureB = renderer.create_texture({8, 8, 1, 1, "rgba8", false, false, {}});
    const auto arrayMaterial = renderer.create_material({"array_material", arrayPipeline,
        {{"atlas", {}, shinkou::render::DescriptorType::Texture, 2, 0, 2, false, {arrayTextureA, arrayTextureB}}}, false});
    if (!arrayMaterial) {
        std::cerr << "fixed descriptor array material was rejected: " << renderer.last_error() << "\n";
        return 79;
    }
    if (renderer.create_material({"short_array_material", arrayPipeline,
        {{"atlas", {}, shinkou::render::DescriptorType::Texture, 2, 0, 2, false, {arrayTextureA}}}, false})) {
        std::cerr << "short fixed descriptor array was accepted\n";
        return 80;
    }
    shinkou::render::RenderGraph materialGraph;
    const auto materialOutput = materialGraph.create_texture({16, 16, 1, 1, "rgba8", true, false, {}});
    materialGraph.add_pass("material_bind", {{materialOutput, shinkou::render::ResourceUsage::ColorAttachment}},
        [arrayMaterial](auto& passBackend, const auto&) {
            passBackend.bind_material(arrayMaterial);
            passBackend.draw_sprite({});
        });
    if (!materialGraph.compile(&error)) {
        std::cerr << "material binding graph compilation failed: " << error << "\n";
        return 81;
    }
    materialGraph.execute(*renderer.backend(), &error);
    if (renderer.stats().materialBinds == 0 || renderer.stats().descriptorBinds == 0 || renderer.stats().drawCalls == 0) {
        std::cerr << "material binding draw regression failed: " << error << "\n";
        return 81;
    }
    shinkou::render::RenderGraph failingGraph;
    const auto failingTexture = failingGraph.create_texture({16, 16, 1, 1, "rgba8", true, false, {}});
    failingGraph.add_pass("intentional_backend_failure", {{failingTexture, shinkou::render::ResourceUsage::ColorAttachment}},
        [](auto& passBackend, const auto&) { passBackend.bind_material({0xdeadbeefu, shinkou::render::ResourceKind::Material}); });
    if (!failingGraph.compile(&error)) {
        std::cerr << "failure graph compilation failed: " << error << "\n";
        return 83;
    }
    failingGraph.execute(*renderer.backend(), &error);
    if (error.find("intentional_backend_failure") == std::string::npos ||
        !failingGraph.diagnostics().executionFailed || failingGraph.diagnostics().executedPassCount != 0 ||
        failingGraph.diagnostics().failedPassIndex != 0 || renderer.stats().discardedFrames == 0) {
        std::cerr << "render pass failure propagation contract failed: " << error << "\n";
        return 84;
    }
    auto invalidFormatPipeline = cachedPipeline;
    invalidFormatPipeline.colorFormats = {"unsupported_format"};
    if (renderer.create_pipeline(invalidFormatPipeline)) {
        std::cerr << "unsupported pipeline attachment format was accepted\n";
        return 62;
    }
    if (renderer.create_bindless_table({8, true, shinkou::render::DescriptorType::Sampler}) ||
        renderer.last_error().find("texture capacity") == std::string::npos) {
        std::cerr << "portable bindless descriptor contract was not enforced\n";
        return 99;
    }
    const auto persistentTable = renderer.create_bindless_table({8, true, shinkou::render::DescriptorType::Texture});
    if (persistentTable) {
        if (!renderer.update_bindless(persistentTable, 0, replacementTexture)) {
            std::cerr << "persistent bindless update failed\n";
            return 39;
        }
        if (renderer.update_bindless(persistentTable, 1, replacementTexture, shinkou::render::DescriptorType::Sampler)) {
            std::cerr << "bindless descriptor type mismatch was accepted\n";
            return 104;
        }
        if (!renderer.recover()) {
            std::cerr << "bindless recovery failed\n";
            return 40;
        }
        if (!renderer.update_bindless(persistentTable, 0, replacementTexture)) {
            std::cerr << "pre-recovery bindless handle was not stable after recovery\n";
            return 105;
        }
    }
    if (!renderer.update_material_binding(material, {"linearSampler", sampler,
        shinkou::render::DescriptorType::Sampler, 1, 0})) {
        std::cerr << "sampler material binding update failed\n";
        return 21;
    }
    shinkou::render::PostProcessPipeline postProcess;
    postProcess.add_stage({"tone_map", recreatedPipeline, sampler, {64, 64, 1, 1, "rgba8", true, false, {}}, 0, 1});
    postProcess.add_stage({"sharpen", recreatedPipeline, sampler, {64, 64, 1, 1, "rgba8", true, false, {}}, 0, 1});
    shinkou::render::RenderGraph postGraph;
    const auto postSource = postGraph.create_texture({64, 64, 1, 1, "rgba8", true, false, {}});
    const auto postResult = postProcess.build(postGraph, postSource,
        {64, 64, 1, 1, "rgba8", true, false, {}});
    if (!postResult || postGraph.passes().size() != 2 || postGraph.resources().size() != 5 ||
        !postGraph.compile(&error) || postGraph.execution_order().size() != 2) {
        std::cerr << "post-process pipeline graph construction failed\n";
        return 22;
    }
    if ((postSource.id & 0x80000000u) == 0 || (postResult.id & 0x80000000u) == 0 ||
        (runtimeTexture.id & 0x80000000u) != 0) {
        std::cerr << "render graph resource namespace was not isolated\n";
        return 87;
    }
    if (postGraph.resources().front().external) {
        std::cerr << "graph-owned post-process source was incorrectly imported\n";
        return 83;
    }
    shinkou::render::RenderGraph hdrGraph;
    const auto hdrTexture = hdrGraph.create_texture({64, 64, 1, 1, "rgba16f", true, false, {}, true, "linear"});
    hdrGraph.add_pass("hdr_tone_map", {{hdrTexture, shinkou::render::ResourceUsage::ColorAttachment}},
        [](auto&, const auto&) {});
    if (!hdrGraph.compile(&error) || hdrGraph.diagnostics().hdrPassCount != 1) {
        std::cerr << "HDR render graph metadata was not recorded\n";
        return 35;
    }
    shinkou::render::PostProcessChainDesc chain;
    chain.name = "taa_chain";
    chain.sourceDescription = {64, 64, 1, 1, "rgba16f", true, false, {}, true, "linear"};
    shinkou::render::RenderGraph chainGraph;
    chain.source = chainGraph.create_texture(chain.sourceDescription);
    const auto chainHistory = chainGraph.create_texture({64, 64, 1, 1, "rgba16f", true, false, {}, true, "linear"});
    chain.stages.push_back({"taa", recreatedPipeline, sampler,
        {64, 64, 1, 1, "rgba16f", true, false, {}, true, "linear"}, 0, 1, false, 1.0f, 0.9f,
        chainHistory, {64, 64, 1, 1, "rgba16f", true, false, {}, true, "linear"}, 2});
    const auto chainOutput = postProcess.build(chainGraph, chain);
    if (!chainOutput || !chainGraph.compile(&error) || chainGraph.passes().size() != 1) {
        std::cerr << "post-process chain descriptor was not compiled: " << error << "\n";
        return 36;
    }
    if (chainGraph.resources().size() != 4 ||
        chainGraph.resources()[0].external || chainGraph.resources()[1].external) {
        std::cerr << "post-process history ownership contract failed\n";
        return 84;
    }
    shinkou::render::RenderGraph historyGraph;
    const auto historyInput = historyGraph.create_texture({32, 32, 1, 1, "rgba16f", true, false, {}, true, "linear"});
    const auto historyRead = historyGraph.create_texture({32, 32, 1, 1, "rgba16f", true, false, {}, true, "linear"});
    const auto historyWrite = historyGraph.create_texture({32, 32, 1, 1, "rgba16f", true, false, {}, true, "linear"});
    shinkou::render::PostProcessPipeline historyPost;
    shinkou::render::PostProcessStageDesc historyStage{"history_mrt", recreatedPipeline, sampler,
        {32, 32, 1, 1, "rgba16f", true, false, {}, true, "linear"}, 0, 1, false, 1.0f, 0.5f,
        historyRead, {32, 32, 1, 1, "rgba16f", true, false, {}, true, "linear"}, 2};
    historyStage.historyOutput = historyWrite;
    historyStage.historyOutputDescription = {32, 32, 1, 1, "rgba16f", true, false, {}, true, "linear"};
    historyPost.add_stage(historyStage);
    const auto historyOutput = historyPost.build(historyGraph, historyInput,
        {32, 32, 1, 1, "rgba16f", true, false, {}, true, "linear"});
    if (!historyOutput || !historyGraph.compile(&error) || historyGraph.passes().front().accesses.size() != 5 ||
        historyGraph.passes().front().accesses.back().usage != shinkou::render::ResourceUsage::ColorAttachment1) {
        std::cerr << "post-process history write contract failed: " << error << "\n";
        return 85;
    }
    shinkou::render::RenderGraph computeHistoryGraph;
    const auto computeHistoryInput = computeHistoryGraph.create_texture({32, 32, 1, 1, "rgba16f", false, false, {}, true, "linear"});
    const auto computeHistoryRead = computeHistoryGraph.create_texture({32, 32, 1, 1, "rgba16f", false, false, {}, true, "linear"});
    const auto computeHistoryWrite = computeHistoryGraph.create_texture({32, 32, 1, 1, "rgba16f", false, false, {}, true, "linear", true});
    shinkou::render::PostProcessPipeline computeHistoryPost;
    shinkou::render::PostProcessStageDesc computeHistoryStage{"history_compute", recreatedPipeline, sampler,
        {32, 32, 1, 1, "rgba16f", false, false, {}, true, "linear", true}, 0, 1, true, 1.0f, 0.5f,
        computeHistoryRead, {32, 32, 1, 1, "rgba16f", false, false, {}, true, "linear"}, 2};
    computeHistoryStage.historyOutput = computeHistoryWrite;
    computeHistoryStage.historyOutputDescription = {32, 32, 1, 1, "rgba16f", false, false, {}, true, "linear", true};
    computeHistoryPost.add_stage(computeHistoryStage);
    const auto computeHistoryOutput = computeHistoryPost.build(computeHistoryGraph, computeHistoryInput,
        {32, 32, 1, 1, "rgba16f", false, false, {}, true, "linear"});
    computeHistoryGraph.add_pass("history_compute_consumer", {{computeHistoryOutput, shinkou::render::ResourceUsage::ShaderRead}},
        [](auto&, const auto&) {});
    if (!computeHistoryOutput || !computeHistoryGraph.compile(&error) ||
        computeHistoryGraph.diagnostics().computePassCount != 1 ||
        computeHistoryGraph.diagnostics().crossQueueDependencyCount != 1 ||
        computeHistoryGraph.queue_batches().size() != 2 ||
        computeHistoryGraph.passes().front().accesses.back().usage != shinkou::render::ResourceUsage::StorageWrite) {
        std::cerr << "post-process compute history queue contract failed: " << error << "\n";
        return 86;
    }
    shinkou::render::RenderGraph computeGraph;
    const auto computeInput = computeGraph.create_texture({32, 32, 1, 1, "rgba16f", false, false, {}, true, "linear"});
    shinkou::render::PostProcessPipeline computePost;
    computePost.add_stage({"compute_bloom", recreatedPipeline, sampler,
        {32, 32, 1, 1, "rgba16f", false, false, {}, true, "linear", true}, 0, 1, true});
    const auto computeOutput = computePost.build(computeGraph, computeInput,
        {32, 32, 1, 1, "rgba16f", false, false, {}, true, "linear"});
    if (!computeOutput || !computeGraph.compile(&error) || computeGraph.diagnostics().computePassCount != 1) {
        std::cerr << "compute post-process stage was not scheduled\n";
        return 37;
    }
    auto invalidPostStage = shinkou::render::PostProcessStageDesc{"invalid_compute", recreatedPipeline, sampler,
        {32, 32, 1, 1, "rgba16f", false, false, {}, true, "linear"}, 0, 1, true};
    if (shinkou::render::validate_post_process_stage(invalidPostStage).valid()) {
        std::cerr << "compute storage/render-target contract was not enforced\n";
        return 38;
    }
    auto invalidHistoryStage = invalidPostStage;
    invalidHistoryStage.compute = false;
    invalidHistoryStage.output = {32, 32, 1, 1, "rgba16f", true, false, {}, true, "linear"};
    invalidHistoryStage.historyTexture = historyRead;
    invalidHistoryStage.historyDescription = {16, 16, 1, 1, "rgba16f", true, false, {}, true, "linear"};
    if (shinkou::render::validate_post_process_stage(invalidHistoryStage).valid()) {
        std::cerr << "history frame description contract was not enforced\n";
        return 39;
    }
    shinkou::render::PostProcessChainDesc invalidChain = chain;
    invalidChain.stages.push_back(invalidChain.stages.front());
    invalidChain.stages.back().name = invalidChain.stages.front().name;
    if (shinkou::render::validate_post_process_chain(invalidChain).valid()) {
        std::cerr << "post-process stage naming conflict was not enforced\n";
        return 40;
    }
    shinkou::render::RenderGraph parameterPostGraph;
    const auto parameterInput = parameterPostGraph.create_texture({48, 24, 1, 1, "rgba16f", false, false, {}, true, "linear"});
    shinkou::render::PostProcessPipeline parameterPost;
    shinkou::render::PostProcessStageDesc parameterStage{"parameter_stage", recreatedPipeline, sampler,
        {24, 12, 1, 1, "rgba16f", true, false, {}, true, "linear"}, 0, 1, false, 1.5f, 0.75f};
    parameterStage.useParameters = true;
    parameterStage.parameterSlot = 4;
    parameterPost.add_stage(parameterStage);
    const auto parameterOutput = parameterPost.build(parameterPostGraph, parameterInput,
        {48, 24, 1, 1, "rgba16f", false, false, {}, true, "linear"});
    if (!parameterOutput || parameterPostGraph.resources().size() != 4 ||
        parameterPostGraph.passes().empty() || parameterPostGraph.passes().front().accesses.size() != 4 ||
        !parameterPostGraph.compile(&error)) {
        std::cerr << "post-process parameter resource contract failed: " << error << "\n";
        return 68;
    }
    shinkou::render::RenderScene shadowScene;
    shinkou::render::ShadowSettings shadows;
    shadows.atlasSize = 1024;
    if (shadows.cascadeCount != 4 || shadows.atlasSize != 1024 || !shadows.enabled) {
        std::cerr << "shadow settings contract was not initialized\n";
        return 38;
    }
    shinkou::render::Renderer sceneRenderer(shinkou::render::BackendApi::Null);
    if (!sceneRenderer.initialize()) {
        std::cerr << "scene renderer initialization failed\n";
        return 41;
    }
    const auto sceneTexture = sceneRenderer.create_texture({8, 8, 1, 1, "rgba8", false, false, {}});
    const auto sceneSampler = sceneRenderer.create_sampler({});
    const auto sceneVertex = sceneRenderer.create_buffer({64, sizeof(float) * 3, true, false, {}, false, true, false});
    const auto sceneIndex = sceneRenderer.create_buffer({64, sizeof(std::uint32_t), false, true, {}});
    const auto scenePipeline = sceneRenderer.create_pipeline({"gpu_scene_pipeline", 1, 2, 0, true, true, false, true});
    const auto sceneMaterial = sceneRenderer.create_material({"gpu_scene_material", scenePipeline,
        {{"albedo", sceneTexture, shinkou::render::DescriptorType::Texture, 0, 0},
         {"sampler", sceneSampler, shinkou::render::DescriptorType::Sampler, 1, 0},
         {"instances", sceneVertex, shinkou::render::DescriptorType::StructuredBuffer, 4, 0},
         {"lights", sceneVertex, shinkou::render::DescriptorType::StructuredBuffer, 5, 0}}, false});
    shinkou::World sceneWorld;
    const auto camera = sceneWorld.ecs().create();
    shinkou::render::TransformComponent cameraTransform;
    cameraTransform.local.position = {0.0f, 0.0f, -5.0f};
    sceneWorld.ecs().emplace<shinkou::render::TransformComponent>(camera, cameraTransform);
    sceneWorld.ecs().emplace<shinkou::render::CameraComponent>(camera, shinkou::render::CameraComponent{});
    const auto lightEntity = sceneWorld.ecs().create();
    shinkou::render::TransformComponent lightTransform;
    lightTransform.local.position = {0.0f, 2.0f, 0.0f};
    sceneWorld.ecs().emplace<shinkou::render::TransformComponent>(lightEntity, lightTransform);
    sceneWorld.ecs().emplace<shinkou::render::LightComponent>(lightEntity, shinkou::render::LightComponent{});
    for (int index = 0; index < 2; ++index) {
        const auto entity = sceneWorld.ecs().create();
        shinkou::render::TransformComponent objectTransform;
        objectTransform.local.position = {static_cast<float>(index), 0.0f, 0.0f};
        sceneWorld.ecs().emplace<shinkou::render::TransformComponent>(entity, objectTransform);
        sceneWorld.ecs().emplace<shinkou::render::MeshRendererComponent>(entity,
            shinkou::render::MeshRendererComponent{sceneVertex, sceneIndex, sceneMaterial, 3, true, true, {}, 1.0f});
    }
    shinkou::render::RenderScene extracted;
    extracted.extract(sceneWorld, sceneRenderer, 1.0f);
    if (extracted.stats().visibleMeshes != 2 || extracted.meshes().size() != 2 || !extracted.meshes()[0].gpuDriven) {
        std::cerr << "GPU driven scene extraction failed\n";
        return 42;
    }
    if (extracted.lights().size() != 1) {
        std::cerr << "light extraction failed\n";
        return 47;
    }
    shinkou::render::Renderer shadowRenderer(shinkou::render::BackendApi::Null);
    shadowRenderer.initialize();
    const auto shadowPipeline = shadowRenderer.create_pipeline({"shadow_pipeline", 1, 2, 0, true, true, false, true});
    const auto shadowTarget = shadowRenderer.create_depth_stencil({128, 128, 1, 1, "d24s8", false, false, {}});
    shadowRenderer.begin_graph();
    shinkou::render::ForwardRenderer shadowForward;
    shadowForward.build_shadows(shadowRenderer, extracted, shadows, shadowPipeline, shadowTarget,
        {128, 128, 1, 1, "d24s8", false, false, {}});
    if (shadowForward.shadow_cascades().size() != shadows.cascadeCount ||
        shadowRenderer.graph().passes().size() != shadows.cascadeCount ||
        !shadowRenderer.graph().compile(&error)) {
        std::cerr << "shadow cascade render plan was not compiled\n";
        return 49;
    }
    for (std::size_t index = 0; index < shadowForward.shadow_cascades().size(); ++index) {
        const auto& cascade = shadowForward.shadow_cascades()[index];
        if (cascade.farDistance <= cascade.nearDistance || cascade.atlasSize == 0 ||
            cascade.viewProjection.m[0] == 0.0f || cascade.viewProjection.m[5] == 0.0f) {
            std::cerr << "shadow cascade projection contract failed\n";
            return 66;
        }
        if (index > 0 && shadowForward.shadow_cascades()[index - 1].farDistance > cascade.nearDistance) {
            std::cerr << "shadow cascade ranges are not ordered\n";
            return 67;
        }
    }
    const auto sceneColor = sceneRenderer.create_texture({64, 64, 1, 1, "rgba8", true, false, {}});
    const auto sceneDepth = sceneRenderer.create_depth_stencil({64, 64, 1, 1, "d24s8", false, false, {}});
    sceneRenderer.begin_graph();
    shinkou::render::ForwardRenderer sceneForward;
    sceneForward.build(sceneRenderer, extracted, sceneColor, {64, 64, 1, 1, "rgba8", true, false, {}},
        sceneDepth, {64, 64, 1, 1, "d24s8", false, false, {}});
    if (!sceneForward.light_buffer() || !sceneRenderer.resource_description(sceneForward.light_buffer())) {
        std::cerr << "light GPU buffer was not created\n";
        return 48;
    }
    if (!sceneRenderer.graph().compile(&error)) {
        std::cerr << "GPU driven scene graph failed: " << error << "\n";
        return 43;
    }
    const auto lightAccessRecorded = std::any_of(sceneRenderer.graph().passes().begin(), sceneRenderer.graph().passes().end(),
        [&](const auto& pass) {
            return std::any_of(pass.accesses.begin(), pass.accesses.end(),
                [&](const auto& access) {
                    return access.resource.id == sceneForward.light_buffer().id &&
                        access.usage == shinkou::render::ResourceUsage::ShaderRead;
                });
        });
    if (!lightAccessRecorded) {
        std::cerr << "GPU driven light buffer dependency was omitted from the graph\n";
        return 100;
    }
    sceneRenderer.submit();
    if (sceneRenderer.stats().indirectDrawCalls != 1) {
        std::cerr << "GPU driven scene did not submit an indirect draw\n";
        return 44;
    }
    shinkou::render::RenderGraph cullingGraph;
    const auto cullingInstances = cullingGraph.create_buffer({256, 64, false, false, {}, false, true, false});
    const auto cullingArguments = cullingGraph.create_buffer({sizeof(std::uint32_t) * 5, sizeof(std::uint32_t) * 5,
        false, false, {}, true, false, true});
    const auto cullingVertex = cullingGraph.create_buffer({256, 16, true, false, {}});
    const auto cullingIndex = cullingGraph.create_buffer({256, sizeof(std::uint32_t), false, true, {}});
    const auto cullingPipeline = cullingGraph.create_pipeline({"culling", 0, 0, 7, false, false, false, false});
    const auto cullingMaterial = cullingGraph.create_material({"culling_material", cullingPipeline,
        {{"instances", cullingInstances, shinkou::render::DescriptorType::StructuredBuffer, 0, 0},
         {"arguments", cullingArguments, shinkou::render::DescriptorType::StorageBuffer, 1, 0}}, false});
    cullingGraph.add_pass("gpu_culling", {
        {cullingMaterial, shinkou::render::ResourceUsage::ShaderRead},
        {cullingInstances, shinkou::render::ResourceUsage::ShaderRead},
        {cullingArguments, shinkou::render::ResourceUsage::StorageWrite}},
        [cullingMaterial](auto& backend, const auto&) {
            backend.bind_material(cullingMaterial);
            backend.dispatch({1, 1, 1});
        }, shinkou::render::RenderQueue::Compute);
    cullingGraph.add_pass("gpu_indirect_consumer", {
        {cullingArguments, shinkou::render::ResourceUsage::IndirectArguments},
        {cullingVertex, shinkou::render::ResourceUsage::VertexBuffer},
        {cullingIndex, shinkou::render::ResourceUsage::IndexBuffer}},
        [cullingVertex, cullingIndex, cullingArguments](auto& backend, const auto&) {
            backend.draw_mesh_indirect({cullingVertex, cullingIndex, cullingArguments, 1});
        });
    if (!cullingGraph.compile(&error) || cullingGraph.diagnostics().crossQueueDependencyCount != 1 ||
        cullingGraph.diagnostics().computePassCount != 1) {
        std::cerr << "GPU culling to indirect draw dependency was not compiled\n";
        return 45;
    }
    auto cullingBackend = shinkou::render::create_backend(shinkou::render::BackendApi::Null);
    cullingBackend->initialize({});
    cullingGraph.execute(*cullingBackend, &error);
    if (cullingBackend->stats().dispatchCalls != 1 || cullingBackend->stats().indirectDrawCalls != 1) {
        std::cerr << "GPU culling execution contract failed\n";
        return 46;
    }
    shinkou::render::RenderGraph aliasGraph;
    const auto aliasA = aliasGraph.create_texture({64, 64, 1, 1, "rgba8", true, false, {}});
    const auto aliasB = aliasGraph.create_texture({64, 64, 1, 1, "rgba8", true, false, {}});
    aliasGraph.add_pass("alias_a", {{aliasA, shinkou::render::ResourceUsage::ColorAttachment}}, [](auto&, const auto&) {});
    aliasGraph.add_pass("alias_b", {{aliasB, shinkou::render::ResourceUsage::ColorAttachment}}, [](auto&, const auto&) {});
    if (!aliasGraph.compile(&error) || aliasGraph.physical_resource(aliasA).id != aliasGraph.physical_resource(aliasB).id) {
        std::cerr << "transient texture aliasing analysis failed\n";
        return 23;
    }
    if (aliasGraph.transitions().size() != 1 || aliasGraph.transitions()[0].resource.id != aliasA.id) {
        std::cerr << "logical transition provenance was not retained\n";
        return 91;
    }
    shinkou::render::RenderGraph bufferAliasGraph;
    const auto bufferAliasA = bufferAliasGraph.create_buffer({1024, 16, false, false, {}, false, true, true});
    const auto bufferAliasB = bufferAliasGraph.create_buffer({1024, 16, false, false, {}, false, true, true});
    bufferAliasGraph.add_pass("buffer_alias_a", {{bufferAliasA, shinkou::render::ResourceUsage::StorageWrite}},
        [](auto&, const auto&) {});
    bufferAliasGraph.add_pass("buffer_alias_b", {{bufferAliasB, shinkou::render::ResourceUsage::StorageWrite}},
        [](auto&, const auto&) {});
    if (!bufferAliasGraph.compile(&error) || bufferAliasGraph.physical_resource(bufferAliasA).id != bufferAliasGraph.physical_resource(bufferAliasB).id ||
        bufferAliasGraph.diagnostics().aliasedBufferCount != 1 || bufferAliasGraph.diagnostics().physicalBufferCount != 1) {
        std::cerr << "transient buffer aliasing analysis failed\n";
        return 82;
    }
    shinkou::render::RenderGraph stateGraph;
    const auto stateTexture = stateGraph.create_texture({32, 32, 1, 1, "rgba8", true, false, {}});
    stateGraph.add_pass("state_write", {{stateTexture, shinkou::render::ResourceUsage::ColorAttachment}},
        [](auto&, const auto&) {});
    stateGraph.add_pass("state_read", {{stateTexture, shinkou::render::ResourceUsage::ShaderRead}},
        [](auto&, const auto&) {});
    if (!stateGraph.compile(&error)) {
        std::cerr << "resource state graph failed to compile\n";
        return 92;
    }
    auto stateBackend = shinkou::render::create_backend(shinkou::render::BackendApi::Null);
    if (!stateBackend || !stateBackend->initialize({})) return 93;
    std::string stateError;
    stateGraph.execute(*stateBackend, &stateError);
    if (!stateError.empty() || stateBackend->stats().barriers != 2) {
        std::cerr << "resource state transition tracking failed: " << stateError << " barriers="
            << (stateBackend ? stateBackend->stats().barriers : 0) << "\n";
        return 94;
    }
    shinkou::render::RenderGraph transitionFailureGraph;
    const auto transitionFailureTexture = transitionFailureGraph.create_texture({16, 16, 1, 1, "rgba8", true, false, {}});
    transitionFailureGraph.add_pass("transition_failure", {{transitionFailureTexture, shinkou::render::ResourceUsage::ColorAttachment}},
        [](auto&, const auto&) {});
    if (!transitionFailureGraph.compile(&error)) return 95;
    auto transitionFailureBackend = shinkou::render::create_backend(shinkou::render::BackendApi::Null);
    if (!transitionFailureBackend || !transitionFailureBackend->initialize({})) return 96;
    if (transitionFailureBackend->transition_resource({0xdeadbeefu, shinkou::render::ResourceKind::Texture2D},
            shinkou::render::ResourceUsage::ShaderRead) || transitionFailureBackend->last_error().empty()) {
        std::cerr << "invalid transition was not rejected\n";
        return 97;
    }
    transitionFailureBackend->clear_error();
    if (transitionFailureBackend->create_resource({0x100u, shinkou::render::ResourceKind::Buffer},
            shinkou::render::TextureDesc{}) || transitionFailureBackend->last_error().empty()) {
        std::cerr << "backend resource kind mismatch was not rejected\n";
        return 98;
    }
    shinkou::render::RenderGraph aliasContractGraph;
    const auto linearTexture = aliasContractGraph.create_texture({64, 64, 1, 1, "rgba16f", true, false, {}, false, "linear", false});
    const auto aliasHdrTexture = aliasContractGraph.create_texture({64, 64, 1, 1, "rgba16f", true, false, {}, true, "linear", false});
    const auto storageTexture = aliasContractGraph.create_texture({64, 64, 1, 1, "rgba16f", true, false, {}, false, "linear", true});
    aliasContractGraph.add_pass("linear_target", {{linearTexture, shinkou::render::ResourceUsage::ColorAttachment}}, [](auto&, const auto&) {});
    aliasContractGraph.add_pass("hdr_target", {{aliasHdrTexture, shinkou::render::ResourceUsage::ColorAttachment}}, [](auto&, const auto&) {});
    aliasContractGraph.add_pass("storage_target", {{storageTexture, shinkou::render::ResourceUsage::ColorAttachment}}, [](auto&, const auto&) {});
    if (!aliasContractGraph.compile(&error) ||
        aliasContractGraph.physical_resource(linearTexture).id == aliasContractGraph.physical_resource(aliasHdrTexture).id ||
        aliasContractGraph.physical_resource(linearTexture).id == aliasContractGraph.physical_resource(storageTexture).id) {
        std::cerr << "transient texture alias compatibility contract failed\n";
        return 51;
    }
    shinkou::render::RenderGraph crossQueueAliasGraph;
    const auto graphicsTransient = crossQueueAliasGraph.create_texture({32, 32, 1, 1, "rgba8", false, false, {}, false, "linear", true});
    const auto computeTransient = crossQueueAliasGraph.create_texture({32, 32, 1, 1, "rgba8", false, false, {}, false, "linear", true});
    crossQueueAliasGraph.add_pass("independent_graphics_write", {{graphicsTransient, shinkou::render::ResourceUsage::ShaderWrite}},
        [](auto&, const auto&) {}, shinkou::render::RenderQueue::Graphics);
    crossQueueAliasGraph.add_pass("independent_compute_write", {{computeTransient, shinkou::render::ResourceUsage::StorageWrite}},
        [](auto&, const auto&) {}, shinkou::render::RenderQueue::Compute);
    if (!crossQueueAliasGraph.compile(&error) ||
        crossQueueAliasGraph.physical_resource(graphicsTransient).id == crossQueueAliasGraph.physical_resource(computeTransient).id) {
        std::cerr << "cross-queue transient alias ordering contract failed\n";
        return 70;
    }
    shinkou::render::RenderGraph crossQueueSyncGraph;
    const auto sharedQueueResource = crossQueueSyncGraph.create_texture({32, 32, 1, 1, "rgba8", false, false, {}, false, "linear", true});
    crossQueueSyncGraph.add_pass("graphics_producer", {{sharedQueueResource, shinkou::render::ResourceUsage::ShaderWrite}},
        [](auto&, const auto&) {}, shinkou::render::RenderQueue::Graphics);
    crossQueueSyncGraph.add_pass("compute_consumer", {{sharedQueueResource, shinkou::render::ResourceUsage::StorageRead}},
        [](auto&, const auto&) {}, shinkou::render::RenderQueue::Compute);
    if (!crossQueueSyncGraph.compile(&error) || crossQueueSyncGraph.queue_dependencies().empty() ||
        crossQueueSyncGraph.queue_batches().size() != 2 || crossQueueSyncGraph.queue_batches()[1].waitBatches.size() != 1) {
        std::cerr << "cross-queue write/read semaphore contract failed\n";
        return 71;
    }
    const auto invalidStorageMaterial = renderer.create_material({"invalid_storage_material", recreatedPipeline,
        {{"storage", runtimeTexture, shinkou::render::DescriptorType::StorageTexture, 0, 0}}, false});
    if (invalidStorageMaterial) {
        std::cerr << "storage descriptor contract was not enforced\n";
        return 52;
    }
    auto validationBackend = shinkou::render::create_backend(shinkou::render::BackendApi::Vulkan);
    if (!validationBackend || !validationBackend->initialize({nullptr, 1, 1, true, true})) {
        std::cerr << "Vulkan validation-capable backend failed to initialize without a window\n";
        return 54;
    }
    auto runHeadlessFrame = [&](shinkou::render::BackendApi api, const char* name) {
        auto headlessBackend = shinkou::render::create_backend(api);
        if (!headlessBackend || !headlessBackend->initialize({nullptr, 64, 64, false, false})) return true;
        shinkou::render::RenderGraph headlessGraph;
        const auto target = headlessGraph.create_texture({64, 64, 1, 1, "rgba8", true, false, {}});
        headlessGraph.add_pass("headless_target", {{target, shinkou::render::ResourceUsage::ColorAttachment}},
            [](auto& backend, const auto&) { backend.set_viewport(0.0f, 0.0f, 64.0f, 64.0f); });
        std::string headlessError;
        if (!headlessGraph.compile(&headlessError)) {
            std::cerr << name << " headless graph compile failed: " << headlessError << '\n';
            return false;
        }
        for (int frame = 0; frame < 3; ++frame) {
            headlessGraph.execute(*headlessBackend, &headlessError);
            if (!headlessError.empty()) break;
        }
        if (!headlessError.empty() || headlessBackend->stats().frames != 3 ||
            headlessBackend->stats().passes != 3 || headlessBackend->stats().queueSubmissions != 3) {
            const auto stats = headlessBackend->stats();
            std::cerr << name << " headless frame submission failed: " << headlessError
                << " frames=" << stats.frames << " passes=" << stats.passes
                << " submissions=" << stats.queueSubmissions << '\n';
            return false;
        }
        shinkou::render::RenderGraph transitionGraph;
        const auto transitionTexture = transitionGraph.create_texture({32, 32, 1, 1, "rgba8", true, false, {}});
        transitionGraph.add_pass("transition_write", {{transitionTexture, shinkou::render::ResourceUsage::ColorAttachment}},
            [](auto&, const auto&) {});
        transitionGraph.add_pass("transition_read", {{transitionTexture, shinkou::render::ResourceUsage::ShaderRead}},
            [](auto&, const auto&) {});
        std::string transitionError;
        if (!transitionGraph.compile(&transitionError)) return false;
        const auto beforeBarriers = headlessBackend->stats().barriers;
        transitionGraph.execute(*headlessBackend, &transitionError);
        if (!transitionError.empty() || headlessBackend->stats().barriers - beforeBarriers < 2) {
            std::cerr << name << " backend state transition validation failed: " << transitionError
                << " barriers=" << (headlessBackend->stats().barriers - beforeBarriers) << '\n';
            return false;
        }
        headlessBackend->wait_idle();
        return true;
    };
    if (!runHeadlessFrame(shinkou::render::BackendApi::DirectX11, "DX11") ||
        !runHeadlessFrame(shinkou::render::BackendApi::DirectX12, "DX12") ||
        !runHeadlessFrame(shinkou::render::BackendApi::Vulkan, "Vulkan")) return 72;
    auto crossQueueBackend = shinkou::render::create_backend(shinkou::render::BackendApi::Vulkan);
    if (crossQueueBackend && crossQueueBackend->initialize({nullptr, 64, 64, false, true})) {
        shinkou::render::RenderGraph crossQueueRuntime;
        std::string crossQueueError;
        const auto sharedBuffer = crossQueueRuntime.create_buffer({256, 16, false, false, {}, false, false, true});
        crossQueueRuntime.add_pass("graphics_write", {{sharedBuffer, shinkou::render::ResourceUsage::ShaderWrite}},
            [](auto&, const auto&) {}, shinkou::render::RenderQueue::Graphics);
        crossQueueRuntime.add_pass("compute_read", {{sharedBuffer, shinkou::render::ResourceUsage::StorageRead}},
            [](auto&, const auto&) {}, shinkou::render::RenderQueue::Compute);
        if (!crossQueueRuntime.compile(&crossQueueError)) {
            std::cerr << "Vulkan cross-queue graph compile failed: " << crossQueueError << '\n';
            return 73;
        }
        for (int frame = 0; frame < 4; ++frame) crossQueueRuntime.execute(*crossQueueBackend, &crossQueueError);
        const auto crossQueueStats = crossQueueBackend->stats();
        if (!crossQueueError.empty() || crossQueueStats.queueSubmissions != 8 ||
            (crossQueueBackend->capabilities().supportsValidation && crossQueueStats.validationMessages != 0)) {
            std::cerr << "Vulkan cross-queue runtime validation failed: " << crossQueueError
                << " submissions=" << crossQueueStats.queueSubmissions
                << " validation=" << crossQueueStats.validationMessages << '\n';
            for (const auto& diagnostic : crossQueueStats.validationDiagnostics) std::cerr << diagnostic << '\n';
            return 74;
        }
        crossQueueBackend->wait_idle();
    }
    auto copyQueueBackend = shinkou::render::create_backend(shinkou::render::BackendApi::Vulkan);
    if (copyQueueBackend && copyQueueBackend->initialize({nullptr, 64, 64, false, true})) {
        shinkou::render::RenderGraph copyQueueRuntime;
        std::string copyQueueError;
        const auto copyBuffer = copyQueueRuntime.create_buffer({256, 16, false, false, {}, false, false, true});
        copyQueueRuntime.add_pass("copy_producer", {{copyBuffer, shinkou::render::ResourceUsage::CopyDestination}},
            [](auto&, const auto&) {}, shinkou::render::RenderQueue::Copy);
        copyQueueRuntime.add_pass("compute_consumer", {{copyBuffer, shinkou::render::ResourceUsage::StorageRead}},
            [](auto&, const auto&) {}, shinkou::render::RenderQueue::Compute);
        if (!copyQueueRuntime.compile(&copyQueueError)) {
            std::cerr << "Vulkan copy-queue graph compile failed: " << copyQueueError << '\n';
            return 75;
        }
        for (int frame = 0; frame < 3; ++frame) copyQueueRuntime.execute(*copyQueueBackend, &copyQueueError);
        const auto copyQueueStats = copyQueueBackend->stats();
        if (!copyQueueError.empty() || copyQueueStats.queueSubmissions != 6 ||
            (copyQueueBackend->capabilities().supportsValidation && copyQueueStats.validationMessages != 0)) {
            std::cerr << "Vulkan copy-queue runtime validation failed: " << copyQueueError
                << " submissions=" << copyQueueStats.queueSubmissions
                << " validation=" << copyQueueStats.validationMessages << '\n';
            for (const auto& diagnostic : copyQueueStats.validationDiagnostics) std::cerr << diagnostic << '\n';
            return 76;
        }
        copyQueueBackend->wait_idle();
    }
    return 0;
}

#include "shinkou/platform/Window.h"
#include "shinkou/render/Renderer.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
#ifndef SHINKOU_SHADER_SCENE_ASSET_DIR
#define SHINKOU_SHADER_SCENE_ASSET_DIR ""
#endif
#ifndef SHINKOU_SHADER_SCENE_SOURCE_DIR
#define SHINKOU_SHADER_SCENE_SOURCE_DIR ""
#endif

std::vector<std::uint8_t> load_binary(const std::string& name) {
    std::ifstream file(std::string(SHINKOU_SHADER_SCENE_ASSET_DIR) + "/" + name, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg();
    if (size <= 0) return {};
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return file ? data : std::vector<std::uint8_t>{};
}

std::string load_text(const std::string& name) {
    std::ifstream file(std::string(SHINKOU_SHADER_SCENE_SOURCE_DIR) + "/" + name);
    if (!file) return {};
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

shinkou::render::BackendApi select_backend(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "dx11") return shinkou::render::BackendApi::DirectX11;
        if (std::string(argv[index]) == "dx12") return shinkou::render::BackendApi::DirectX12;
    }
    return shinkou::render::BackendApi::Vulkan;
}

bool has_argument(int argc, char** argv, const char* argument) {
    for (int index = 1; index < argc; ++index) if (std::string(argv[index]) == argument) return true;
    return false;
}

std::uint64_t frame_limit(int argc, char** argv) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "--frames") return std::strtoull(argv[index + 1], nullptr, 10);
    }
    return 0;
}

std::array<float, 8> build_frame_state(double elapsedSeconds, std::uint64_t frameIndex) {
    std::array<float, 8> state{};
    state[0] = static_cast<float>(std::max(0.0, elapsedSeconds));
    state[1] = static_cast<float>(frameIndex);
    const double yaw = -0.09 + std::sin(elapsedSeconds * 0.20) * 0.22;
    const double pitch = -0.16 + std::sin(elapsedSeconds * 0.13) * 0.06;
    const double distance = 22.0 + std::sin(elapsedSeconds * 0.17) * 0.6;
    state[4] = static_cast<float>(yaw / 6.28318530718 + 0.5);
    state[5] = static_cast<float>(pitch / 2.4 + 0.5);
    state[6] = static_cast<float>((distance - 18.0) / 8.0);
    state[7] = 1.0f;
    return state;
}

std::vector<std::uint8_t> to_bytes(const std::array<float, 8>& state) {
    std::vector<std::uint8_t> bytes(sizeof(state));
    std::memcpy(bytes.data(), state.data(), bytes.size());
    return bytes;
}

shinkou::render::ShaderDesc make_shader(shinkou::render::BackendApi backend,
                                      shinkou::render::ShaderStage stage,
                                      std::string name,
                                      std::string sourceFile,
                                      std::string binaryStem) {
    shinkou::render::ShaderDesc shader;
    shader.stage = stage;
    shader.name = std::move(name);
    shader.entryPoint = stage == shinkou::render::ShaderStage::Vertex ? "VSMain" : "PSMain";
    if (backend == shinkou::render::BackendApi::Vulkan) {
        shader.sourceKind = shinkou::render::ShaderSourceKind::SpirV;
        shader.bytecode = load_binary(binaryStem + ".spv");
    } else if (backend == shinkou::render::BackendApi::DirectX12) {
        shader.sourceKind = shinkou::render::ShaderSourceKind::Dxil;
        shader.bytecode = load_binary(binaryStem + ".dxil");
    } else {
        shader.sourceKind = shinkou::render::ShaderSourceKind::Source;
        shader.profile = stage == shinkou::render::ShaderStage::Vertex ? "vs_5_0" : "ps_5_0";
        shader.source = load_text(sourceFile);
    }
    return shader;
}
}

int main(int argc, char** argv) {
    constexpr std::uint32_t width = 1280;
    constexpr std::uint32_t height = 720;
    const auto backend = select_backend(argc, argv);
    const bool renderOnce = has_argument(argc, argv, "--once");
    const bool enableMrt = has_argument(argc, argv, "--mrt");
    const bool enableValidation = has_argument(argc, argv, "--validation");
    const auto requestedFrames = frame_limit(argc, argv);

    shinkou::platform::Window window;
    if (!window.create({"ShinkouEngine Reference Shader Test - Kerr-Newman Black Hole", width, height, true})) {
        std::cerr << "shader scene window creation failed\n";
        return 1;
    }
    shinkou::render::Renderer renderer(backend, {window.native_handle(), width, height, false, enableValidation});
    if (!renderer.initialize()) {
        std::cerr << "shader scene renderer initialization failed: " << renderer.last_error() << '\n';
        window.destroy();
        return 2;
    }

    const auto vertex = renderer.create_shader(make_shader(backend,
        shinkou::render::ShaderStage::Vertex, "reference_fullscreen_vertex", "shader_scene.hlsl", "shader_scene_vs"));
    const auto blackhole = renderer.create_shader(make_shader(backend,
        shinkou::render::ShaderStage::Fragment, "reference_buffer_a_blackhole", "shader_scene.hlsl", "shader_scene_ps"));
    const auto postB = renderer.create_shader(make_shader(backend,
        shinkou::render::ShaderStage::Fragment, "reference_buffer_b_bloom", "reference_post_b.hlsl", "reference_post_b"));
    const auto blurH = renderer.create_shader(make_shader(backend,
        shinkou::render::ShaderStage::Fragment, "reference_buffer_c_blur_horizontal", "reference_blur_h.hlsl", "reference_blur_h"));
    const auto blurV = renderer.create_shader(make_shader(backend,
        shinkou::render::ShaderStage::Fragment, "reference_buffer_d_blur_vertical", "reference_blur_v.hlsl", "reference_blur_v"));
    const auto image = renderer.create_shader(make_shader(backend,
        shinkou::render::ShaderStage::Fragment, "reference_image_composite", "reference_image_pass.hlsl", "reference_image_pass"));
    if (!vertex || !blackhole || !postB || !blurH || !blurV || !image) {
        std::cerr << "reference shader assets are missing; build shinkou_shader_scene_shaders first\n";
        window.destroy();
        return 3;
    }

    shinkou::render::PipelineDesc pipelineADesc{"reference_buffer_a", vertex.id, blackhole.id, 0, false, false, false};
    if (enableMrt) pipelineADesc.colorFormats = {"rgba8", "rgba16f"};
    const auto pipelineA = renderer.create_pipeline(pipelineADesc);
    const auto pipelineB = renderer.create_pipeline({"reference_buffer_b", vertex.id, postB.id, 0, false, false, false});
    const auto pipelineC = renderer.create_pipeline({"reference_buffer_c", vertex.id, blurH.id, 0, false, false, false});
    const auto pipelineD = renderer.create_pipeline({"reference_buffer_d", vertex.id, blurV.id, 0, false, false, false});
    const auto pipelineImage = renderer.create_pipeline({"reference_image", vertex.id, image.id, 0, false, false, false});

    const shinkou::render::TextureDesc targetDesc{width, height, 1, 1, "rgba8", true, false, {}};
    const auto initialState = build_frame_state(0.0, 0);
    const shinkou::render::BufferDesc stateDesc{sizeof(initialState), sizeof(float) * 4, false, false, to_bytes(initialState)};
    const auto bufferA = renderer.create_texture(targetDesc);
    const auto mrtTargetDesc = shinkou::render::TextureDesc{width, height, 1, 1, "rgba16f", true, false, {}, true, "linear"};
    const auto bufferMrt = enableMrt ? renderer.create_texture(mrtTargetDesc) : shinkou::render::ResourceHandle{};
    const auto bufferB = renderer.create_texture(targetDesc);
    const auto bufferC = renderer.create_texture(targetDesc);
    const auto bufferD = renderer.create_texture(targetDesc);
    const auto stateBuffer = renderer.create_buffer(stateDesc);

    const auto materialA = renderer.create_material({"reference_buffer_a_material", pipelineA,
        {{"frameState", stateBuffer, shinkou::render::DescriptorType::UniformBuffer, 2, backend == shinkou::render::BackendApi::Vulkan ? 1u : 0u}}, false});
    const auto materialB = renderer.create_material({"reference_buffer_b_material", pipelineB,
        {{"sourceImage", bufferA, shinkou::render::DescriptorType::Texture, 0, backend == shinkou::render::BackendApi::Vulkan ? 1u : 0u}}, false});
    const auto materialC = renderer.create_material({"reference_buffer_c_material", pipelineC,
        {{"sourceImage", bufferB, shinkou::render::DescriptorType::Texture, 0, backend == shinkou::render::BackendApi::Vulkan ? 1u : 0u}}, false});
    const auto materialD = renderer.create_material({"reference_buffer_d_material", pipelineD,
        {{"sourceImage", bufferC, shinkou::render::DescriptorType::Texture, 0, backend == shinkou::render::BackendApi::Vulkan ? 1u : 0u}}, false});
    const auto materialImage = renderer.create_material({"reference_image_material", pipelineImage,
        {{"colorImage", bufferA, shinkou::render::DescriptorType::Texture, 0, backend == shinkou::render::BackendApi::Vulkan ? 1u : 0u},
         {"bloomImage", bufferD, shinkou::render::DescriptorType::Texture, 1, backend == shinkou::render::BackendApi::Vulkan ? 1u : 0u}}, false});

    const auto startTime = std::chrono::steady_clock::now();
    std::uint64_t frameIndex = 0;
    std::uint64_t renderedFrames = 0;
    auto renderFrame = [&]() {
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
        const auto currentFrame = frameIndex++;
        const auto state = build_frame_state(elapsed, currentFrame);
        if (!renderer.update_buffer({stateBuffer, 0, to_bytes(state)})) {
            return false;
        }
        renderer.begin_graph();
        renderer.graph().import_texture(bufferA, targetDesc);
        if (bufferMrt) renderer.graph().import_texture(bufferMrt, mrtTargetDesc);
        renderer.graph().import_texture(bufferB, targetDesc);
        renderer.graph().import_texture(bufferC, targetDesc);
        renderer.graph().import_texture(bufferD, targetDesc);
        renderer.graph().add_pass("reference_buffer_a", enableMrt ? std::vector<shinkou::render::ResourceAccess>{
            {bufferA, shinkou::render::ResourceUsage::ColorAttachment0},
            {bufferMrt, shinkou::render::ResourceUsage::ColorAttachment1}} :
            std::vector<shinkou::render::ResourceAccess>{{bufferA, shinkou::render::ResourceUsage::ColorAttachment}},
            [materialA](auto& backendRef, const auto&) {
                backendRef.bind_material(materialA);
                backendRef.draw_sprite({});
            });
        renderer.graph().add_pass("reference_buffer_b", {
            {bufferA, shinkou::render::ResourceUsage::ShaderRead},
            {bufferB, shinkou::render::ResourceUsage::ColorAttachment}},
            [materialB](auto& backendRef, const auto&) {
                backendRef.bind_material(materialB);
                backendRef.draw_sprite({});
            });
        renderer.graph().add_pass("reference_buffer_c", {
            {bufferB, shinkou::render::ResourceUsage::ShaderRead},
            {bufferC, shinkou::render::ResourceUsage::ColorAttachment}},
            [materialC](auto& backendRef, const auto&) {
                backendRef.bind_material(materialC);
                backendRef.draw_sprite({});
            });
        renderer.graph().add_pass("reference_buffer_d", {
            {bufferC, shinkou::render::ResourceUsage::ShaderRead},
            {bufferD, shinkou::render::ResourceUsage::ColorAttachment}},
            [materialD](auto& backendRef, const auto&) {
                backendRef.bind_material(materialD);
                backendRef.draw_sprite({});
            });
        renderer.graph().add_pass("reference_image", {
            {bufferA, shinkou::render::ResourceUsage::ShaderRead},
            {bufferD, shinkou::render::ResourceUsage::ShaderRead}},
            [materialImage](auto& backendRef, const auto&) {
                backendRef.bind_material(materialImage);
                backendRef.draw_sprite({});
            });
        renderer.submit();
        return renderer.last_error().empty();
    };

    if (renderOnce) {
        if (!renderFrame()) {
            std::cerr << "shader scene submission failed: " << renderer.last_error() << '\n';
            window.destroy();
            return 4;
        }
        renderedFrames = 1;
    } else {
        while (window.is_open() && (requestedFrames == 0 || renderedFrames < requestedFrames)) {
            window.process_events();
            const auto frameStart = std::chrono::steady_clock::now();
            if (!renderFrame()) {
                std::cerr << "shader scene submission failed: " << renderer.last_error() << '\n';
                window.destroy();
                return 4;
            }
            ++renderedFrames;
            const auto frameTime = std::chrono::duration<double>(std::chrono::steady_clock::now() - frameStart).count();
            const double remaining = (1.0 / 60.0) - frameTime;
            if (remaining > 0.0) {
                std::this_thread::sleep_for(std::chrono::duration<double>(remaining));
            }
        }
    }

    const auto stats = renderer.stats();
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
    const double fps = elapsed > 0.0 ? static_cast<double>(renderedFrames) / elapsed : 0.0;
    std::cout << "shader-scene=reference-kerr-newman-buffer-chain backend=" << static_cast<int>(renderer.capabilities().api)
              << " frames=" << stats.frames << " passes=" << stats.passes << " draws=" << stats.drawCalls
              << " elapsed=" << elapsed << " fps=" << fps
              << " validation=" << renderer.capabilities().supportsValidation
              << " validation-messages=" << stats.validationMessages << '\n';
    for (const auto& diagnostic : stats.validationDiagnostics) std::cout << "validation: " << diagnostic << '\n';
    window.destroy();
    return 0;
}

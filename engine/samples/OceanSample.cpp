#include "shinkou/Engine.h"
#include "shinkou/Math.h"
#include "shinkou/render/RenderBackend.h"
#include "shinkou/render/RenderTypes.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#if defined(SHINKOU_OCEAN_WITH_IMGUI)
#include <imgui.h>
#if defined(_WIN32)
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_dx12.h>
#endif
#if defined(SHINKOU_WITH_VULKAN)
#include <backends/imgui_impl_vulkan.h>
#endif
#endif

namespace {
#ifndef SHINKOU_SAMPLE_SHADER_DIR
#define SHINKOU_SAMPLE_SHADER_DIR ""
#endif
#ifndef SHINKOU_SAMPLE_SOURCE_DIR
#define SHINKOU_SAMPLE_SOURCE_DIR ""
#endif

using shinkou::math::Vec3;
using shinkou::math::Vec2;
using shinkou::math::Vec4;
using shinkou::math::Mat4;
using shinkou::render::BackendApi;

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

std::string load_text_shader(const std::string& name) {
    const std::string path = std::string(SHINKOU_SAMPLE_SOURCE_DIR) + "/shaders/" + name;
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

BackendApi backend_from_arguments(int argc, char** argv) {
    BackendApi result = BackendApi::DirectX11;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--dx11" || argument == "dx11") result = BackendApi::DirectX11;
        else if (argument == "--dx12" || argument == "dx12") result = BackendApi::DirectX12;
        else if (argument == "--vulkan" || argument == "vulkan") result = BackendApi::Vulkan;
    }
    return result;
}

const char* backend_name(BackendApi api) {
    switch (api) {
    case BackendApi::DirectX11: return "dx11";
    case BackendApi::DirectX12: return "dx12";
    case BackendApi::Vulkan: return "vulkan";
    default: return "unsupported";
    }
}

std::uint64_t requested_frames(int argc, char** argv) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "--frames") {
            try { return std::stoull(argv[index + 1]); }
            catch (...) { return 0; }
        }
    }
    return 0;
}

struct alignas(16) OceanFrameData {
    Vec4 cameraPosition{};
    Vec4 cameraForwardTan{};
    Vec4 cameraRightTime{};
    Vec4 cameraUpAspect{};
    Vec4 windParameters{};
    Vec4 lightingParameters{};
    Vec4 renderParams{1000.0f, 1.0f, 1.0f, 1.0f};
    Vec4 waterParameters{0.24f, 0.18f, 0.55f, 0.0f};
    Vec4 resolutionMouse{1280.0f, 720.0f, 0.0f, 0.0f};
    Mat4 reflectionViewProjection{Mat4::Identity()};
};

static_assert(sizeof(OceanFrameData) == 9u * sizeof(Vec4) + sizeof(Mat4));
static_assert(alignof(OceanFrameData) >= 16u);

struct OceanRenderResources {
    shinkou::render::ResourceHandle material{};
    shinkou::render::ResourceHandle noiseTexture{};
    shinkou::render::ResourceHandle noiseSampler{};
    shinkou::render::ResourceHandle frameBuffer{};
};

struct OceanSettings {
    Vec2 windDirection{0.86f, 0.51f};
    float windSpeed{1.0f};
    float waveScale{1.0f};
    float waveLength{1.0f};
    Vec3 sunDirection{0.35f, 0.42f, 0.82f};
    float sunIntensity{1.15f};
    float fogDistance{1000.0f};
    float waterRoughness{0.24f};
    float foamStrength{0.18f};
    float waveChoppiness{0.55f};
};

struct CameraState {
    Vec3 focus{0.0f, 0.0f, 0.0f};
    float distance{3.0f};
    float yaw{0.5f};
    float pitch{-0.5f};
};

Vec3 camera_forward(const CameraState& camera) {
    const float cosPitch = std::cos(camera.pitch);
    return shinkou::math::Normalize(Vec3{
        std::sin(camera.yaw) * cosPitch,
        std::sin(camera.pitch),
        std::cos(camera.yaw) * cosPitch});
}

Vec3 camera_position(const CameraState& camera) {
    const auto forward = camera_forward(camera);
    return {
        camera.focus.x - forward.x * camera.distance,
        camera.focus.y - forward.y * camera.distance,
        camera.focus.z - forward.z * camera.distance};
}

void normalize_settings(OceanSettings& settings) {
    const float length = std::sqrt(settings.windDirection.x * settings.windDirection.x +
                                   settings.windDirection.y * settings.windDirection.y);
    if (!std::isfinite(length) || length < 0.001f) {
        settings.windDirection = {1.0f, 0.0f};
    } else {
        settings.windDirection.x /= length;
        settings.windDirection.y /= length;
    }
    settings.windSpeed = std::clamp(std::isfinite(settings.windSpeed) ? settings.windSpeed : 1.0f, 0.0f, 4.0f);
    settings.waveScale = std::clamp(std::isfinite(settings.waveScale) ? settings.waveScale : 1.0f, 0.05f, 3.0f);
    settings.waveLength = std::clamp(std::isfinite(settings.waveLength) ? settings.waveLength : 1.0f, 0.25f, 4.0f);
    settings.sunIntensity = std::clamp(std::isfinite(settings.sunIntensity) ? settings.sunIntensity : 1.15f, 0.0f, 4.0f);
    settings.fogDistance = std::clamp(std::isfinite(settings.fogDistance) ? settings.fogDistance : 1000.0f, 100.0f, 1000.0f);
    settings.waterRoughness = std::clamp(std::isfinite(settings.waterRoughness) ? settings.waterRoughness : 0.24f, 0.03f, 0.75f);
    settings.foamStrength = std::clamp(std::isfinite(settings.foamStrength) ? settings.foamStrength : 0.18f, 0.0f, 1.0f);
    settings.waveChoppiness = std::clamp(std::isfinite(settings.waveChoppiness) ? settings.waveChoppiness : 0.55f, 0.0f, 1.5f);
    const auto sunLength = shinkou::math::Length(settings.sunDirection);
    if (!std::isfinite(sunLength) || sunLength < 0.001f)
        settings.sunDirection = {0.35f, 0.42f, 0.82f};
    else
        settings.sunDirection = shinkou::math::Normalize(settings.sunDirection);
}

#if defined(SHINKOU_OCEAN_WITH_IMGUI)
void begin_imgui_frame(shinkou::Engine& engine, shinkou::render::Renderer& renderer,
                       float deltaSeconds) {
    auto& io = ImGui::GetIO();
    const auto& mouse = engine.input().mouse();
    io.DisplaySize = ImVec2(static_cast<float>(engine.window().width()),
                            static_cast<float>(engine.window().height()));
    io.DeltaTime = std::clamp(deltaSeconds, 1.0f / 1000.0f, 0.1f);
    io.FontGlobalScale = std::clamp(engine.window().dpi_scale(), 0.75f, 2.0f);
    io.MousePos = ImVec2(mouse.position.x, mouse.position.y);
    io.MouseDown[0] = mouse.buttons[1];
    io.MouseDown[1] = mouse.buttons[3];
    io.MouseDown[2] = mouse.buttons[2];
    io.MouseDown[3] = mouse.buttons[4];
    io.MouseDown[4] = mouse.buttons[5];
    io.MouseWheel = mouse.wheel.y;
    io.MouseWheelH = mouse.wheel.x;
#if defined(_WIN32)
    if (renderer.capabilities().api == BackendApi::DirectX11)
        ImGui_ImplDX11_NewFrame();
    else if (renderer.capabilities().api == BackendApi::DirectX12)
        ImGui_ImplDX12_NewFrame();
#endif
#if defined(SHINKOU_WITH_VULKAN)
    if (renderer.capabilities().api == BackendApi::Vulkan)
        ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();
}

void draw_ocean_controls(OceanSettings& settings, CameraState& camera, BackendApi api) {
    ImGui::SetNextWindowPos(ImVec2(18.0f, 18.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Ocean Controls");
    ImGui::Text("Backend: %s   Target: 60 FPS", backend_name(api));
    ImGui::Text("Right-drag the viewport to orbit the camera");
    ImGui::SeparatorText("Wind and waves");
    float windDirection[2] = {settings.windDirection.x, settings.windDirection.y};
    if (ImGui::SliderFloat2("Wind direction", windDirection, -1.0f, 1.0f, "%.2f"))
        settings.windDirection = {windDirection[0], windDirection[1]};
    ImGui::SliderFloat("Wind speed", &settings.windSpeed, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Wave height", &settings.waveScale, 0.05f, 3.0f, "%.2f");
    ImGui::SliderFloat("Wave length", &settings.waveLength, 0.25f, 4.0f, "%.2f");
    ImGui::SliderFloat("Wave choppiness", &settings.waveChoppiness, 0.0f, 1.5f, "%.2f");
    ImGui::SeparatorText("Lighting and atmosphere");
    ImGui::SliderFloat3("Sun direction", &settings.sunDirection.x, -1.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Sun intensity", &settings.sunIntensity, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Visibility / fog", &settings.fogDistance, 100.0f, 1000.0f, "%.0f m");
    ImGui::SliderFloat("Water roughness", &settings.waterRoughness, 0.03f, 0.75f, "%.2f");
    ImGui::SliderFloat("Foam strength", &settings.foamStrength, 0.0f, 1.0f, "%.2f");
    ImGui::SeparatorText("Camera");
    ImGui::SliderFloat("Camera distance", &camera.distance, 1.5f, 12.0f, "%.1f m");
    if (ImGui::Button("Reset camera")) {
        camera.focus = {0.0f, 0.0f, 0.0f};
        camera.distance = 3.0f;
        camera.yaw = 0.5f;
        camera.pitch = -0.5f;
    }
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::End();
}

void update_camera_from_mouse(shinkou::Engine& engine, CameraState& camera) {
    const auto& mouse = engine.input().mouse();
    if (!mouse.buttons[3] || ImGui::GetIO().WantCaptureMouse) return;
    constexpr float sensitivity = 0.0045f;
    camera.yaw += mouse.delta.x * sensitivity;
    camera.pitch = std::clamp(camera.pitch - mouse.delta.y * sensitivity, -1.35f, 1.35f);
}
#endif

template <typename T>
std::vector<std::uint8_t> bytes_of(const T& value) {
    std::vector<std::uint8_t> result(sizeof(value));
    std::memcpy(result.data(), &value, sizeof(value));
    return result;
}

std::vector<std::uint8_t> make_reference_noise_texture() {
    constexpr std::uint32_t size = 256;
    std::vector<std::uint8_t> result(static_cast<std::size_t>(size) * size * 4u);
    const auto hash = [](std::uint32_t value) {
        value ^= value >> 16;
        value *= 0x7feb352du;
        value ^= value >> 15;
        value *= 0x846ca68bu;
        value ^= value >> 16;
        return value;
    };
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const auto seed = hash(x + hash(y + 0x9e3779b9u));
            const auto seed1 = hash(seed + 0x68bc21ebu);
            const auto seed2 = hash(seed + 0x02e5be93u);
            const auto seed3 = hash(seed + 0x967a889bu);
            const auto offset = (static_cast<std::size_t>(y) * size + x) * 4u;
            result[offset + 0] = static_cast<std::uint8_t>(seed >> 24);
            result[offset + 1] = static_cast<std::uint8_t>(seed1 >> 24);
            result[offset + 2] = static_cast<std::uint8_t>(seed2 >> 24);
            result[offset + 3] = static_cast<std::uint8_t>(seed3 >> 24);
        }
    }
    return result;
}

bool create_ocean_resources(shinkou::Engine& engine, BackendApi api,
                            OceanRenderResources& resources) {
    auto& renderer = engine.renderer();
    shinkou::render::TextureDesc noiseDescription;
    noiseDescription.width = 256;
    noiseDescription.height = 256;
    noiseDescription.format = "rgba8";
    noiseDescription.colorSpace = "linear";
    noiseDescription.initialData = make_reference_noise_texture();
    noiseDescription.retainCpuCopy = false;
    resources.noiseTexture = renderer.create_texture(noiseDescription);
    // The reference samples a wrapping 256x256 noise channel with explicit LOD 0.
    resources.noiseSampler = renderer.create_sampler({"linear", "repeat", "repeat", "repeat", 1.0f, false});
    if (!resources.noiseTexture || !resources.noiseSampler) {
        std::cerr << "ocean reference noise resource creation failed: " << renderer.last_error() << '\n';
        return false;
    }

    shinkou::render::ShaderDesc vertexShader;
    vertexShader.stage = shinkou::render::ShaderStage::Vertex;
    vertexShader.name = "ocean_vs";
    vertexShader.entryPoint = "VSMain";
    shinkou::render::ShaderDesc fragmentShader;
    fragmentShader.stage = shinkou::render::ShaderStage::Fragment;
    fragmentShader.name = "ocean_ps";
    fragmentShader.entryPoint = "PSMain";

    if (api == BackendApi::Vulkan) {
        vertexShader.sourceKind = shinkou::render::ShaderSourceKind::SpirV;
        fragmentShader.sourceKind = shinkou::render::ShaderSourceKind::SpirV;
        vertexShader.bytecode = load_binary_shader("ocean_vs.spv");
        fragmentShader.bytecode = load_binary_shader("ocean_ps.spv");
    } else if (api == BackendApi::DirectX12) {
        vertexShader.sourceKind = shinkou::render::ShaderSourceKind::Dxil;
        fragmentShader.sourceKind = shinkou::render::ShaderSourceKind::Dxil;
        vertexShader.bytecode = load_binary_shader("ocean_vs.dxil");
        fragmentShader.bytecode = load_binary_shader("ocean_ps.dxil");
    } else {
        vertexShader.sourceKind = shinkou::render::ShaderSourceKind::Source;
        fragmentShader.sourceKind = shinkou::render::ShaderSourceKind::Source;
        vertexShader.profile = "vs_5_0";
        fragmentShader.profile = "ps_5_0";
        vertexShader.source = load_text_shader("ocean.hlsl");
        fragmentShader.source = vertexShader.source;
    }

    if ((api == BackendApi::Vulkan || api == BackendApi::DirectX12) &&
        (vertexShader.bytecode.empty() || fragmentShader.bytecode.empty())) {
        std::cerr << "ocean shader binaries are missing; build shinkou_sample_shaders first\n";
        return false;
    }
    if (api == BackendApi::DirectX11 &&
        (vertexShader.source.empty() || fragmentShader.source.empty())) {
        std::cerr << "ocean HLSL source is missing\n";
        return false;
    }

    const auto vertex = renderer.create_shader(vertexShader);
    const auto fragment = renderer.create_shader(fragmentShader);
    if (!vertex || !fragment) {
        std::cerr << "ocean shader creation failed: " << renderer.last_error() << '\n';
        return false;
    }

    shinkou::render::PipelineDesc pipeline{
        "ocean_pipeline", vertex.id, fragment.id, 0, false, false, false, false};
    pipeline.colorFormat = "bgra8";
    pipeline.depthFormat = "none";
    pipeline.cullMode = "none";
    const auto pipelineHandle = renderer.create_pipeline(pipeline);
    if (!pipelineHandle) {
        std::cerr << "ocean pipeline creation failed: " << renderer.last_error() << '\n';
        return false;
    }
    const std::vector<shinkou::render::DescriptorBinding> referenceBindings{
        {"noiseTexture", resources.noiseTexture, shinkou::render::DescriptorType::Texture, 0, 0},
        {"noiseSampler", resources.noiseSampler, shinkou::render::DescriptorType::Sampler, 1, 0}};
    resources.material = renderer.create_material({"ocean_reference_material", pipelineHandle, referenceBindings, false});
    if (!resources.material) {
        std::cerr << "ocean material creation failed: " << renderer.last_error() << '\n';
        return false;
    }

    OceanFrameData initialFrame;
    shinkou::render::BufferDesc frameDescription;
    frameDescription.size = sizeof(OceanFrameData);
    frameDescription.stride = sizeof(Vec4);
    frameDescription.uniformBuffer = true;
    frameDescription.initialData = bytes_of(initialFrame);
    resources.frameBuffer = renderer.create_buffer(frameDescription);
    if (!resources.frameBuffer) {
        std::cerr << "ocean frame buffer creation failed: " << renderer.last_error() << '\n';
        return false;
    }
    return true;
}

}

int main(int argc, char** argv) {
    const auto api = backend_from_arguments(argc, argv);
    shinkou::EngineConfig config;
    config.renderBackend = api;
    config.window.title = std::string("Shinkou Ocean - ") + backend_name(api);
    config.window.width = 1280;
    config.window.height = 720;
    config.window.visible = true;
    config.targetFrameRate = 60.0;
    config.maxDeltaSeconds = 0.1f;
    config.audio.startDevice = false;

    shinkou::Engine engine(config);
    if (!engine.initialize()) {
        std::cerr << "ocean engine initialization failed\n";
        return 1;
    }

#if defined(SHINKOU_OCEAN_WITH_IMGUI)
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendPlatformName = "ShinkouInput";
    ImGui::GetIO().BackendRendererName = backend_name(api);
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui::GetStyle().FrameRounding = 4.0f;
    if (!engine.renderer().initialize_imgui()) {
        std::cerr << "ocean ImGui renderer initialization failed: "
                  << engine.renderer().last_error() << '\n';
        ImGui::DestroyContext();
        engine.shutdown();
        return 2;
    }
#endif

    OceanRenderResources resources;
    if (!create_ocean_resources(engine, api, resources)) {
#if defined(SHINKOU_OCEAN_WITH_IMGUI)
        engine.renderer().shutdown_imgui();
        ImGui::DestroyContext();
#endif
        engine.shutdown();
        return 3;
    }

    const auto* frameDescription = engine.renderer().resource_description(resources.frameBuffer);
    if (!frameDescription) {
        std::cerr << "ocean frame buffer description is unavailable\n";
#if defined(SHINKOU_OCEAN_WITH_IMGUI)
        engine.renderer().shutdown_imgui();
        ImGui::DestroyContext();
#endif
        engine.shutdown();
        return 4;
    }
    OceanFrameData frameData;
    std::vector<std::uint8_t> frameBytes(sizeof(frameData));
    float elapsedTime = 0.0f;
    OceanSettings settings;
    CameraState camera;
    normalize_settings(settings);

    engine.set_render_callback([&, resources](
        shinkou::render::Renderer& renderer, shinkou::World&, shinkou::Seconds dt,
        shinkou::FrameIndex) {
        elapsedTime += std::max(dt, 0.0f);
#if defined(SHINKOU_OCEAN_WITH_IMGUI)
        begin_imgui_frame(engine, renderer, static_cast<float>(dt));
        draw_ocean_controls(settings, camera, api);
        update_camera_from_mouse(engine, camera);
        ImGui::Render();
        renderer.set_imgui_draw_data(ImGui::GetDrawData());
#endif
        normalize_settings(settings);
        const auto width = std::max<std::uint32_t>(engine.window().width(), 1u);
        const auto height = std::max<std::uint32_t>(engine.window().height(), 1u);
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        const Vec3 cameraPosition = camera_position(camera);
        const Vec3 cameraForward = camera_forward(camera);
        const Vec3 cameraRight = shinkou::math::Normalize(
            shinkou::math::Cross(Vec3{0.0f, 1.0f, 0.0f}, cameraForward));
        const Vec3 cameraUp = shinkou::math::Cross(cameraForward, cameraRight);

        frameData.cameraPosition = {cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f};
        frameData.cameraForwardTan = {cameraForward.x, cameraForward.y, cameraForward.z,
            std::tan(shinkou::math::Radians(58.0f) * 0.5f)};
        frameData.cameraRightTime = {cameraRight.x, cameraRight.y, cameraRight.z, elapsedTime};
        frameData.cameraUpAspect = {cameraUp.x, cameraUp.y, cameraUp.z, aspect};
        frameData.windParameters = {settings.windDirection.x, settings.windDirection.y,
            0.0f, settings.windSpeed};
        frameData.lightingParameters = {settings.sunDirection.x, settings.sunDirection.y,
            settings.sunDirection.z, settings.sunIntensity};
        frameData.renderParams = {settings.fogDistance, settings.waveScale,
            settings.waveLength, settings.sunIntensity};
        frameData.waterParameters = {settings.waterRoughness, settings.foamStrength,
            settings.waveChoppiness, 0.0f};
        frameData.resolutionMouse = {static_cast<float>(width), static_cast<float>(height), 0.0f, 0.0f};
        frameData.reflectionViewProjection = Mat4::Identity();
        std::memcpy(frameBytes.data(), &frameData, sizeof(frameData));
        if (!renderer.update_buffer({resources.frameBuffer, 0, frameBytes})) return;

        auto& graph = renderer.graph();
        const auto import_external = [&](shinkou::render::ResourceHandle handle) {
            const auto* description = renderer.resource_description(handle);
            if (!description) return false;
            graph.import_resource(handle, *description);
            return true;
        };
        if (!import_external(resources.frameBuffer) ||
            !import_external(resources.material) ||
            !import_external(resources.noiseTexture) ||
            !import_external(resources.noiseSampler)) return;

        graph.add_pass("ocean_reference_raymarch", {
            {resources.noiseTexture, shinkou::render::ResourceUsage::ShaderRead},
            {resources.material, shinkou::render::ResourceUsage::ShaderRead},
            {resources.frameBuffer, shinkou::render::ResourceUsage::UniformBuffer}},
            [resources, width, height](shinkou::render::IRenderBackend& backend, const auto&) {
                backend.set_render_target({});
                backend.set_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
                backend.bind_material(resources.material);
                backend.bind_uniform_buffer(resources.frameBuffer, 2, 0);
                backend.draw_sprite({});
            }, shinkou::render::RenderQueue::Graphics, true, false);
    });

    std::cerr << "ocean backend=" << backend_name(api)
              << " resolution=" << config.window.width << 'x' << config.window.height
              << " target_fps=60 fog_distance_m=1000\n";
    const bool ran = engine.run(requested_frames(argc, argv));
    const auto error = engine.renderer().last_error();
#if defined(SHINKOU_OCEAN_WITH_IMGUI)
    engine.renderer().shutdown_imgui();
    ImGui::DestroyContext();
#endif
    engine.shutdown();
    if (!ran) {
        std::cerr << "ocean engine run failed\n";
        return 4;
    }
    if (!error.empty()) {
        std::cerr << "ocean renderer error: " << error << '\n';
        return 5;
    }
    return 0;
}

#include "shinkou/Engine.h"
#include "shinkou/components/CameraComponent.h"
#include "shinkou/components/PrimitiveMeshComponent.h"
#include "shinkou/editor/EditorPreferences.h"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#if defined(_WIN32)
#include <shellapi.h>
#include <windows.h>
#endif

namespace {
using namespace shinkou;
void seed_scene(Engine &engine) {
    auto &world = engine.world();
    auto &scene = world.create_object("Demo Scene");
    const auto mesh = [&](const char *name, math::Vec3 position, math::Vec3 scale,
                          math::Vec3 color) -> GameObject & {
        auto &object = scene.create_child(name);
        auto *transform = object.get_component<components::TransformComponent>();
        transform->set_position(position);
        transform->set_scale(scale);
        object.add_component<components::PrimitiveMeshComponent>()->set_color(color);
        return object;
    };
    const auto selected = mesh("Demo Mesh", {-1, .7f, 0}, {1, 1, 1}, {.32f, .56f, .82f}).id();
    mesh("Companion Mesh", {1.1f, .5f, .7f}, {.7f, .7f, .7f}, {.8f, .36f, .13f});
    mesh("Ground", {0, -.12f, 2}, {6, .12f, 6}, {.26f, .28f, .31f});
    auto &camera = scene.create_child("Game Camera");
    camera.add_component<components::CameraComponent>();
    math::Transform pose;
    pose.position = {3, 2.4f, -6};
    pose.rotation = math::FromAxisAngle({0, 1, 0}, -.42f) * math::FromAxisAngle({1, 0, 0}, .25f);
    camera.get_component<components::TransformComponent>()->set_local_transform(pose);
    world.refresh_transforms();
    engine.editor().select_object(selected, world);
}
int run_editor(const std::vector<std::string> &arguments) {
    std::filesystem::path project = SHINKOU_EDITOR_DEFAULT_PROJECT;
    std::string backend, theme;
    std::uint64_t frames = 0;
    bool validation = false;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const auto &argument = arguments[i];
        if (argument == "--project" && i + 1 < arguments.size())
            project = std::filesystem::u8path(arguments[++i]);
        else if (argument == "--theme" && i + 1 < arguments.size())
            theme = arguments[++i];
        else if (argument == "--render-validation")
            validation = true;
        else if (argument == "--frames" && i + 1 < arguments.size()) {
            const auto &value = arguments[++i];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), frames);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                return 2;
        } else if (argument == "dx11" || argument == "dx12" || argument == "vulkan")
            backend = argument;
        else if (argument != "--editor")
            return 2;
    }
    project = std::filesystem::absolute(project).lexically_normal();
    editor::FileSystemService files;
    files.set_root(project);
    editor::EditorPreferences preferences;
    std::string error;
    if (files.exists(editor::editor_preferences_path))
        editor::load_editor_preferences(files, preferences, error);
    if (backend.empty())
        backend = preferences.startupBackend;
    EngineConfig config;
    config.editor = true;
    config.enableRenderValidation = validation;
    config.targetFrameRate = 0;
    config.editorProjectRoot = project.generic_string();
    config.window.title = "ShinkouEngine Editor";
    config.window.maximized = true;
    config.renderBackend = backend == "dx12"     ? render::BackendApi::DirectX12
                           : backend == "vulkan" ? render::BackendApi::Vulkan
                                                 : render::BackendApi::DirectX11;
    Engine engine(config);
    if (!engine.initialize())
        return 1;
    seed_scene(engine);
    if (!engine.editor().layout().lastSceneFile.empty() &&
        files.exists(engine.editor().layout().lastSceneFile))
        engine.editor().execute_command(editor::EditorCommand::OpenScene,
                                        engine.editor().layout().lastSceneFile, engine.world());
    if (!theme.empty())
        engine.editor().set_theme(theme);
    const bool ran = engine.run(frames);
    const auto rendererError = engine.renderer().last_error();
    const auto &graph = engine.renderer().graph();
    std::ostringstream diagnostics;
    diagnostics << "backend=" << backend << " frames=" << engine.renderer().stats().frames << '\n';
    if (!rendererError.empty())
        diagnostics << "renderer-error=" << rendererError << '\n';
    if (!graph.diagnostics().lastError.empty())
        diagnostics << "graph-error=" << graph.diagnostics().lastError << '\n';
    for (const auto &pass : graph.pass_timings())
        diagnostics << "pass " << pass.name << " draws=" << pass.drawCalls
                    << " cpu-ns=" << pass.cpuNanoseconds << " gpu-ns=" << pass.gpuNanoseconds
                    << '\n';
    files.write_text_atomic("Saved/Editor/Logs/LastRun.txt", diagnostics.str());
    engine.shutdown();
    return ran && rendererError.empty() ? 0 : 3;
}
} // namespace
#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int count{};
    auto **wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide)
        return 1;
    std::vector<std::string> arguments;
    arguments.reserve(count);
    for (int i = 0; i < count; ++i) {
        const int bytes =
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string value(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, value.data(), bytes, nullptr, nullptr);
        if (!value.empty())
            value.pop_back();
        arguments.push_back(std::move(value));
    }
    LocalFree(wide);
    return run_editor(arguments);
}
#else
int main(int count, char **values) {
    return run_editor(std::vector<std::string>(values, values + count));
}
#endif

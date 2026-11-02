#include "shinkou/Engine.h"
#include "shinkou/World.h"
#include "shinkou/editor/EditorLayer.h"
#include "shinkou/input/InputSystem.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

using namespace shinkou;
using namespace shinkou::editor;
namespace {
std::vector<ObjectId> populate(World &world, int count) {
    world.reserve(count + 1);
    auto &root = world.create_object("Large hierarchy");
    std::vector<ObjectId> result;
    result.reserve(count);
    for (int i = 0; i < count; ++i)
        result.push_back(root.create_child("Object " + std::to_string(i)).id());
    return result;
}
class Input final : public input::IInputBackend {
  public:
    std::vector<input::InputEvent> queue, frame;
    input::MouseState mouse;
    std::vector<input::GamepadState> pads;
    bool initialize() override {
        return true;
    }
    void poll() override {
        frame = std::move(queue);
        queue.clear();
    }
    float control_value(std::string_view) const override {
        return 0;
    }
    const input::MouseState &mouse_state() const override {
        return mouse;
    }
    const std::vector<input::GamepadState> &gamepads() const override {
        return pads;
    }
    const std::vector<input::InputEvent> &events() const override {
        return frame;
    }
};
} // namespace
int main(int argc, char **argv) {
    // The native capture helper forwards its optional argument separator.
    if (argc > 1 && std::string_view(argv[1]) == "--") {
        ++argv;
        --argc;
    }
    if (argc > 3 && std::string_view(argv[1]) == "--native") {
        EngineConfig config;
        config.editor = true;
        config.window.title = "Shinkou UI performance";
        config.window.width = 1600;
        config.window.height = 1000;
        config.renderBackend = std::string_view(argv[2]) == "vulkan" ? render::BackendApi::Vulkan
                               : std::string_view(argv[2]) == "dx12"
                                   ? render::BackendApi::DirectX12
                                   : render::BackendApi::DirectX11;
        config.editorProjectRoot = argv[3];
        config.assets.projectRoot = argv[3];
        config.assets.cacheRoot = std::filesystem::path(argv[3]) / ".shinkou/cache";
        Engine engine(config);
        if (!engine.initialize())
            return 4;
        populate(engine.world(), argc > 4 ? std::max(100, std::atoi(argv[4])) : 20000);
        engine.editor().set_theme(argc > 5 ? argv[5] : "dark");
        const bool success = engine.run();
        engine.shutdown();
        return success ? 0 : 5;
    }
    const int count = argc > 1 ? std::max(100, std::atoi(argv[1])) : 20000;
    const auto project =
        std::filesystem::temp_directory_path() /
        ("shinkou-ui-bench-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(project / "assets");
    World world;
    const auto objects = populate(world, count);
    render::Renderer renderer;
    if (!renderer.initialize())
        return 1;
    EditorLayer editor;
    editor.set_project_root(project.generic_string());
    editor.set_layout_path((project / "layout.json").generic_string());
    editor.set_display_size(1600, 1000, 1);
    if (!editor.initialize(true))
        return 2;
    auto backend = std::make_unique<Input>();
    auto *source = backend.get();
    input::InputSystem input(std::move(backend));
    if (!input.initialize())
        return 3;
    FrameIndex frame = 0;
    const auto tick = [&] {
        input.poll();
        editor.process_input(input, world);
        const auto start = std::chrono::steady_clock::now();
        renderer.begin_graph();
        editor.prepare_frame(renderer, world);
        editor.draw(renderer, world, 1.f / 60, frame++);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    };
    const auto key = [&](const char *name, bool down) {
        input::InputEvent event;
        event.type = down ? input::InputEventType::KeyDown : input::InputEventType::KeyUp;
        event.control = name;
        source->queue.push_back(event);
        tick();
    };
    const auto click = [&](math::Vec2 point) {
        input::InputEvent event;
        event.type = input::InputEventType::MouseButtonDown;
        event.control = "mouse:left";
        event.position = point;
        source->queue.push_back(event);
        tick();
        event.type = input::InputEventType::MouseButtonUp;
        source->queue.push_back(event);
        tick();
    };
    const auto report = [&](const char *workload, float dpi, std::vector<double> values) {
        std::sort(values.begin(), values.end());
        const auto stats = editor.ui_performance();
        std::cout << world.object_count() << ',' << dpi << ',' << workload << ',' << values.size()
                  << ',' << values[(values.size() - 1) / 2] << ','
                  << values[static_cast<std::size_t>(std::ceil(values.size() * .95)) - 1] << ','
                  << values.back() << ',' << editor.ui_interaction_regions().size() << ','
                  << editor.ui_command_count() << ',' << stats.hierarchyRows << ','
                  << stats.hierarchySubmitted << ',' << stats.hierarchyRebuilds << ','
                  << stats.qaEnabled << '\n';
    };
    std::cout << "objects,dpi,workload,frames,p50_ms,p95_ms,max_ms,qa_regions,draw_commands,"
                 "hierarchy_rows,hierarchy_submitted,hierarchy_rebuilds,qa_enabled\n";
    for (float dpi : {1.f, 1.25f}) {
        editor.set_display_size(1600, 1000, dpi);
        for (int i = 0; i < 6; ++i)
            tick();
        std::vector<double> values;
        for (int i = 0; i < 30; ++i)
            values.push_back(tick());
        report("idle", dpi, values);
        const bool qa = editor.ui_qa_enabled();
        editor.set_ui_qa_enabled(true);
        tick();
        const auto filter = editor.ui_interaction_regions().at("hierarchy.filter");
        editor.set_ui_qa_enabled(qa);
        const math::Vec2 field{(filter.x + filter.width * .5f) * dpi,
                               (filter.y + filter.height * .5f) * dpi};
        values.clear();
        for (int i = 0; i < 30; ++i) {
            input::InputEvent event;
            event.type = input::InputEventType::MouseMove;
            event.position = {field.x, field.y + (50 + i % 5) * dpi};
            source->queue.push_back(event);
            values.push_back(tick());
        }
        report("hover", dpi, values);
        values.clear();
        for (int i = 0; i < 30; ++i) {
            input::InputEvent event;
            event.type = input::InputEventType::MouseWheel;
            event.delta = {0, i < 15 ? -1.f : 1.f};
            source->queue.push_back(event);
            values.push_back(tick());
        }
        report("scroll", dpi, values);
        values.clear();
        for (int i = 0; i < 30; ++i) {
            editor.select_object(objects[(i * 37) % objects.size()], world);
            values.push_back(tick());
        }
        report("selection", dpi, values);
        values.clear();
        click(field);
        for (char c : std::string("Object 199")) {
            input::InputEvent event;
            event.type = input::InputEventType::TextInput;
            event.text = std::string(1, c);
            source->queue.push_back(event);
            values.push_back(tick());
        }
        values.push_back(tick());
        report("typing", dpi, values);
        key("key:Left Ctrl", true);
        key("key:A", true);
        key("key:A", false);
        key("key:Left Ctrl", false);
        key("key:Backspace", true);
        key("key:Backspace", false);
        tick();
        values.clear();
        for (int i = 0; i < 30; ++i) {
            editor.set_display_size(i % 2 ? 1600 : 1580, i % 2 ? 1000 : 980, dpi);
            values.push_back(tick());
        }
        report("resize", dpi, values);
    }
    editor.shutdown(&renderer);
    std::filesystem::remove_all(project);
}

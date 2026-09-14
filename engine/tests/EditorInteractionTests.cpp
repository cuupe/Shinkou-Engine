#include "shinkou/editor/EditorLayer.h"
#include "shinkou/editor/EditorDocument.h"
#include "shinkou/World.h"
#include "shinkou/render/RenderScene.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace shinkou;
using namespace shinkou::editor;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
class TestInput final : public input::IInputBackend {
public:
    std::vector<input::InputEvent> queue, frame;
    input::MouseState mouse{};
    std::vector<input::GamepadState> pads;
    bool initialize() override { return true; }
    void poll() override { frame = std::move(queue); queue.clear(); }
    float control_value(std::string_view) const override { return 0; }
    const input::MouseState& mouse_state() const override { return mouse; }
    const std::vector<input::GamepadState>& gamepads() const override { return pads; }
    const std::vector<input::InputEvent>& events() const override { return frame; }
};

class ModelArtifactProcessor final : public assets::IAssetProcessor {
public:
    bool process(const assets::AssetProcessContext& context, assets::AssetArtifact& output,
                 std::string&) const override {
        if (std::filesystem::path(context.key.uri).extension() == ".gltf") {
            output.payload = context.sourceBytes;
            output.format = "model";
            return true;
        }
        static constexpr char imported[] =
            "o AssetSystemImported\n"
            "v -4 -2 0\n"
            "v 4 -2 0\n"
            "v 0 4 0\n"
            "f 1 2 3\n";
        output.payload.assign(imported, imported + sizeof(imported) - 1);
        output.format = "model";
        return true;
    }
};

class TextureArtifactProcessor final : public assets::IAssetProcessor {
public:
    bool process(const assets::AssetProcessContext&, assets::AssetArtifact& output,
                 std::string&) const override {
        static constexpr unsigned char png[] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
            0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x08, 0x04, 0x00, 0x00, 0x00, 0xb5, 0x1c, 0x0c, 0x02,
            0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54,
            0x78, 0x9c, 0x63, 0x64, 0x00, 0x02, 0x00, 0x00,
            0x05, 0x00, 0x01, 0xe9, 0x8d, 0x5d, 0x3c,
            0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
            0xae, 0x42, 0x60, 0x82};
        output.payload.assign(std::begin(png), std::end(png));
        output.format = "texture";
        return true;
    }
};
}

int main() {
    try {
        PropertyValue value;
        require(!parse_property_text(PropertyType::Vec3,"1 2 nan",value),"non-finite vector accepted");
        require(!parse_property_text(PropertyType::Number,"2junk",value),"trailing numeric garbage accepted");
        require(!parse_property_text(PropertyType::UnsignedInteger,"-1",value),"negative unsigned accepted");
        require(parse_property_text(PropertyType::Vec3,"1.5 -2 3",value),"valid vector rejected");
        World world;
        auto& root = world.create_object("Root");
        root.create_child("Child").add_component<components::LifetimeComponent>()->remaining = 9;
        root.add_component<components::LifetimeComponent>(); // -1 means never expire; must round-trip.
        auto* tags = root.add_component<components::TagComponent>(); tags->add("hero");
        EditorDocument document; std::string error, json;
        require(EditorDocument::capture(world,root.id(),document,error),"snapshot failed");
        require(document.to_json(json,error),"serialize failed");
        EditorDocument decoded;
        require(EditorDocument::from_json(json,decoded,error),"deserialize failed");
        ObjectId selected{};
        require(decoded.restore(world,selected,error),"restore failed");
        require(world.object_count()==2 && world.find_object(selected)->name()=="Root","hierarchy round trip failed");
        require(world.find_object(selected)->get_component<components::TagComponent>()->has("hero"),"tags lost");
        auto malformed = decoded; malformed.objects[1].parent=1;
        require(!malformed.restore(world,selected,error) && world.object_count()==2,"malformed scene damaged current world");
        malformed=decoded; malformed.objects[0].components[0].properties[0].value="nan 0 0";
        require(!malformed.restore(world,selected,error) && world.object_count()==2,"invalid setter damaged world");

        const auto project = std::filesystem::temp_directory_path()/ ("shinkou-ui-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(project/"assets/Folder/Nested");
        std::ofstream(project/"assets/Folder/Nested/needle.txt") << "fixture";
        std::ofstream(project/"assets/preview.wav", std::ios::binary) << "not a decoded fixture";
        std::ofstream(project/"assets/preview.avi", std::ios::binary) << "not a decoded video fixture";
        std::ofstream(project/"assets/preview.obj") <<
            "o Triangle\n"
            "v -1 -1 0\n"
            "v 1 -1 0\n"
            "v 0 1 0\n"
            "f 1 2 3\n";
        std::ofstream(project/"assets/preview.gltf") << R"json({
          "asset":{"version":"2.0"},
          "buffers":[{"byteLength":42,"uri":"data:application/octet-stream;base64,AACAvwAAgL8AAAAAAACAPwAAgL8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAAQABAAIA"}],
          "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
          "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],
          "images":[
            {"name":"AlbedoA","uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNg+M/AAAADAQEAyf6S7wAAAABJRU5ErkJggg==","mimeType":"image/png"},
            {"name":"AlbedoB","uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNg+M/AAAADAQEAyf6S7wAAAABJRU5ErkJggg==","mimeType":"image/png"}
          ],
          "samplers":[{}],
          "textures":[{"name":"TextureA","source":0,"sampler":0},{"name":"TextureB","source":1,"sampler":0}],
          "materials":[
            {"name":"MaterialA","pbrMetallicRoughness":{"baseColorTexture":{"index":0}}},
            {"name":"MaterialB","pbrMetallicRoughness":{"baseColorTexture":{"index":1}}}
          ],
          "meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}]
        })json";
        std::ofstream(project/"assets/Folder-extra.txt") << "sibling must follow the complete subtree";
        std::ofstream(project/"assets/preview.png", std::ios::binary) << "AssetSystem texture source";
        std::ofstream(project/"CMakeLists.txt") << "cmake_minimum_required(VERSION 3.20)\n";
        std::ofstream(project/"Sample.sln") << "Microsoft Visual Studio Solution File\n";
        std::ofstream(project/"compile_commands.json") << R"json([{"directory":".","file":"assets/Folder/Nested/needle.txt","arguments":["clang++","-c","assets/Folder/Nested/needle.txt"]}])json";
        for (int i=0;i<100;++i) std::ofstream(project/("assets/item"+std::to_string(i)+".txt")) << i;
        assets::AssetSystemConfig assetConfig;
        assetConfig.projectRoot = project;
        assetConfig.cacheRoot = project / ".shinkou" / "cache";
        assetConfig.workerCount = 2;
        assetConfig.memoryBudgetBytes = 16u * 1024u * 1024u;
        assets::AssetSystem resourceSystem(assetConfig);
        resourceSystem.register_processor("model", std::make_unique<ModelArtifactProcessor>());
        resourceSystem.register_processor("texture", std::make_unique<TextureArtifactProcessor>());
        require(resourceSystem.initialize(), "asset system initialization failed");
        require(resourceSystem.write_manifest(project / ".shinkou" / "manifest.json"),
                "asset system could not write the editor manifest fixture");
        assets::AssetId previewModelAssetId = 0;
        for (const auto& entry : resourceSystem.scan_sources()) {
            if (entry.key.type == "model" && entry.sourcePath.filename() == "preview.obj") {
                previewModelAssetId = entry.id;
                break;
            }
        }
        require(previewModelAssetId != 0, "asset system manifest did not index the model fixture");
        EditorBuildProfile alternateProfile;
        alternateProfile.id = "release";
        alternateProfile.name = "CMake Release";
        alternateProfile.configuration = "Release";
        EditorBuildProfileSet profileSet;
        profileSet.profiles = {EditorBuildProfile{}, alternateProfile};
        profileSet.selectedId = "default";
        std::string profileSetJson;
        std::string profileSetError;
        require(EditorBuildProfileStore::serialize_set(profileSet, profileSetJson, &profileSetError),
                "could not create multi-profile fixture");
        std::ofstream(project / ".shinkou" / "build-profile.json") << profileSetJson;
        EditorLayer editor; editor.set_project_root(project.generic_string());
        editor.set_layout_path((project/"Saved/layout.json").generic_string());
        require(editor.initialize(false),"editor initialization failed");
        editor.set_asset_system(&resourceSystem);
        editor.set_display_size(1280,720,1);
        render::Renderer renderer;
        auto backend = std::make_unique<TestInput>(); auto* fake=backend.get();
        input::InputSystem input(std::move(backend)); input.initialize();
        FrameIndex frame=0;
        auto tick = [&] { input.poll(); editor.process_input(input,world); editor.prepare_frame(renderer,world); editor.draw(renderer,world,1.0f/60,frame++); };
        auto region = [&](const std::string& id) {
            const auto& regions=editor.editor_ui().interaction_regions();
            auto it=regions.find(id);
            if (it==regions.end() || it->second.width<=0 || it->second.height<=0) throw std::runtime_error("missing region: "+id);
            return it->second;
        };
        auto click = [&](const std::string& id) {
            const auto r=region(id); fake->mouse.position={r.x+r.width*0.5f,r.y+r.height*0.5f};
            input::InputEvent e; e.type=input::InputEventType::MouseButtonDown; e.control="mouse:left"; e.position=fake->mouse.position;
            fake->queue.push_back(e); e.type=input::InputEventType::MouseButtonUp; fake->queue.push_back(e); tick();
        };
        auto key = [&](const char* name) { input::InputEvent e; e.type=input::InputEventType::KeyDown;e.control=std::string("key:")+name;fake->queue.push_back(e);tick(); };
        auto text = [&](const char* text) { input::InputEvent e;e.type=input::InputEventType::TextInput;e.text=text;fake->queue.push_back(e);tick(); };
        for(int i=0;i<200 && editor.ui_asset_file_count()<100;++i) {tick();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        require(editor.ui_asset_file_count()>100,"async file scan did not finish");
        const auto camera = world.ecs().create();
        world.ecs().emplace<render::CameraComponent>(camera);
        render::TransformComponent cameraTransform; cameraTransform.local.position={0,0,-5};
        world.ecs().emplace<render::TransformComponent>(camera,cameraTransform);
        tick();
        const auto viewport = region("viewport.surface");
        input::InputEvent gesture; gesture.type=input::InputEventType::MouseWheel;
        gesture.position={viewport.x+viewport.width/2,viewport.y+viewport.height/2}; gesture.delta.y=1;
        fake->queue.push_back(gesture);tick();
        auto* cameraPose=world.ecs().try_get<render::TransformComponent>(camera);
        require(cameraPose->local.position.z > -5,"viewport wheel did not move scene camera");
        gesture.type=input::InputEventType::MouseButtonDown;gesture.control="mouse:middle";fake->queue.push_back(gesture);tick();
        gesture.type=input::InputEventType::MouseMove;gesture.position.x+=40;gesture.position.y+=20;fake->queue.push_back(gesture);tick();
        gesture.type=input::InputEventType::MouseButtonUp;fake->queue.push_back(gesture);tick();
        require(std::abs(cameraPose->local.rotation.y)>0.01f,"viewport orbit did not rotate camera");
        editor.execute_command(EditorCommand::ProjectSettings,{},world);tick();
        click("command:theme-light"); require(editor.layout().theme=="light","settings theme command failed");
        const auto settings=region("tools:settings");
        gesture.type=input::InputEventType::MouseWheel;gesture.position={settings.x+10,settings.y+10};gesture.delta.y=-20;
        fake->queue.push_back(gesture);tick();
        click("command:save-layout"); require(std::filesystem::exists(editor.layout().layoutFile),"settings could not save layout");
        editor.set_panel_visible("settings",false);editor.set_theme("dark");tick();
        editor.execute_command(EditorCommand::TogglePage, "build", world); tick();
        require(editor.layout().showBuild && editor.panel_visible("build"), "build panel visibility was not persisted in the editor state");
        require(region("command:build-panel-build").width > 0 && region("command:build-panel-refresh").width > 0,
                "retained build panel controls were not registered");
        require(region("command:build-panel-open-vscode").width > 0 &&
                region("command:build-panel-save-profile").width > 0,
                "retained IDE/profile controls were not registered");
        require(region("command:build-panel-profile:default").width > 0 &&
                region("command:build-panel-profile:release").width > 0 &&
                region("command:build-diagnostic-filter:all").width > 0 &&
                region("command:build-diagnostic-filter:errors").width > 0 &&
                region("command:build-panel-refresh-project-files").width > 0 &&
                region("command:build-panel-associate-project-file").width > 0 &&
                region("command:build-panel-generate-clangd").width > 0 &&
                region("field:build-profile.name").width > 0 &&
                region("field:build-profile.buildDirectory").width > 0,
                "retained build profile editor controls were not registered");
        click("command:build-diagnostic-filter:errors");
        require(editor.diagnostic_filter() == "errors", "diagnostic filter command did not update editor state");
        click("command:build-diagnostic-filter:all");
        click("command:build-panel-refresh-project-files");
        require(editor.project_integration_status().find("Discovered") != std::string::npos &&
                editor.project_discovery().recommendedProjectFile == std::filesystem::path("Sample.sln"),
                "project discovery command did not expose the recommended solution");
        click("command:build-panel-associate-project-file");
        require(editor.build_profile().projectFile == std::filesystem::path("Sample.sln") &&
                editor.project_integration_status().find("Associated") != std::string::npos,
                "project association command did not update the selected profile");
        click("command:build-panel-generate-clangd");
        require(std::filesystem::exists(project / ".clangd") &&
                editor.clangd_config_status().find("Generated") != std::string::npos,
                "clangd config command did not write a project-root config");
        click("command:build-panel-profile:release");
        require(editor.selected_build_profile_id() == "release" && editor.build_profile().name == "CMake Release",
                "retained build profile selector did not switch the active profile");
        click("command:build-panel-profile:default");
        click("field:build-profile.name"); text("QA Profile"); key("Return");
        require(editor.build_profile().name == "QA Profile" &&
                editor.build_profile_status().find("Unsaved") != std::string::npos,
                "build profile field edit did not commit through the retained input path");
        editor.execute_command(EditorCommand::SaveBuildProfile, {}, world); tick();
        require(std::filesystem::exists(project / ".shinkou" / "build-profile.json"),
                "build profile command did not persist project configuration");
        require(editor.build_profile_status().find("Saved") != std::string::npos,
                "build profile save status was not published");
        editor.execute_command(EditorCommand::ReloadBuildProfile, {}, world); tick();
        require(editor.build_profile_status().find("Loaded") != std::string::npos,
                "build profile reload command did not publish status");
        require(editor.build_profile().name == "QA Profile",
                "build profile field edit was not persisted by the profile set format");
        editor.execute_command(EditorCommand::ImportCompileCommands, {}, world); tick();
        require(editor.compile_command_count() == 1 && editor.compile_commands_status().find("Imported 1") != std::string::npos,
                "compile_commands import did not update the build model");
        editor.execute_command(EditorCommand::ExportCompileCommands, "Saved/exported-compile_commands.json", world);
        require(std::filesystem::exists(project/"Saved/exported-compile_commands.json"),
                "compile_commands export did not write through the project filesystem");
        editor.execute_command(EditorCommand::TogglePage, "build", world); tick();
        require(!editor.layout().showBuild && !editor.panel_visible("build"), "build panel toggle command failed");
        tick();click("asset-view:large"); require(editor.asset_view()==EditorAssetView::LargeIcons,"large view click failed");
        click("asset-view:tree");
        require(region("asset:assets/Folder/Nested/needle.txt").y < region("asset:assets/Folder-extra.txt").y,"tree sibling interrupted a directory subtree");
        click("asset-toggle:assets/Folder");
        click("assets.filter"); text("needle");
        require(editor.ui_visible_asset_file_count()==3,"search did not include collapsed ancestors");
        key("Return"); key("End"); tick();
        require(editor.editor_ui().selected_asset()=="assets/Folder/Nested/needle.txt","End navigation failed");
        for (int i = 0; i < 100 && editor.asset_preview().loading; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_preview().path == "assets/Folder/Nested/needle.txt" &&
                editor.asset_preview().kind == "Text" && !editor.asset_preview().textLines.empty() &&
                editor.asset_preview().textLines.front().find("fixture") != std::string::npos,
                "selected text resource did not produce an async preview");
        for (int i = 0; i < 100 && !editor.asset_preview().assetSystemReady; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_preview().assetSystemReady &&
                editor.asset_preview().assetSystemStatus == "AssetSystem ready" &&
                editor.asset_preview().assetSystemFormat == "text" &&
                editor.asset_preview().assetSystemMetadataFormat == "shinkou.asset.text.v1" &&
                editor.asset_preview().assetSystemMetadataBytes > 0 &&
                editor.asset_preview().assetSystemSourceHash != 0,
                "selected resource did not publish the AssetSystem cache contract");
        for (int i = 0; i < 160 && editor.asset_system_manifest_count() == 0; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (editor.asset_system_manifest_count() == 0 ||
            editor.asset_system_manifest_status().find("AssetSystem manifest ready") == std::string::npos) {
            throw std::runtime_error("editor did not publish the AssetSystem manifest snapshot: count=" +
                std::to_string(editor.asset_system_manifest_count()) + " status=" +
                editor.asset_system_manifest_status());
        }
        require(editor.asset_system_manifest_status().find("validated cache") != std::string::npos,
                "editor did not validate and seed the persisted AssetSystem manifest");
        for (int i = 0; i < 8; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_system_manifest_count() > 0 &&
                editor.asset_system_manifest_status().find("AssetSystem manifest ready") != std::string::npos,
                "ready AssetSystem manifest was restarted while the file tree was still settling");
        bool bridgeStatusVisible = false;
        for (const auto& command : editor.editor_ui().render_list().commands()) {
            if (command.type == ui::DrawCommandType::Text && command.text.find("AssetSystem ready") != std::string::npos) {
                bridgeStatusVisible = true;
                break;
            }
        }
        require(bridgeStatusVisible, "Inspector did not retain-paint the AssetSystem status");
        editor.execute_command(EditorCommand::OpenAsset, "assets/Folder/Nested/needle.txt", world); tick();
        require(editor.layout().showInspector && editor.asset_preview().path == "assets/Folder/Nested/needle.txt",
                "open asset command did not route to the inspector preview");
        editor.execute_command(EditorCommand::OpenAsset, "../outside.txt", world);
        require(editor.last_status().find("outside") != std::string::npos,
                "open asset command accepted a path outside the project root");
        key("F2"); text("renamed.txt"); key("Return");
        require(std::filesystem::exists(project/"assets/Folder/Nested/renamed.txt"),"real rename failed");
        click("assets.filter"); key("Left Ctrl"); key("A");
        {input::InputEvent e;e.type=input::InputEventType::KeyUp;e.control="key:Left Ctrl";fake->queue.push_back(e);tick();}
        key("Backspace"); key("Return"); key("End");tick();
        require(editor.editor_ui().asset_scroll_offset()>0,"keyboard failed to reveal last item");
        const auto rail=region("assets.scrollbar");
        input::InputEvent e;e.type=input::InputEventType::MouseButtonDown;e.control="mouse:left";e.position={rail.x+6,rail.y+rail.height-3};fake->queue.push_back(e);tick();
        e.type=input::InputEventType::MouseMove;e.position={rail.x+6,rail.y};fake->queue.push_back(e);tick();
        e.type=input::InputEventType::MouseButtonUp;fake->queue.push_back(e);tick();
        require(editor.editor_ui().asset_scroll_offset()<30,"scrollbar drag failed");

        editor.execute_command(EditorCommand::CreateEmpty,{},world); tick();
        const auto created=editor.layout().selectedObject;
        click("field:name");text("Edited Object");key("Return");
        require(world.find_object(created)->name()=="Edited Object","inspector did not commit name");
        std::string positionField;
        for (const auto& r:editor.editor_ui().interaction_regions()) if(r.first.find(":position")!=std::string::npos) positionField=r.first;
        require(!positionField.empty(),"missing transform input");click(positionField);text("3 4 5");key("Return");
        require(world.find_object(created)->get_component<components::TransformComponent>()->local.position.x==3,"transform edit failed");
        click(positionField);text("nan 4 5");key("Return");
        require(world.find_object(created)->get_component<components::TransformComponent>()->local.position.x==3,"invalid transform modified world");key("Escape");
        editor.execute_command(EditorCommand::Undo,{},world);tick();
        require(world.find_object(editor.layout().selectedObject)->get_component<components::TransformComponent>()->local.position.x==0,"undo failed");
        editor.execute_command(EditorCommand::Redo,{},world);tick();
        require(world.find_object(editor.layout().selectedObject)->get_component<components::TransformComponent>()->local.position.x==3,"redo failed");
        editor.execute_command(EditorCommand::SaveScene,"assets/Scenes/test.scene",world);
        require(std::filesystem::exists(project/"assets/Scenes/test.scene"),"save scene failed");
        editor.execute_command(EditorCommand::NewScene,{},world);require(world.object_count()==0,"new scene failed");
        editor.execute_command(EditorCommand::OpenScene,"assets/Scenes/test.scene",world);require(world.object_count()==3,"open scene failed");
        editor.set_panel_visible("build", false);
        editor.set_panel_visible("viewport", true);
        editor.dock_workspace().activate_tab("viewport");
        for (int i = 0; i < 160 && editor.asset_system_manifest_count() == 0; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_system_manifest_count() > 0 &&
                editor.asset_system_manifest_status().find("AssetSystem manifest ready") != std::string::npos,
                "asset manifest did not settle before the viewport drop interaction");
        tick();
        click("assets.filter");
        key("Left Ctrl");
        key("A");
        {input::InputEvent e;e.type=input::InputEventType::KeyUp;e.control="key:Left Ctrl";fake->queue.push_back(e);tick();}
        key("Backspace");
        text("preview.obj");
        tick();
        const auto objectCountBeforeDrop = world.object_count();
        const auto assetSource = region("asset:assets/preview.obj");
        const auto viewportDropTarget = region("viewport.surface");
        gesture.type = input::InputEventType::MouseButtonDown;
        gesture.control = "mouse:left";
        gesture.position = {assetSource.x + assetSource.width * 0.5f,
                             assetSource.y + assetSource.height * 0.5f};
        fake->queue.push_back(gesture);
        tick();
        gesture.type = input::InputEventType::MouseMove;
        gesture.position = {viewportDropTarget.x + viewportDropTarget.width * 0.75f,
                             viewportDropTarget.y + viewportDropTarget.height * 0.25f};
        fake->queue.push_back(gesture);
        tick();
        bool dropCueVisible = false;
        for (const auto& command : editor.editor_ui().render_list().commands()) {
            if (command.type == ui::DrawCommandType::Text && command.text.find("Drop preview.obj") != std::string::npos) {
                dropCueVisible = true;
                break;
            }
        }
        require(dropCueVisible, "asset drag did not retain a viewport drop cue");
        gesture.type = input::InputEventType::MouseButtonUp;
        fake->queue.push_back(gesture);
        tick();
        require(world.object_count() == objectCountBeforeDrop + 1,
                "asset drag did not create a scene object");
        GameObject* droppedAssetObject = nullptr;
        world.each_object([&](GameObject& object) {
            if (object.get_component<AssetReferenceComponent>() != nullptr) droppedAssetObject = &object;
        });
        require(droppedAssetObject != nullptr &&
                droppedAssetObject->get_component<AssetReferenceComponent>()->path() == "assets/preview.obj" &&
                droppedAssetObject->get_component<AssetReferenceComponent>()->asset_id() == previewModelAssetId &&
                droppedAssetObject->get_component<components::TransformComponent>()->local.position.x > 1.0f &&
                droppedAssetObject->get_component<components::TransformComponent>()->local.position.y > 1.0f &&
                editor.last_status().find("Created asset reference") != std::string::npos,
                "asset drop did not publish a manifest identity, bounded reference and viewport position");
        EditorDocument droppedDocument;
        std::string droppedDocumentError;
        std::string droppedDocumentJson;
        require(EditorDocument::capture(world, editor.layout().selectedObject, droppedDocument, droppedDocumentError) &&
                droppedDocument.to_json(droppedDocumentJson, droppedDocumentError) &&
                droppedDocumentJson.find("assetId") != std::string::npos,
                "asset reference manifest identity was not serialized");
        editor.execute_command(EditorCommand::Undo, {}, world);
        require(world.object_count() == objectCountBeforeDrop, "asset drop was not undoable");
        editor.execute_command(EditorCommand::Redo, {}, world);
        require(world.object_count() == objectCountBeforeDrop + 1,
                "asset drop was not redoable");
        bool restoredDroppedAsset = false;
        world.each_object([&](GameObject& object) {
            const auto* reference = object.get_component<AssetReferenceComponent>();
            restoredDroppedAsset = restoredDroppedAsset ||
                (reference != nullptr && reference->path() == "assets/preview.obj");
        });
        require(restoredDroppedAsset, "asset reference was not restored by redo");
        bool restoredAssetId = false;
        world.each_object([&](GameObject& object) {
            const auto* reference = object.get_component<AssetReferenceComponent>();
            restoredAssetId = restoredAssetId ||
                (reference != nullptr && reference->path() == "assets/preview.obj" &&
                 reference->asset_id() == previewModelAssetId);
        });
        require(restoredAssetId, "asset reference manifest identity was not restored by redo");
        for (int i = 0; i < 160 && editor.model_scene_loaded_asset_count() == 0; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.model_scene_loaded_asset_count() == 1,
                "AssetId-backed model reference did not enter the asynchronous scene-instance cache");
        require(editor.model_scene_instance_count() == 0 &&
                editor.model_scene_status().find("GPU model scene fallback") != std::string::npos,
                "Null renderer did not expose the model scene backend fallback honestly");
        click("assets.filter");
        key("Left Ctrl");
        key("A");
        {input::InputEvent e;e.type=input::InputEventType::KeyUp;e.control="key:Left Ctrl";fake->queue.push_back(e);tick();}
        key("Backspace");
        key("Return");
        tick();
        const auto objectCountBeforeRejectedDrop = world.object_count();
        const auto unsupportedSource = region("asset:assets/Folder-extra.txt");
        gesture.type = input::InputEventType::MouseButtonDown;
        gesture.control = "mouse:left";
        gesture.position = {unsupportedSource.x + unsupportedSource.width * 0.5f,
                             unsupportedSource.y + unsupportedSource.height * 0.5f};
        fake->queue.push_back(gesture);
        tick();
        gesture.type = input::InputEventType::MouseMove;
        gesture.position = {viewportDropTarget.x + viewportDropTarget.width * 0.25f,
                             viewportDropTarget.y + viewportDropTarget.height * 0.25f};
        fake->queue.push_back(gesture);
        tick();
        gesture.type = input::InputEventType::MouseButtonUp;
        fake->queue.push_back(gesture);
        tick();
        require(world.object_count() == objectCountBeforeRejectedDrop &&
                editor.last_status().find("not instantiable") != std::string::npos,
                "unsupported asset drop changed the scene");
        const auto objectCountBeforeBoundaryDrops = world.object_count();
        require(!editor.create_asset_reference_at_viewport(
                    world, "assets/Folder", {viewportDropTarget.x + 10.0f, viewportDropTarget.y + 10.0f}) &&
                editor.last_status().find("folders cannot be instantiated") != std::string::npos,
                "directory asset drop was not rejected at the editor boundary");
        require(!editor.create_asset_reference_at_viewport(
                    world, "../outside.txt", {viewportDropTarget.x + 10.0f, viewportDropTarget.y + 10.0f}) &&
                editor.last_status().find("outside the project") != std::string::npos,
                "out-of-root asset drop was not rejected at the editor boundary");
        require(!editor.create_asset_reference_at_viewport(
                    world, "assets/preview.obj", {viewportDropTarget.x - 1.0f, viewportDropTarget.y}) &&
                editor.last_status().find("position is invalid") != std::string::npos &&
                world.object_count() == objectCountBeforeBoundaryDrops,
                "invalid viewport coordinate changed the scene");
        const auto objectCountBeforeNativeDrop = world.object_count();
        require(editor.create_asset_reference_from_window_drop(
                    world, (project / "assets/preview.obj").generic_string(),
                    {viewportDropTarget.x + 20.0f, viewportDropTarget.y + 20.0f}) &&
                world.object_count() == objectCountBeforeNativeDrop + 1 &&
                editor.last_status().find("Created asset reference") != std::string::npos,
                "native absolute file drop did not route through the validated editor ingress");
        const auto objectCountAfterNativeDrop = world.object_count();
        require(!editor.create_asset_reference_from_window_drop(
                    world, (project.parent_path() / "outside-native.obj").generic_string(),
                    {viewportDropTarget.x + 20.0f, viewportDropTarget.y + 20.0f}) &&
                world.object_count() == objectCountAfterNativeDrop &&
                editor.last_status().find("native path") != std::string::npos,
                "native file drop boundary accepted an outside-project path");
        editor.execute_command(EditorCommand::Undo, {}, world);
        require(world.object_count() == objectCountBeforeNativeDrop,
                "native file drop did not remain undoable through the shared editor transaction");
        tick();
        require(!editor.consume_simulation_step(),"edit mode advances simulation");
        key("Left Ctrl");
        { input::InputEvent lost; lost.type=input::InputEventType::FocusLost; fake->queue.push_back(lost); tick(); }
        click("field:name"); text("After focus loss"); key("Return");
        require(world.find_object(editor.layout().selectedObject)->name()=="After focus loss","focus loss left Ctrl latched");
        editor.execute_command(EditorCommand::Play,{},world);require(editor.consume_simulation_step(),"play failed");
        editor.execute_command(EditorCommand::Pause,{},world);require(!editor.consume_simulation_step(),"pause failed");
        editor.execute_command(EditorCommand::Step,{},world);require(editor.consume_simulation_step()&&!editor.consume_simulation_step(),"step did not advance exactly one frame");
        editor.execute_command(EditorCommand::OpenAsset, "assets/preview.wav", world);
        for (int i = 0; i < 100 && editor.media_preview().kind != "Audio"; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.media_preview().kind == "Audio" && editor.media_preview().path == "assets/preview.wav" &&
                !editor.media_preview().available && editor.media_preview().status.find("not connected") != std::string::npos,
                "audio asset did not expose an honest unavailable preview state");
        editor.set_panel_visible("media", false);
        editor.execute_command(EditorCommand::TogglePage, "media", world); tick();
        require(region("command:media-play").width > 0 && region("command:media-stop").width > 0,
                "audio media transport controls were not registered");
        editor.execute_command(EditorCommand::OpenAsset, "assets/preview.png", world);
        for (int i = 0; i < 120 && editor.asset_preview().loading; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_preview().kind == "Image", "image asset did not enter the image preview state");
        if (editor.asset_preview().imageSnapshot) {
            require(editor.asset_preview().imageSnapshot->valid(),
                    "image asset produced an invalid decoded thumbnail snapshot");
        bool imageCommand = false;
        for (const auto& command : editor.editor_ui().render_list().commands()) {
            if (command.type == ui::DrawCommandType::Image && command.imageSnapshot) {
                imageCommand = true;
                break;
            }
        }
        require(imageCommand, "retained inspector did not submit the image snapshot command");
        } else {
            require(editor.asset_preview().status.find("unavailable") != std::string::npos,
                    "image provider failure was not exposed honestly");
        }
        editor.execute_command(EditorCommand::OpenAsset, "assets/preview.avi", world);
        for (int i = 0; i < 120 && (editor.media_preview().kind != "Video" || editor.media_preview().previewLoading); ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.media_preview().kind == "Video" && editor.media_preview().path == "assets/preview.avi",
                "video asset did not enter the media preview state");
        require(!editor.media_preview().videoPreview &&
                editor.media_preview().previewStatus.find("unavailable") != std::string::npos,
                "video provider failure was not exposed honestly");
        editor.execute_command(EditorCommand::OpenAsset, "assets/preview.obj", world);
        for (int i = 0; i < 120 && editor.asset_preview().loading; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_preview().kind == "Mesh" && editor.asset_preview().modelPreview &&
                editor.asset_preview().modelPreview->valid(),
                "model asset did not produce a bounded wireframe preview");
        require(editor.asset_preview().modelPreview->minX == -4.0f &&
                editor.asset_preview().modelPreview->maxY == 4.0f,
                "model preview did not consume the AssetSystem structured source artifact");
        require(editor.asset_preview().modelPreviewScene && editor.asset_preview().modelPreviewScene->valid(),
                "model asset did not publish an isolated preview scene state");
        require(region("asset-model-material-prev").width > 0.0f &&
                region("asset-model-material-next").width > 0.0f &&
                region("asset-model-texture-prev").width > 0.0f &&
                region("asset-model-texture-next").width > 0.0f,
                "model material/texture selector regions were not retained");
        click("asset-model-material-next");
        click("asset-model-texture-next");
        require(editor.asset_preview().modelMaterialIndex == -1 &&
                editor.asset_preview().modelTextureIndex == -1,
                "empty model material/texture selectors changed state");
        const auto worldObjectsBeforeModelOrbit = world.object_count();
        const auto modelProjectionBeforeOrbit = editor.asset_preview().modelPreviewScene->projectionRevision;
        const auto modelSurface = region("asset-model-preview");
        gesture.type = input::InputEventType::MouseButtonDown;
        gesture.control = "mouse:middle";
        gesture.position = {modelSurface.x + modelSurface.width * 0.5f, modelSurface.y + modelSurface.height * 0.5f};
        fake->queue.push_back(gesture); tick();
        gesture.type = input::InputEventType::MouseMove;
        gesture.position.x += 28.0f;
        gesture.position.y -= 12.0f;
        fake->queue.push_back(gesture); tick();
        gesture.type = input::InputEventType::MouseButtonUp;
        fake->queue.push_back(gesture); tick();
        require(editor.asset_preview().modelPreviewScene->projectionRevision != modelProjectionBeforeOrbit,
                "model preview orbit did not update the isolated camera");
        require(world.object_count() == worldObjectsBeforeModelOrbit,
                "model preview orbit mutated the active world");
        click("asset-model-preview-reset");
        require(editor.asset_preview().modelPreviewScene->camera.yaw == 0.55f,
                "model preview reset did not restore the camera");
        editor.execute_command(EditorCommand::OpenAsset, "assets/preview.gltf", world);
        for (int i = 0; i < 240 &&
             (editor.asset_preview().loading || editor.asset_preview().modelTextureStatus.empty() ||
              editor.asset_preview().modelTextureStatus.find("Loading") != std::string::npos); ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_preview().modelPreview && editor.asset_preview().modelPreview->valid() &&
                editor.asset_preview().modelMaterialIndex == 0 &&
                editor.asset_preview().modelTextureIndex == 0 &&
                editor.asset_preview().modelTextureImageIndex == 0 &&
                editor.asset_preview().modelMaterialLabel.find("MaterialA") != std::string::npos &&
                editor.asset_preview().modelTextureLabel.find("TextureA") != std::string::npos,
                "GLTF material-to-texture selection was not published");
        require(editor.asset_preview().modelTextureSnapshot &&
                editor.asset_preview().modelTextureSnapshot->valid(),
                "GLTF image artifact was not decoded by the WIC preview seam");
        click("asset-model-material-next");
        for (int i = 0; i < 120 && editor.asset_preview().modelTextureStatus.find("Loading") != std::string::npos; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_preview().modelMaterialIndex == 1 &&
                editor.asset_preview().modelTextureIndex == 1 &&
                editor.asset_preview().modelTextureImageIndex == 1 &&
                editor.asset_preview().modelMaterialLabel.find("MaterialB") != std::string::npos &&
                editor.asset_preview().modelTextureLabel.find("TextureB") != std::string::npos,
                "GLTF material selection did not follow its base-color texture reference");
        editor.shutdown();
        resourceSystem.shutdown();
        std::filesystem::remove_all(project);
        std::cout << "Editor document, real filesystem, input routing, inspector, undo and simulation passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

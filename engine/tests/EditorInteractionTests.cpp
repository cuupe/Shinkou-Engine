#include "shinkou/editor/EditorLayer.h"
#include "shinkou/editor/EditorDocument.h"
#include "shinkou/World.h"
#include "shinkou/render/RenderScene.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

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
        std::ofstream(project/"assets/Folder-extra.txt") << "sibling must follow the complete subtree";
        for (int i=0;i<100;++i) std::ofstream(project/("assets/item"+std::to_string(i)+".txt")) << i;
        EditorLayer editor; editor.set_project_root(project.generic_string());
        editor.set_layout_path((project/"Saved/layout.json").generic_string());
        require(editor.initialize(false),"editor initialization failed");
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
        tick();click("asset-view:large"); require(editor.asset_view()==EditorAssetView::LargeIcons,"large view click failed");
        click("asset-view:tree");
        require(region("asset:assets/Folder/Nested/needle.txt").y < region("asset:assets/Folder-extra.txt").y,"tree sibling interrupted a directory subtree");
        click("asset-toggle:assets/Folder");
        click("assets.filter"); text("needle");
        require(editor.ui_visible_asset_file_count()==3,"search did not include collapsed ancestors");
        key("Return"); key("End"); tick();
        require(editor.editor_ui().selected_asset()=="assets/Folder/Nested/needle.txt","End navigation failed");
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
        require(!editor.consume_simulation_step(),"edit mode advances simulation");
        key("Left Ctrl");
        { input::InputEvent lost; lost.type=input::InputEventType::FocusLost; fake->queue.push_back(lost); tick(); }
        click("field:name"); text("After focus loss"); key("Return");
        require(world.find_object(editor.layout().selectedObject)->name()=="After focus loss","focus loss left Ctrl latched");
        editor.execute_command(EditorCommand::Play,{},world);require(editor.consume_simulation_step(),"play failed");
        editor.execute_command(EditorCommand::Pause,{},world);require(!editor.consume_simulation_step(),"pause failed");
        editor.execute_command(EditorCommand::Step,{},world);require(editor.consume_simulation_step()&&!editor.consume_simulation_step(),"step did not advance exactly one frame");
        editor.shutdown();
        std::filesystem::remove_all(project);
        std::cout << "Editor document, real filesystem, input routing, inspector, undo and simulation passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

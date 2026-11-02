#include "shinkou/editor/EditorLayer.h"
#include "shinkou/World.h"
#include "shinkou/input/InputSystem.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace shinkou;
using namespace shinkou::editor;
namespace {
void require(bool b,const char* text){if(!b)throw std::runtime_error(text);}
class Input final:public input::IInputBackend {
public:
    std::vector<input::InputEvent> queue,events_;input::MouseState mouse;std::vector<input::GamepadState> pads;
    bool initialize()override{return true;}
    void poll()override{events_=std::move(queue);queue.clear();}
    float control_value(std::string_view)const override{return 0;}
    const input::MouseState& mouse_state()const override{return mouse;}
    const std::vector<input::GamepadState>& gamepads()const override{return pads;}
    const std::vector<input::InputEvent>& events()const override{return events_;}
};
}
int main(){try{
    const auto project=std::filesystem::temp_directory_path()/("shinkou-imgui-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    World world;const auto root=world.create_object("Selected object").id();
    render::Renderer renderer;require(renderer.initialize(),"Null renderer failed");
    EditorLayer editor;editor.set_project_root(project.generic_string());editor.set_layout_path((project/"Saved/layout.json").generic_string());
    editor.set_ui_qa_enabled(true);
    editor.set_display_size(1600,1000,1);require(editor.initialize(true),"ImGui presenter failed");editor.select_object(root,world);
    auto backend=std::make_unique<Input>();auto* source=backend.get();input::InputSystem input(std::move(backend));require(input.initialize(),"input failed");
    FrameIndex frame=0;auto tick=[&]{input.poll();editor.process_input(input,world);editor.prepare_frame(renderer,world);editor.draw(renderer,world,1.f/60,frame++);};
    auto click=[&](const std::string& id){
        const auto& regions=editor.ui_interaction_regions();auto found=regions.find(id);require(found!=regions.end(),("missing region "+id).c_str());
        const auto r=found->second;input::InputEvent event;event.type=input::InputEventType::MouseButtonDown;event.control="mouse:left";event.position={r.x+r.width/2,r.y+r.height/2};
        source->queue.push_back(event);tick();event.type=input::InputEventType::MouseButtonUp;source->queue.push_back(event);tick();tick();
    };
    for(int i=0;i<4;++i)tick();
    require(editor.ui_command_count()>0,"actual ImGui draw data was not generated");
    const auto x=editor.ui_interaction_regions().at("transform:position:X");const auto y=editor.ui_interaction_regions().at("transform:position:Y");const auto z=editor.ui_interaction_regions().at("transform:position:Z");
    require(x.x+x.width<=y.x && y.x+y.width<=z.x,"XYZ fields overlap");
    click("toolbar.play");require(editor.consume_simulation_step(),"play button did not dispatch the controller command");
    click("toolbar.pause");require(!editor.consume_simulation_step(),"pause button did not dispatch the controller command");
    click("toolbar.play");
    click("assets.filter");input::InputEvent text;text.type=input::InputEventType::TextInput;text.text="NEEDLE";source->queue.push_back(text);tick();
    require(editor.asset_browser().filter=="NEEDLE","ImGui input did not update shared browser state");
    int customDraws=0;require(editor.register_panel({"custom","Custom",true,[&](EditorPanelContext& context){require(&context.world==&world,"custom context wrong");++customDraws;}}),"custom panel registration failed");
    tick();tick();require(customDraws>0,"registered panel did not use the shared presenter");
    for(float dpi:{1.25f,1.5f,2.f}){editor.set_display_size(2000,1300,dpi);tick();tick();const auto r=editor.ui_interaction_regions().at("assets.filter");require(r.width>0 && r.height>0,"DPI change lost input region");}
    editor.shutdown(&renderer);std::filesystem::remove_all(project);
    std::cout<<"Actual ImGui controls, XYZ layout, shared state, custom panels and DPI passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

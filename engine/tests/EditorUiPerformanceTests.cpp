#include "shinkou/editor/EditorLayer.h"
#include "shinkou/World.h"
#include "shinkou/input/InputSystem.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace shinkou;
using namespace shinkou::editor;
namespace {
void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
class Input final:public input::IInputBackend {
public:
    std::vector<input::InputEvent> queue,frame;
    input::MouseState mouse;
    std::vector<input::GamepadState> pads;
    bool initialize()override{return true;}
    void poll()override{frame=std::move(queue);queue.clear();}
    float control_value(std::string_view)const override{return 0;}
    const input::MouseState& mouse_state()const override{return mouse;}
    const std::vector<input::GamepadState>& gamepads()const override{return pads;}
    const std::vector<input::InputEvent>& events()const override{return frame;}
};
}
int main() {try {
    const auto project=std::filesystem::temp_directory_path()/("shinkou-ui-perf-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    World world;world.reserve(10002);
    auto& root=world.create_object("Root");
    const auto first=root.create_child("First").id();
    for(int i=0;i<10000;++i)root.create_child(i==9500?"Needle":"Object "+std::to_string(i));
    render::Renderer renderer;require(renderer.initialize(),"renderer failed");
    EditorLayer editor;editor.set_project_root(project.generic_string());editor.set_layout_path((project/"layout.json").generic_string());
    editor.set_display_size(1600,1000,1);require(editor.initialize(true),"presenter failed");
    auto backend=std::make_unique<Input>();auto* source=backend.get();input::InputSystem input(std::move(backend));input.initialize();
    FrameIndex frame=0;
    auto tick=[&]{input.poll();editor.process_input(input,world);renderer.begin_graph();editor.prepare_frame(renderer,world);editor.draw(renderer,world,1.f/60,frame++);};
    auto mouseClick=[&](ui::Rect r,float x=0.5f){
        const auto scale=editor.ui_dpi_scale();
        input::InputEvent event;event.type=input::InputEventType::MouseButtonDown;event.control="mouse:left";event.position={(r.x+r.width*x)*scale,(r.y+r.height/2)*scale};
        source->queue.push_back(event);tick();event.type=input::InputEventType::MouseButtonUp;source->queue.push_back(event);tick();tick();
    };
    auto key=[&](const char* control,bool down){input::InputEvent event;event.control=control;event.type=down?input::InputEventType::KeyDown:input::InputEventType::KeyUp;source->queue.push_back(event);tick();};
    for(int i=0;i<5;++i)tick();
    auto stats=editor.ui_performance();
    require(!stats.qaEnabled && stats.qaRegions==0 && editor.ui_interaction_regions().empty(),"production frame recorded QA regions");
    require(stats.hierarchyRows==10002 && stats.hierarchySubmitted>0 && stats.hierarchySubmitted<100,"hierarchy did not virtualize offscreen rows");
    const auto rebuilds=stats.hierarchyRebuilds;
    editor.select_object(first,world);for(int i=0;i<5;++i)tick();
    require(editor.ui_performance().hierarchyRebuilds==rebuilds,"selection or idle rebuilt hierarchy rows");
    editor.set_ui_qa_enabled(true);tick();
    const auto firstRect=editor.ui_interaction_regions().at("hierarchy:"+std::to_string(first));
    const auto filterRect=editor.ui_interaction_regions().at("hierarchy.filter");
    require(editor.ui_interaction_regions().size()<250,"QA retained offscreen hierarchy records");
    input::InputEvent move;move.type=input::InputEventType::MouseMove;
    move.position={firstRect.x+firstRect.width/2,firstRect.y+firstRect.height/2};
    input::InputEvent wheel;wheel.type=input::InputEventType::MouseWheel;wheel.delta={0,-100000};
    source->queue.push_back(move);source->queue.push_back(wheel);tick();tick();tick();
    require(editor.ui_interaction_regions().count("hierarchy-name:Object 9999")!=0 &&
        editor.ui_interaction_regions().count("hierarchy-name:First")==0,"scrolling did not clip to the last hierarchy rows");
    require(editor.ui_performance().hierarchySubmitted<100 && editor.ui_performance().hierarchyRebuilds==rebuilds,
        "scrolling rebuilt or submitted the entire hierarchy");
    wheel.delta.y=100000;source->queue.push_back(wheel);tick();tick();tick();
    require(editor.ui_interaction_regions().count("hierarchy-name:First")!=0,"scrolling back lost the first row");
    editor.set_ui_qa_enabled(false);
    mouseClick(firstRect);key("key:Delete",true);key("key:Delete",false);
    require(!world.find_object(first),"Delete shortcut depended on QA bookkeeping");
    mouseClick(filterRect);
    input::InputEvent text;text.type=input::InputEventType::TextInput;text.text="Needle";source->queue.push_back(text);tick();tick();
    require(editor.ui_performance().hierarchyRows==2,"filter did not preserve the matching ancestor chain");
    key("key:Left Ctrl",true);key("key:A",true);key("key:A",false);key("key:Left Ctrl",false);key("key:Backspace",true);key("key:Backspace",false);tick();
    require(editor.ui_performance().hierarchyRows==10001,"clearing filter lost hierarchy rows");
    editor.set_ui_qa_enabled(true);tick();
    const auto rootRect=editor.ui_interaction_regions().at("hierarchy:"+std::to_string(root.id()));
    mouseClick(rootRect,0.015f);tick();
    require(editor.ui_performance().hierarchyRows==1,"collapse failed after virtualization");
    mouseClick(rootRect,0.015f);tick();
    for(const char* theme:{"dark","light","high-contrast"}) {
    editor.set_theme(theme);
    for(float dpi:{1.25f,1.5f,2.f}) {
        editor.set_display_size(1600,1000,dpi);tick();tick();
        const auto header=editor.ui_interaction_regions().at("hierarchy.filter");
        move.position={100*dpi,300*dpi};wheel.delta.y=-100000;
        source->queue.push_back(move);source->queue.push_back(wheel);tick();tick();tick();
        require(editor.ui_interaction_regions().count("hierarchy-name:Object 9999")!=0,
            "fractional DPI row-height drift hid the final hierarchy row");
        const auto last=editor.ui_interaction_regions().at("hierarchy-name:Object 9999");
        require((last.y+last.height)*dpi<=1000,"QA exposed a clipped row outside the client");
        require(editor.ui_performance().hierarchySubmitted<100,"DPI change disabled virtualization");
        require(editor.ui_interaction_regions().at("hierarchy.filter").y==header.y,"search header scrolled with the list");
        wheel.delta.y=100000;source->queue.push_back(wheel);tick();tick();tick();
        const auto top=editor.ui_interaction_regions().at("hierarchy-name:Object 0");
        mouseClick(top);key("key:End",true);key("key:End",false);tick();tick();
        require(editor.ui_interaction_regions().count("hierarchy-name:Object 9999")!=0,"End did not reach the last virtual row");
        key("key:Home",true);key("key:Home",false);tick();tick();
    }
    }
    editor.set_ui_qa_enabled(false);tick();
    require(editor.ui_interaction_regions().empty(),"disabling QA left stale regions");
    editor.shutdown(&renderer);std::filesystem::remove_all(project);
    std::cout<<"Virtualized hierarchy, stable cache, filtering, collapse and production input passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}

#include "shinkou/editor/EditorPreferences.h"
#include "shinkou/editor/EditorViewportController.h"
#include "shinkou/editor/EditorDocument.h"
#include "shinkou/editor/EditorLayer.h"
#include "shinkou/editor/EditorPointerCapture.h"
#include "shinkou/components/CameraComponent.h"
#include "shinkou/components/PrimitiveMeshComponent.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <chrono>
#include <filesystem>

using namespace shinkou;
using namespace shinkou::editor;
namespace {
struct PointerProbe {math::Vec2 position{100,100};bool focused{true},canWarp{true};int acquired{},released{},restored{};};
class PointerDevice final:public IEditorPointerDevice {
    PointerProbe& probe_;
public:
    explicit PointerDevice(PointerProbe& probe):probe_(probe){}
    bool begin(std::uintptr_t,EditorPointerBounds,math::Vec2)override{++probe_.acquired;return true;}
    bool sample(math::Vec2& position)override{position=probe_.position;return probe_.focused;}
    bool warp(math::Vec2 position)override{if(!probe_.canWarp)return false;probe_.position=position;return true;}
    void release(bool restore)noexcept override{++probe_.released;probe_.restored+=restore;}
};
}
int main() {
    PointerProbe probe;
    {
        EditorPointerCapture pointer(std::make_unique<PointerDevice>(probe));
        assert(pointer.begin(1,{0,0,300,300},{100,100}));
        probe.position={125,110};const auto relative=pointer.delta({125,110},{999,999});assert(relative.x==25 && relative.y==10);
        const auto stationary=pointer.delta({100,100},{999,999});assert(stationary.x==0 && stationary.y==0);
        probe.focused=false;pointer.delta({100,100},{});assert(!pointer.active() && pointer.lost_focus() && probe.released==1 && probe.restored==0);
        probe.focused=true;assert(pointer.begin(1,{0,0,300,300},{100,100}));
        probe.canWarp=false;probe.position={110,100};pointer.delta({110,100},{});assert(!pointer.active() && probe.released==2);
        probe.canWarp=true;assert(pointer.begin(1,{0,0,300,300},{110,100}));pointer.end(true);assert(probe.released==3 && probe.restored==1);
        pointer.end(true);assert(probe.released==3);
        assert(pointer.begin(1,{0,0,300,300},{110,100}));
    }
    assert(probe.released==4);
    EditorActionRegistry registry;EditorPreferences preferences;std::string error,json;
    assert(preferences.valid(registry,error) && preferences.to_json(json,error));
    EditorPreferences restored;assert(EditorPreferences::from_json(json,restored,error));
    assert(restored.bindings.size()==registry.actions().size());
    auto invalid=preferences;invalid.camera.flySpeed=NAN;assert(!invalid.valid(registry,error));
    const auto goodSpeed=restored.camera.flySpeed;
    assert(!EditorPreferences::from_json("{\"version\":999}",restored,error) && restored.camera.flySpeed==goodSpeed);
    auto conflict=preferences.bindings.front();conflict.action="scene.open";
    assert(!EditorInputRouter::conflict(registry,preferences.bindings,conflict).empty());
    EditorInputRouter router;EditorInputFrame frame;
    const auto w=static_cast<std::size_t>(EditorKey::W),shift=static_cast<std::size_t>(EditorKey::Shift);
    frame.down[w]=frame.pressed[w]=true;
    assert(router.route(registry,preferences.bindings,frame,EditorInputContext::Scene).contains("tool.move"));
    const auto fly=router.route(registry,preferences.bindings,frame,EditorInputContext::FlyCamera);
    assert(fly.contains("camera.forward") && !fly.contains("tool.move"));
    frame.down[shift]=true;frame.modifiers=ShiftModifier;frame.itemActive=true;
    const auto fast=router.route(registry,preferences.bindings,frame,EditorInputContext::FlyCamera);
    assert(fast.contains("camera.boost") && fast.contains("camera.forward"));
    assert(std::abs(EditorViewportController::movement_speed(fast,preferences.camera)-20)<.001f);
    for(int block=0;block<3;++block) {
        auto blocked=frame;if(block==0)blocked.textInput=true;if(block==1)blocked.popupOpen=true;if(block==2)blocked.focused=false;
        assert(router.route(registry,preferences.bindings,blocked,EditorInputContext::FlyCamera).count==0);
    }
    math::Transform poseA,poseB;
    for(int i=0;i<60;++i)EditorViewportController::fly(poseA,{1,0,1},1.f/60,5,false);
    for(int i=0;i<120;++i)EditorViewportController::fly(poseB,{1,0,1},1.f/120,5,false);
    assert(math::Length(poseA.position-poseB.position)<.001f && std::abs(math::Length(poseA.position)-5)<.001f);
    World world;auto& cameraObject=world.create_object("Game Camera");
    cameraObject.add_component<components::CameraComponent>();
    cameraObject.get_component<components::TransformComponent>()->set_position({2,3,-7});world.refresh_transforms();
    const auto gameView=render::extract_render_view(world,16.f/9);assert(gameView.has_camera());
    EditorViewportController editorCamera;editorCamera.initialize_from_world(world);editorCamera.apply_settings(preferences.camera);
    editorCamera.fly(fly,.1f,preferences.camera);
    assert(math::Length(editorCamera.pose().position-gameView.transform.position)>.1f);
    assert(math::Length(render::extract_render_view(world,16.f/9).transform.position-gameView.transform.position)<.001f);
    auto& parent=world.create_object("Parent");auto& mesh=parent.create_child("Mesh");mesh.add_component<components::PrimitiveMeshComponent>();
    mesh.create_child("Child");world.refresh_transforms();
    EditorDocument document;assert(EditorDocument::capture_subtree(mesh,document,error));
    const auto count=world.object_count();ObjectId copy{};
    assert(document.instantiate(world,parent.id(),copy,error));
    assert(world.object_count()==count+2 && world.find_object(copy)->parent()==&parent && world.find_object(copy)->children().size()==1);
    auto broken=document;broken.objects.back().components.push_back({"UnknownComponent"});
    assert(!broken.instantiate(world,0,copy,error));assert(world.object_count()==count+2);
    const auto project=std::filesystem::temp_directory_path()/("shinkou-controls-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(project/"assets");
    EditorLayer editor;editor.set_project_root(project.generic_string());assert(editor.initialize(false));
    editor.select_object(mesh.id(),world);editor.execute_command(EditorCommand::SaveScene,"assets/First.scene",world);
    assert(editor.scene_named() && !editor.scene_dirty());const auto savedCount=world.object_count();
    editor.execute_command(EditorCommand::NewScene,{},world);
    assert(!editor.scene_named() && editor.scene_path()!="assets/First.scene" && world.object_count()==0);
    editor.execute_command(EditorCommand::Undo,{},world);
    assert(editor.scene_named() && editor.scene_path()=="assets/First.scene" && world.object_count()==savedCount && !editor.scene_dirty());
    auto* selected=world.find_object(editor.layout().selectedObject);assert(selected);
    auto* transform=selected->get_component<components::TransformComponent>();const auto original=transform->local.position;
    const auto field=std::to_string(transform->id())+":position";
    assert(editor.begin_inspector_edit(field) && editor.edit_inspector_value(field,math::Vec3{9,8,7}));editor.finish_inspector_edit();
    editor.reset_layout();editor.execute_command(EditorCommand::Undo,{},world);
    assert(math::Length(world.find_object(editor.layout().selectedObject)->get_component<components::TransformComponent>()->local.position-original)<.001f);
    auto settings=editor.preferences();settings.camera.flySpeed=12.5f;assert(editor.set_preferences(settings) && editor.save_preferences());
    render::Renderer renderer;assert(renderer.initialize());editor.prepare_frame(renderer,world);
    editor.execute_action("view.frame",world);const auto remembered=editor.viewport_controller().pose().position;assert(editor.save_layout());
    EditorLayer reopened;reopened.set_project_root(project.generic_string());assert(reopened.initialize(false));reopened.prepare_frame(renderer,world);
    assert(std::abs(reopened.preferences().camera.flySpeed-12.5f)<.001f);
    assert(math::Length(reopened.viewport_controller().pose().position-remembered)<.001f);
    reopened.shutdown(&renderer);editor.shutdown(&renderer);std::filesystem::remove_all(project);
    std::cout<<"Preferences, context routing, camera isolation, movement and subtree instantiation passed\n";
}

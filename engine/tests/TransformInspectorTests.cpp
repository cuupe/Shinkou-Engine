#include "shinkou/World.h"
#include "shinkou/editor/EditorLayer.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

using namespace shinkou;
using namespace shinkou::editor;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
bool near(math::Vec3 a,math::Vec3 b,float tolerance=0.001f){return math::Length(a-b)<tolerance;}
void rotation_roundtrip(math::Vec3 degrees) {
    const auto q=math::FromEulerXYZ(degrees*math::Radians(1));
    for(const auto source:{q,math::Quat{-q.x,-q.y,-q.z,-q.w}}) {
        const auto r=math::FromEulerXYZ(math::ToEulerXYZ(source));
        for(const auto axis:{math::Vec3{1,0,0},math::Vec3{0,1,0},math::Vec3{0,0,1}})
            require(near(math::Rotate(q,axis),math::Rotate(r,axis),0.0015f),"Euler XYZ orientation round-trip failed");
    }
}
}
int main(int argc,char**) {
    try {
        std::mt19937 random(0x58a3);std::uniform_real_distribution<float> angle(-720,720);
        for(int i=0;i<12000;++i)rotation_roundtrip({angle(random),angle(random),angle(random)});
        for(float y:{-90.001f,-90.0f,-89.999f,89.999f,90.0f,90.001f})
            for(float x:{-179.0f,-42.0f,0.0f,78.0f,180.0f})rotation_roundtrip({x,y,37});
        World world;auto& parent=world.create_object("Parent");auto& child=parent.create_child("Child");
        auto* p=parent.get_component<components::TransformComponent>();auto* c=child.get_component<components::TransformComponent>();
        p->set_position({10,0,0});c->set_position({1,2,3});world.refresh_transforms();
        require(near(math::TransformPoint(c->world_matrix(),{}),{11,2,3}),"parent transform not composed");
        p->local.position={20,0,0};world.refresh_transforms();
        require(near(math::TransformPoint(c->world_matrix(),{}),{21,2,3}),"direct legacy write left stale child cache");
        require(c->set_scale({-1,0,2}),"finite mirrored or zero scale rejected");
        const auto before=c->local;
        const float invalid=std::numeric_limits<float>::quiet_NaN();
        require(!c->set_position({invalid,2,3})&&!c->set_rotation({0,0,0,0})&&!c->set_euler_degrees({0,invalid,0}),"invalid transform accepted");
        auto bad=before;bad.scale.x=invalid;bad.position.x=99;
        require(!c->set_local_transform(bad)&&near(c->local.position,before.position),"invalid transform was partially applied");
        require(c->set_euler_degrees({450,-35,721})&&near(c->euler_degrees(),{450,-35,721}),"editor winding was lost");
        const auto orientation=c->local.rotation;c->set_rotation({-orientation.x,-orientation.y,-orientation.z,-orientation.w});
        require(near(c->euler_degrees(),{450,-35,721}),"equivalent quaternion sign reset editor winding");
        EditorUiModel model;model.select_object(child.id());model.sync(world);
        bool rotationFound=false;
        for(const auto& f:model.inspector_fields())if(f.componentName=="Transform"&&f.presentation==PropertyPresentation::EulerDegrees) {
            rotationFound=true;require(f.type==PropertyType::Quaternion&&std::holds_alternative<math::Vec3>(f.data),"rotation presentation must be a typed Euler vector");
        }
        require(rotationFound,"Transform group metadata missing");
        EditorDocument saved;std::string error,json;
        require(EditorDocument::capture(world,child.id(),saved,error)&&saved.to_json(json,error),"document capture failed");
        World loaded;ObjectId selected{};require(saved.restore(loaded,selected,error),"quaternion document round-trip failed");
        const auto* restored=loaded.find_object(child.id())->get_component<components::TransformComponent>();
        require(near(math::Rotate(c->local.rotation,{1,0,0}),math::Rotate(restored->local.rotation,{1,0,0})),"serialized rotation changed");

        World edited;EditorLayer editor;
        editor.execute_command(EditorCommand::CreateEmpty,{},edited);
        auto pose=[&]{auto* object=edited.find_object(editor.layout().selectedObject);require(object!=nullptr,"edited selection disappeared");return object->get_component<components::TransformComponent>();};
        const auto field=std::to_string(pose()->id())+":position";
        require(editor.begin_inspector_edit(field),"typed edit did not start");
        for(int i=1;i<=40;++i)require(editor.edit_inspector_value(field,math::Vec3{float(i),2,3}),"live axis edit failed");
        editor.finish_inspector_edit();
        require(near(pose()->local.position,{40,2,3}),"live value not committed");
        editor.execute_command(EditorCommand::Undo,{},edited);
        require(near(pose()->local.position,{}),"one gesture did not undo atomically");
        editor.execute_command(EditorCommand::Redo,{},edited);
        require(near(pose()->local.position,{40,2,3}),"redo lost vector axes");
        const auto currentField=std::to_string(pose()->id())+":position";
        require(editor.begin_inspector_edit(currentField)&&editor.edit_inspector_value(currentField,math::Vec3{77,2,3}),"cancel edit did not start");
        editor.finish_inspector_edit(true);
        require(near(pose()->local.position,{40,2,3}),"Escape did not restore previous vector");
        require(!editor.edit_inspector_value(currentField,std::string("1 2 3"))&&!editor.edit_inspector_value(currentField,math::Vec3{invalid,0,0}),"invalid typed edit accepted");
        editor.execute_command(EditorCommand::Undo,{},edited);
        require(near(pose()->local.position,{}),"cancel created an undo entry");
        // Structural undo replaces object/component IDs. Older property history
        // must resolve against the restored hierarchy rather than stale IDs.
        editor.execute_command(EditorCommand::Redo,{},edited);
        editor.execute_command(EditorCommand::CreateChild,{},edited);
        editor.execute_command(EditorCommand::Undo,{},edited);
        editor.execute_command(EditorCommand::Undo,{},edited);
        require(near(pose()->local.position,{}),"property history broke across structural undo");
        pose()->game_object().enable_ecs();
        struct RenderPayload{int handle;};
        edited.ecs().emplace<RenderPayload>(pose()->game_object().ecs_entity(),RenderPayload{71});
        const auto ecsField=std::to_string(pose()->id())+":position";
        require(editor.edit_inspector_value(ecsField,math::Vec3{9,8,7}),"ECS-owned object property edit was blocked by scene serialization");
        editor.execute_command(EditorCommand::Undo,{},edited);
        require(near(pose()->local.position,{})&&edited.ecs().get<RenderPayload>(pose()->game_object().ecs_entity()).handle==71,"property undo discarded ECS rendering data");
        require(editor.edit_inspector_value(ecsField,math::Vec3{4,5,6}),"external lifetime fixture edit failed");
        pose()->game_object().destroy();
        auto& replacement=edited.create_object("Replacement");
        replacement.get_component<components::TransformComponent>()->set_position({8,8,8});
        editor.execute_command(EditorCommand::Undo,{},edited);
        require(near(replacement.get_component<components::TransformComponent>()->local.position,{8,8,8}),"stale property history edited a replacement object");
        std::cout<<"Typed inspector, quaternion persistence, Euler XYZ, parent caches and transactional undo passed\n";
        if(argc>1) {
            constexpr int count=1000000;float checksum=0;
            const auto start=std::chrono::steady_clock::now();
            for(int i=0;i<count;++i){const auto v=math::ToEulerXYZ(math::FromEulerXYZ({i*0.00001f,0.3f,0.7f}));checksum+=v.x;}
            const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            std::cout<<"Scalar Euler conversions: "<<count/seconds<<" pairs/s, "<<seconds<<" s, checksum="<<checksum<<"\n";
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

#include "shinkou/editor/EditorWorldViewport.h"
#include "shinkou/render/Renderer.h"
#include <cassert>
#include <cmath>
#include <cstring>

using namespace shinkou;
using namespace shinkou::editor;
namespace {
bool near(math::Vec3 a,math::Vec3 b) {return math::Length(a-b)<0.001f;}
}
int main() {
    render::CameraComponent camera;
    math::Transform level;level.rotation=math::FromAxisAngle({0,1,0},-.42f)*math::FromAxisAngle({1,0,0},.25f);
    const auto levelForward=math::Rotate(level.rotation,{0,0,1});float levelFocus=5;
    navigate_editor_camera(camera,level,ViewportNavigation::Orbit,{1,1},900,1.5f,levelFocus);
    assert(math::Length(math::Rotate(level.rotation,{0,0,1})-levelForward)<.01f);
    assert(std::abs(math::Rotate(level.rotation,{1,0,0}).y)<.00001f);
    math::Transform pose;pose.position={2,3,-5};pose.rotation=math::FromEulerXYZ({0.4f,0.7f,0});
    const auto start=pose;const auto forward=math::Rotate(pose.rotation,{0,0,1});float focus=5;
    navigate_editor_camera(camera,pose,ViewportNavigation::Pan,{120,-45},900,1.5f,focus);
    assert(std::abs(math::Dot(pose.position-start.position,forward))<0.0001f);
    const auto panned=pose.position;
    pose=start;float focus2=5;
    navigate_editor_camera(camera,pose,ViewportNavigation::Pan,{240,-90},1800,3,focus2);
    assert(near(panned,pose.position));
    pose=start;
    navigate_editor_camera(camera,pose,ViewportNavigation::Orbit,{90,15},900,1.5f,focus);
    assert(near(pose.position,start.position));
    const auto turned=math::Rotate(pose.rotation,{0,0,1});
    pose=start;
    navigate_editor_camera(camera,pose,ViewportNavigation::Orbit,{180,30},1800,3,focus2);
    assert(near(turned,math::Rotate(pose.rotation,{0,0,1})));
    const auto held=pose.rotation;
    for (int i=0;i<120;++i)
        navigate_editor_camera(camera,pose,ViewportNavigation::Orbit,{0,0},900,1.5f,focus);
    assert(near(math::Rotate(held,{0,0,1}),math::Rotate(pose.rotation,{0,0,1})));
    pose=start;
    for (int i=0;i<90;++i)
        navigate_editor_camera(camera,pose,ViewportNavigation::Orbit,{1,1.0f/6},900,1.5f,focus);
    assert(near(turned,math::Rotate(pose.rotation,{0,0,1})));
    navigate_editor_camera(camera,pose,ViewportNavigation::Orbit,{0,100000},1800,3,focus2);
    assert(std::abs(math::Rotate(pose.rotation,{0,0,1}).y)<1);
    camera.orthographic=true;camera.orthographicSize=8;pose=start;
    navigate_editor_camera(camera,pose,ViewportNavigation::Pan,{100,0},800,2,focus);
    assert(std::abs(math::Length(pose.position-start.position)-1)<0.001f);

    const auto projection=math::Perspective(math::Pi/3,16.0f/9,0.05f,100);
    const auto ray=make_viewport_ray(projection,{800,450},{1600,900});assert(ray);
    assert(near(ray->direction,{0,0,1}));
    const auto scaled=make_viewport_ray(projection,{1600,900},{3200,1800});assert(scaled);
    assert(near(ray->direction,scaled->direction));
    assert(!make_viewport_ray(projection,{1600,900},{1600,900}));
    assert(!make_viewport_ray(projection,{NAN,0},{1600,900}));
    assert(!make_viewport_ray(math::Mat4{}, {1,1},{1600,900}));
    const auto hit=intersect_viewport_triangle(*ray,{-1,-1,3},{1,-1,3},{0,1,3});assert(hit);
    assert(std::abs(*hit-2.95f)<0.001f);
    assert(intersect_viewport_triangle(*ray,{0,1,3},{1,-1,3},{-1,-1,3}));
    assert(!intersect_viewport_triangle(*ray,{-1,-1,-3},{1,-1,-3},{0,1,-3}));
    assert(!intersect_viewport_triangle(*ray,{0,0,1},{0,0,1},{0,0,1}));
    const auto ortho=math::Orthographic(-8,8,-4.5f,4.5f,0.05f,100);
    const auto left=make_viewport_ray(ortho,{400,450},{1600,900});assert(left);
    assert(near(left->direction,{0,0,1}) && std::abs(left->origin.x+4)<0.001f);

    render::Renderer renderer(render::BackendApi::Null);assert(renderer.initialize());
    shinkou::World world;
    auto cameraEntity=world.ecs().create();
    render::TransformComponent cameraPose;cameraPose.local.position={0,0,-5};
    world.ecs().emplace<render::TransformComponent>(cameraEntity,cameraPose);
    world.ecs().emplace<render::CameraComponent>(cameraEntity,render::CameraComponent{});
    auto& parent=world.create_object("Parent");auto& object=parent.create_child("Pick target");
    const auto entity=object.enable_ecs();
    const math::Vec3 vertices[]={{-1,-1,0},{1,-1,0},{0,1,0}};const std::uint32_t indices[]={0,1,2};
    render::BufferDesc vb{sizeof(vertices),sizeof(vertices[0]),true};vb.initialData.resize(sizeof(vertices));std::memcpy(vb.initialData.data(),vertices,sizeof(vertices));
    render::BufferDesc ib{sizeof(indices),4,false,true};ib.initialData.resize(sizeof(indices));std::memcpy(ib.initialData.data(),indices,sizeof(indices));
    render::ShaderDesc vs;vs.name="viewport_test_vs";vs.source="void main() {}";
    auto fs=vs;fs.name="viewport_test_fs";fs.stage=render::ShaderStage::Fragment;
    render::PipelineDesc pipeline;pipeline.name="viewport_test_pipeline";
    pipeline.vertexShader=renderer.create_shader(vs).id;pipeline.fragmentShader=renderer.create_shader(fs).id;
    const auto material=renderer.create_material({"viewport_test_material",renderer.create_pipeline(pipeline),{},false});assert(material);
    world.ecs().emplace<render::TransformComponent>(entity,render::TransformComponent{});
    world.ecs().emplace<render::MeshRendererComponent>(entity,render::MeshRendererComponent{renderer.create_buffer(vb),renderer.create_buffer(ib),material,3,true});
    world.refresh_transforms();
    EditorWorldViewport viewport;
    renderer.begin_graph();assert(viewport.render(renderer,world,1600,900,{},true));
    assert(renderer.graph().compile());
    render::RenderScene debug;debug.extract(world,renderer,1600.0f/900);
    assert(debug.meshes().size()==1);
    assert(viewport.pick(world,renderer,{}, {800,450})==object.id());
    const auto first=viewport.texture();
    renderer.begin_graph();assert(viewport.render(renderer,world,3200,1800,{}));
    assert(viewport.texture().id!=first.id && !renderer.resource_description(first));
    assert(viewport.pick(world,renderer,{}, {1600,900})==object.id());
    auto snapshot=std::make_shared<EditorModelPreviewSnapshot>();
    snapshot->vertices=std::make_shared<const std::vector<math::Vec3>>(std::begin(vertices),std::end(vertices));
    snapshot->indices=std::make_shared<const std::vector<std::uint32_t>>(std::begin(indices),std::end(indices));
    snapshot->minX=snapshot->minY=-1;snapshot->maxX=snapshot->maxY=1;
    const auto nearer=world.create_object("Nearest model").id();
    std::vector<EditorModelSceneInstance> candidates{{10,nearer,snapshot,math::Translation({0,0,-1})},
        {11,99,snapshot,math::Translation({0,0,2})}};
    assert(viewport.pick(world,renderer,candidates,{1600,900})==nearer);
    candidates[0].model=math::Translation({0,0,2000});
    assert(viewport.pick(world,renderer,candidates,{1600,900})==object.id());
    parent.get_component<components::TransformComponent>()->set_position({20,0,0});
    world.refresh_transforms();
    renderer.begin_graph();assert(viewport.render(renderer,world,3200,1800,{}));
    assert(viewport.pick(world,renderer,{}, {1600,900})==0);
    const auto last=viewport.texture();viewport.release(renderer);
    assert(!renderer.resource_description(last));
}

#include "shinkou/editor/EditorWorldViewport.h"
#include "shinkou/render/Renderer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace shinkou::editor {
namespace {
bool intersects_bounds(const ViewportRay& ray, const math::Mat4& model, math::Vec3 minimum, math::Vec3 maximum) {
    math::Mat4 inverse;
    if (!math::Inverse(model,inverse)) return false;
    const auto origin=math::TransformPoint(inverse,ray.origin),direction=math::TransformVector(inverse,ray.direction);
    const float origins[]={origin.x,origin.y,origin.z},directions[]={direction.x,direction.y,direction.z};
    const float low[]={minimum.x,minimum.y,minimum.z},high[]={maximum.x,maximum.y,maximum.z};
    float first=0,last=ray.maxDistance;
    for (int axis=0;axis<3;++axis) {
        if (std::abs(directions[axis])<1e-8f) {if(origins[axis]<low[axis] || origins[axis]>high[axis])return false;continue;}
        float a=(low[axis]-origins[axis])/directions[axis],b=(high[axis]-origins[axis])/directions[axis];
        if (a>b) std::swap(a,b);
        first=std::max(first,a);last=std::min(last,b);if(first>last)return false;
    }
    return true;
}
}
std::optional<ViewportRay> make_viewport_ray(const math::Mat4& viewProjection,
    math::Vec2 point, math::Vec2 size) noexcept {
    if (!math::IsFinite(point.x) || !math::IsFinite(point.y) ||
        !math::IsFinite(size.x) || !math::IsFinite(size.y) ||
        size.x <= 0 || size.y <= 0 || point.x < 0 || point.y < 0 ||
        point.x >= size.x || point.y >= size.y) return {};
    math::Mat4 inverse;
    if (!math::Inverse(viewProjection, inverse)) return {};
    const float x = point.x / size.x * 2 - 1, y = 1 - point.y / size.y * 2;
    const auto nearPoint = math::TransformPoint(inverse, {x,y,0});
    const auto farPoint = math::TransformPoint(inverse, {x,y,1});
    const auto direction = farPoint - nearPoint;
    if (!math::IsFinite(math::LengthSquared(direction)) || math::LengthSquared(direction) < math::Epsilon) return {};
    return ViewportRay{nearPoint,math::Normalize(direction),math::Length(direction)};
}

std::optional<float> intersect_viewport_triangle(const ViewportRay& ray,
    math::Vec3 a, math::Vec3 b, math::Vec3 c) noexcept {
    const auto edge1=b-a, edge2=c-a, p=math::Cross(ray.direction,edge2);
    const float det=math::Dot(edge1,p);
    if (!math::IsFinite(det) || std::abs(det)<1.0e-8f) return {};
    const auto t=ray.origin-a;
    const float u=math::Dot(t,p)/det;
    if (u<0 || u>1) return {};
    const auto q=math::Cross(t,edge1);
    const float v=math::Dot(ray.direction,q)/det;
    if (v<0 || u+v>1) return {};
    const float distance=math::Dot(edge2,q)/det;
    return math::IsFinite(distance) && distance>=0 && distance<=ray.maxDistance ? std::optional<float>(distance) : std::nullopt;
}

void navigate_editor_camera(render::CameraComponent& camera, math::Transform& transform,
    ViewportNavigation action, math::Vec2 delta, float height, float dpi, float& distance) noexcept {
    if (!math::IsFinite(delta.x) || !math::IsFinite(delta.y) || !math::IsFinite(height) || height<=0) return;
    const auto forward=math::Rotate(transform.rotation,{0,0,1});
    if (action==ViewportNavigation::Zoom) {
        const float factor=std::exp(-std::clamp(delta.y,-10.0f,10.0f)*0.15f);
        if (camera.orthographic) camera.orthographicSize=std::clamp(camera.orthographicSize*factor,0.05f,10000.0f);
        else {
            const float next=std::clamp(distance*factor,0.1f,10000.0f);
            transform.position=transform.position+forward*(distance-next); distance=next;
        }
    } else if (action==ViewportNavigation::Pan || camera.orthographic) {
        const auto right=math::Rotate(transform.rotation,{1,0,0});
        const auto up=math::Rotate(transform.rotation,{0,1,0});
        const float units=(camera.orthographic ? camera.orthographicSize :
            2*distance*std::tan(camera.verticalFieldOfView*0.5f))/height;
        transform.position=transform.position+(right*-delta.x+up*delta.y)*units;
    } else {
        const float sensitivity=0.004f/std::max(0.25f,dpi);
        const float yaw=std::atan2(forward.x,forward.z)+delta.x*sensitivity;
        const float pitch=std::clamp(-std::asin(std::clamp(forward.y,-1.0f,1.0f))+delta.y*sensitivity,
            -math::HalfPi+0.01f,math::HalfPi-0.01f);
        // Mouse look rotates around the eye; it never translates the camera.
        transform.rotation=math::FromEulerXYZ({pitch,yaw,0});
    }
}

bool EditorWorldViewport::render(render::Renderer& renderer, const shinkou::World& world,
    std::uint32_t width, std::uint32_t height, const std::vector<EditorModelSceneInstance>& instances,bool showGrid) {
    if (!width || !height || width>16384 || height>16384) return false;
    if (!color_ || width!=colorDesc_.width || height!=colorDesc_.height) {
        render::TextureDesc color; color.width=width;color.height=height;color.format="bgra8";color.renderTarget=true;
        render::TextureDesc depth; depth.width=width;depth.height=height;depth.format="d24s8";depth.depthStencil=true;
        const auto nextColor=renderer.create_texture(color);
        const auto nextDepth=renderer.create_depth_stencil(depth);
        if (!nextColor || !nextDepth) {
            renderer.destroy_resource(nextColor); renderer.destroy_resource(nextDepth); return false;
        }
        renderer.destroy_resource(color_); renderer.destroy_resource(depth_);
        color_=nextColor; depth_=nextDepth; colorDesc_=color; depthDesc_=depth;
    }
    scene_.extract(world,renderer,static_cast<float>(width)/height);
    forward_.build(renderer,scene_,color_,colorDesc_,depth_,depthDesc_);
    if (!skyReady_) skyReady_=sky_.initialize(renderer,"bgra8");
    if (skyReady_) {
        render::SkySettings sky;sky.enabled=true;
        sky.zenith={0.035f,0.05f,0.075f};sky.horizon={0.19f,0.23f,0.28f};sky.ground={0.045f,0.05f,0.06f};
        if (!sky_.record(renderer,renderer.graph(),scene_.view(),sky,color_,depth_)) return false;
    }
    modelState_=models_.render(renderer,instances,scene_.view().viewProjection,
        scene_.view().transform.position,color_,depth_);
    if (showGrid && !grid_.record(renderer,scene_.view(),color_,depth_)) return false;
    std::vector<assets::AssetId> assets;
    std::vector<std::uint64_t> objects;
    for (const auto& item:instances) {assets.push_back(item.assetId);objects.push_back(item.objectId);}
    models_.prune(renderer,assets,objects);
    return true;
}

ObjectId EditorWorldViewport::pick(const shinkou::World& world, const render::Renderer& renderer,
    const std::vector<EditorModelSceneInstance>& instances, math::Vec2 point) const {
    const auto ray=make_viewport_ray(scene_.view().viewProjection,point,
        {static_cast<float>(width()),static_cast<float>(height())});
    if (!ray) return 0;
    float closest=std::numeric_limits<float>::max(); ObjectId selected=0;
    const auto triangle=[&](ObjectId id,const math::Mat4& model,math::Vec3 a,math::Vec3 b,math::Vec3 c) {
        const auto hit=intersect_viewport_triangle(*ray,math::TransformPoint(model,a),
            math::TransformPoint(model,b),math::TransformPoint(model,c));
        if (hit && *hit<closest) {closest=*hit;selected=id;}
    };
    for (const auto& item:instances) {
        if (!item.snapshot || !item.snapshot->vertices || !item.snapshot->indices) continue;
        const auto& snapshot=*item.snapshot;
        if (!intersects_bounds(*ray,item.model,{snapshot.minX,snapshot.minY,snapshot.minZ},
            {snapshot.maxX,snapshot.maxY,snapshot.maxZ})) continue;
        const auto& positions=*item.snapshot->vertices; const auto& indices=*item.snapshot->indices;
        for (std::size_t i=0;i+2<indices.size();i+=3) {
            if (indices[i]>=positions.size() || indices[i+1]>=positions.size() || indices[i+2]>=positions.size()) continue;
            triangle(item.objectId,item.model,positions[indices[i]],positions[indices[i+1]],positions[indices[i+2]]);
        }
    }
    world.each_game_object([&](const GameObject& object) {
        if (!object.active_in_hierarchy() || !object.has_ecs_entity()) return;
        for (const auto& mesh:scene_.meshes()) {
            if (mesh.entity!=object.ecs_entity()) continue;
            const auto* vb=renderer.resource_description(mesh.vertexBuffer);
            const auto* ib=renderer.resource_description(mesh.indexBuffer);
            const auto* vertices=vb?std::get_if<render::BufferDesc>(vb):nullptr;
            const auto* indices=ib?std::get_if<render::BufferDesc>(ib):nullptr;
            if (!vertices || !indices || vertices->stride<sizeof(math::Vec3)) continue;
            std::uint32_t positionOffset=0;
            if (const auto* material=std::get_if<render::MaterialDesc>(&mesh.materialDescription)) {
                if (const auto* description=renderer.resource_description(material->pipeline))
                    if (const auto* pipeline=std::get_if<render::PipelineDesc>(description)) {
                        if (!pipeline->vertexLayout.attributes.empty()) {
                            const auto attribute=std::find_if(pipeline->vertexLayout.attributes.begin(),pipeline->vertexLayout.attributes.end(),
                                [](const auto& value){return value.semantic=="POSITION" && value.semanticIndex==0 && value.binding==0 && value.format==render::VertexFormat::Float3;});
                            if (attribute==pipeline->vertexLayout.attributes.end()) continue;
                            positionOffset=attribute->offset;
                        }
                    }
            }
            if (positionOffset>vertices->stride || sizeof(math::Vec3)>vertices->stride-positionOffset) continue;
            const auto position=[&](std::uint32_t index,math::Vec3& out) {
                const auto offset=static_cast<std::size_t>(index)*vertices->stride+positionOffset;
                if (offset>vertices->initialData.size() || sizeof(out)>vertices->initialData.size()-offset) return false;
                std::memcpy(&out,vertices->initialData.data()+offset,sizeof(out));return true;
            };
            for (std::size_t i=0;i+2<mesh.indexCount;i+=3) {
                const auto offset=(static_cast<std::size_t>(mesh.firstIndex)+i)*sizeof(std::uint32_t);
                if (offset>indices->initialData.size() || 12>indices->initialData.size()-offset) break;
                std::uint32_t ix[3];std::memcpy(ix,indices->initialData.data()+offset,sizeof(ix));
                math::Vec3 a,b,c;
                const auto vertex=[&](std::uint32_t index,math::Vec3& out) {
                    const auto shifted=static_cast<std::int64_t>(index)+mesh.vertexOffset;
                    return shifted>=0 && shifted<=UINT32_MAX && position(static_cast<std::uint32_t>(shifted),out);
                };
                if (vertex(ix[0],a) && vertex(ix[1],b) && vertex(ix[2],c)) triangle(object.id(),mesh.transform,a,b,c);
            }
        }
    });
    return selected;
}

void EditorWorldViewport::release(render::Renderer& renderer) {
    models_.clear(renderer);forward_.release(renderer);grid_.release(renderer);sky_.release(renderer);skyReady_=false;
    renderer.destroy_resource(color_);renderer.destroy_resource(depth_);color_={};depth_={};
}
}

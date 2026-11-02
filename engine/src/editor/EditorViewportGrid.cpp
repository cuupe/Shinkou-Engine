#include "shinkou/editor/EditorViewportGrid.h"
#include "shinkou/render/Renderer.h"
#include <cmath>
#include <cstring>

namespace shinkou::editor {
namespace {
constexpr const char *vertexSource = R"(
struct Output {float4 position:SV_Position;float2 ndc:TEXCOORD0;};
Output main(uint id:SV_VertexID) {
    float2 p[6]={float2(-1,-1),float2(-1,1),float2(1,1),float2(-1,-1),float2(1,1),float2(1,-1)};
    Output o;o.position=float4(p[id],0,1);o.ndc=p[id];return o;
})";
constexpr const char *fragmentSource = R"(
cbuffer GridFrame:register(b4) {float4x4 inverseViewProjection;float4x4 viewProjection;float4 camera;float4 parameters;};
struct Input {float4 position:SV_Position;float2 ndc:TEXCOORD0;};
struct Output {float4 color:SV_Target;float depth:SV_Depth;};
float grid_coverage(float2 coord) {
    float2 width=max(fwidth(coord),1e-5);
    float2 distance=abs(frac(coord-0.5)-0.5)/width;
    return 1-saturate(min(distance.x,distance.y));
}
Output main(Input input) {
    float4 nearPoint=mul(inverseViewProjection,float4(input.ndc,0,1));nearPoint/=nearPoint.w;
    float4 farPoint=mul(inverseViewProjection,float4(input.ndc,1,1));farPoint/=farPoint.w;
    float3 ray=farPoint.xyz-nearPoint.xyz;
    if(abs(ray.y)<1e-6)discard;
    float t=-nearPoint.y/ray.y;if(t<0 || t>1)discard;
    float3 world=nearPoint.xyz+ray*t;
    float4 clip=mul(viewProjection,float4(world,1));float depth=clip.z/clip.w;
    if(depth<0 || depth>1)discard;
    float minor=grid_coverage(world.xz/parameters.x),major=grid_coverage(world.xz/(parameters.x*5));
    float2 derivative=max(fwidth(world.xz),1e-5);
    float xAxis=1-saturate(abs(world.z)/derivative.y),zAxis=1-saturate(abs(world.x)/derivative.x);
    float fade=1-smoothstep(parameters.y*0.3,parameters.y,length(world-camera.xyz));
    Output o;o.color=float4(0.6,0.65,0.72,(minor*0.18+major*0.25)*fade);
    if(xAxis>0.01){o.color.rgb=float3(0.78,0.28,0.25);o.color.a=max(o.color.a,xAxis*0.65*fade);}
    if(zAxis>0.01){o.color.rgb=float3(0.3,0.48,0.83);o.color.a=max(o.color.a,zAxis*0.65*fade);}
    if(o.color.a<0.005)discard;
    o.depth=max(0,depth-0.000002);return o;
})";
struct alignas(16) GridFrame {
    math::Mat4 inverse, viewProjection;
    math::Vec4 camera, parameters;
};
} // namespace
bool EditorViewportGrid::record(render::Renderer &renderer, const render::RenderView &view,
                                render::ResourceHandle color, render::ResourceHandle depth,
                                float spacing, float fadeDistance) {
    if (!pipeline_) {
        render::ShaderDesc vertex;
        vertex.name = "editor_grid_vertex";
        vertex.source = vertexSource;
        vertex.profile = "vs_5_0";
        auto fragment = vertex;
        fragment.name = "editor_grid_fragment";
        fragment.source = fragmentSource;
        fragment.profile = "ps_5_0";
        fragment.stage = render::ShaderStage::Fragment;
        vertex_ = renderer.create_shader(vertex);
        fragment_ = renderer.create_shader(fragment);
        render::PipelineDesc pipeline;
        pipeline.name = "editor_depth_grid";
        pipeline.vertexShader = vertex_.id;
        pipeline.fragmentShader = fragment_.id;
        pipeline.colorFormat = "bgra8";
        pipeline.cullMode = "none";
        pipeline.depthWrite = false;
        pipeline.alphaBlend = true;
        // The view is later sampled by ImGui with alpha blending. Grid
        // coverage affects RGB only; keep the world image fully opaque.
        render::BlendAttachmentState blend;
        blend.enabled = true;
        blend.writeMask = 0x7;
        pipeline.blendAttachments = {blend};
        if (vertex_ && fragment_)
            pipeline_ = renderer.create_pipeline(pipeline);
        if (!pipeline_)
            return false;
    }
    GridFrame frame;
    frame.viewProjection = view.viewProjection;
    if (!math::Inverse(view.viewProjection, frame.inverse))
        return false;
    const auto camera = view.transform.position;
    frame.camera = {camera.x, camera.y, camera.z, 1};
    const float scale = view.projection.orthographic ? view.projection.orthographicSize * 0.1f
                                                     : std::max(1.0f, std::abs(camera.y)) * 0.35f;
    const float base = std::pow(10.0f, std::floor(std::log10(std::max(0.01f, scale))));
    const float normalized = scale / base;
    const float step = spacing > 0 ? spacing
                                   : base * (normalized <= 1   ? 1
                                             : normalized <= 2 ? 2
                                             : normalized <= 5 ? 5
                                                               : 10);
    frame.parameters = {step, fadeDistance > 0 ? fadeDistance : std::max(25.0f, step * 150), 0, 0};
    render::BufferDesc constants;
    constants.size = sizeof(frame);
    constants.stride = 16;
    constants.uniformBuffer = true;
    constants.initialData.resize(sizeof(frame));
    std::memcpy(constants.initialData.data(), &frame, sizeof(frame));
    auto &graph = renderer.graph();
    const auto buffer = graph.create_buffer(constants);
    graph.import_resource(pipeline_, *renderer.resource_description(pipeline_));
    const auto material =
        graph.create_material({"editor_grid",
                               pipeline_,
                               {{"GridFrame", buffer, render::DescriptorType::UniformBuffer, 4}},
                               false});
    const auto pass = graph.add_pass(
        "editor_depth_grid",
        {{color, render::ResourceUsage::ColorAttachment},
         {depth, render::ResourceUsage::DepthStencil},
         {buffer, render::ResourceUsage::UniformBuffer},
         {pipeline_, render::ResourceUsage::ShaderRead},
         {material, render::ResourceUsage::ShaderRead}},
        [material](auto &backend, const auto &) {
            backend.bind_material(material);
            backend.draw_sprite({});
        },
        render::RenderQueue::Graphics, true, false);
    graph.set_depth_clear(pass, false);
    return true;
}
void EditorViewportGrid::release(render::Renderer &renderer) {
    for (const auto resource : {pipeline_, vertex_, fragment_})
        renderer.destroy_resource(resource);
    pipeline_ = vertex_ = fragment_ = {};
}
} // namespace shinkou::editor

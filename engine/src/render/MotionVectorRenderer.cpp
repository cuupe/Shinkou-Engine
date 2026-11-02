#include "shinkou/render/MotionVectorRenderer.h"
#include "BuiltinShaderSources.h"
#include "shinkou/render/PbrRenderPipeline.h"
#include "shinkou/render/Renderer.h"
#include <algorithm>
#include <cstring>
#include <fstream>

namespace shinkou::render {
namespace {
ShaderDesc shader(BackendApi api, ShaderStage stage, const char *variant = nullptr) {
    if (!variant)
        variant = stage == ShaderStage::Vertex ? "MOTION_VERTEX" : "MOTION_FRAGMENT";
    ShaderDesc desc;
    desc.name = variant;
    desc.stage = stage;
    desc.profile = stage == ShaderStage::Vertex ? "vs_5_0" : "ps_5_0";
    if (api == BackendApi::Vulkan) {
        std::ifstream stream(std::string(SHINKOU_BUILTIN_SHADER_OUTPUT_DIR) + "/" + variant +
                                 ".spv",
                             std::ios::binary);
        desc.bytecode.assign(std::istreambuf_iterator<char>(stream), {});
        desc.sourceKind = ShaderSourceKind::SpirV;
    } else
        desc.source =
            std::string("#define SHINKOU_") + variant + " 1\n" + std::string(detail::motionSource);
    return desc;
}
template <class T> ResourceHandle uniform(RenderGraph &graph, const T &value) {
    BufferDesc desc;
    desc.size = sizeof(value);
    desc.stride = 16;
    desc.uniformBuffer = true;
    desc.initialData.resize(sizeof(value));
    std::memcpy(desc.initialData.data(), &value, sizeof(value));
    return graph.create_buffer(desc);
}
} // namespace
bool MotionVectorRenderer::initialize(Renderer &renderer) {
    release(renderer);
    error_.clear();
    vertex_ = renderer.create_shader(shader(renderer.capabilities().api, ShaderStage::Vertex));
    if (!vertex_) {
        error_ = renderer.last_error();
        return false;
    }
    fragment_ = renderer.create_shader(shader(renderer.capabilities().api, ShaderStage::Fragment));
    if (!fragment_) {
        error_ = renderer.last_error();
        release(renderer);
        return false;
    }
    PipelineDesc pipeline;
    pipeline.name = "rigid_motion_vectors";
    pipeline.vertexShader = vertex_.id;
    pipeline.fragmentShader = fragment_.id;
    pipeline.colorFormat = "rgba16f";
    pipeline.cullMode = "none";
    pipeline.depthWrite = false;
    pipeline.vertexLayout.bindings = {{0, sizeof(PbrMeshVertex), false, 1}};
    pipeline.vertexLayout.attributes = {{"POSITION", 0, 0, 0, VertexFormat::Float3, 0}};
    pipeline_ = renderer.create_pipeline(pipeline);
    if (!pipeline_) {
        error_ = renderer.last_error();
        release(renderer);
        return false;
    }
    backgroundVertex_ = renderer.create_shader(
        shader(renderer.capabilities().api, ShaderStage::Vertex, "MOTION_BACKGROUND_VERTEX"));
    backgroundFragment_ =
        backgroundVertex_
            ? renderer.create_shader(shader(renderer.capabilities().api, ShaderStage::Fragment,
                                            "MOTION_BACKGROUND_FRAGMENT"))
            : ResourceHandle{};
    PipelineDesc background;
    background.name = "background_motion_vectors";
    background.vertexShader = backgroundVertex_.id;
    background.fragmentShader = backgroundFragment_.id;
    background.colorFormat = "rgba16f";
    background.depthFormat = "none";
    background.depthTest = background.depthWrite = false;
    background.cullMode = "none";
    backgroundPipeline_ =
        backgroundFragment_ ? renderer.create_pipeline(background) : ResourceHandle{};
    SamplerDesc point;
    point.filter = "nearest";
    point.addressU = point.addressV = point.addressW = "clamp";
    sampler_ = backgroundPipeline_ ? renderer.create_sampler(point) : ResourceHandle{};
    if (!sampler_) {
        error_ = renderer.last_error();
        release(renderer);
        return false;
    }
    return true;
}
bool MotionVectorRenderer::record(Renderer &renderer, RenderGraph &graph, const RenderScene &scene,
                                  const RenderView &currentView,
                                  const math::Mat4 &previousViewProjection,
                                  const PreviousObjectTransforms &previousObjects,
                                  bool historyValid, ResourceHandle target, ResourceHandle depth) {
    error_.clear();
    if (!pipeline_ || !graph.contains_resource(target) || !graph.contains_resource(depth)) {
        error_ = "motion renderer or targets are unavailable";
        return false;
    }
    const auto *pipeline = renderer.resource_description(pipeline_);
    if (!pipeline) {
        error_ = "motion pipeline was released";
        return false;
    }
    graph.import_resource(pipeline_, *pipeline);
    struct Camera {
        math::Mat4 current, previous, inverseCurrent;
        math::Vec4 position, flags;
    } camera{};
    camera.current = currentView.viewProjection;
    camera.previous = previousViewProjection;
    camera.position = {currentView.transform.position.x, currentView.transform.position.y,
                       currentView.transform.position.z, 0};
    camera.flags = {historyValid ? 1.0f : 0.0f, currentView.projection.orthographic ? 1.0f : 0.0f,
                    0, 0};
    if (!math::Inverse(currentView.viewProjection, camera.inverseCurrent)) {
        error_ = "motion camera matrix is singular";
        return false;
    }
    const auto frame = uniform(graph, camera);
    for (auto handle : {backgroundPipeline_, sampler_}) {
        const auto *description = renderer.resource_description(handle);
        if (!description) {
            error_ = "background motion resources were released";
            return false;
        }
        graph.import_resource(handle, *description);
    }
    const auto background =
        graph.create_material({"background_motion",
                               backgroundPipeline_,
                               {{"SceneDepth", depth, DescriptorType::Texture, 0},
                                {"DepthSampler", sampler_, DescriptorType::Sampler, 1},
                                {"MotionCamera", frame, DescriptorType::UniformBuffer, 2}},
                               false});
    const auto backgroundPass = graph.add_pass("motion_vectors_background",
                                               {{target, ResourceUsage::ColorAttachment},
                                                {depth, ResourceUsage::ShaderRead},
                                                {frame, ResourceUsage::UniformBuffer},
                                                {backgroundPipeline_, ResourceUsage::ShaderRead},
                                                {sampler_, ResourceUsage::ShaderRead},
                                                {background, ResourceUsage::ShaderRead}},
                                               {});
    graph.set_command_recorder(backgroundPass, [background](auto &commands, const auto &) {
        commands.bind_material(background);
        commands.draw_sprite({});
    });
    struct Draw {
        MeshDraw mesh;
        ResourceHandle material;
    };
    std::vector<Draw> draws;
    std::vector<ResourceAccess> accesses{{target, ResourceUsage::ColorAttachment},
                                         {depth, ResourceUsage::DepthStencil},
                                         {frame, ResourceUsage::UniformBuffer},
                                         {pipeline_, ResourceUsage::ShaderRead}};
    const auto append = [&](ResourceHandle handle, ResourceUsage usage) {
        if (std::none_of(accesses.begin(), accesses.end(),
                         [&](const auto &value) { return value.resource.id == handle.id; }))
            accesses.push_back({handle, usage});
    };
    for (const auto &mesh : scene.meshes()) {
        graph.import_resource(mesh.vertexBuffer, mesh.vertexDescription);
        graph.import_resource(mesh.indexBuffer, mesh.indexDescription);
        const auto found = previousObjects.find(mesh.entity);
        const bool valid = historyValid && found != previousObjects.end();
        struct Object {
            math::Mat4 current, previous;
            math::Vec4 flags;
        };
        const auto object = uniform(graph, Object{mesh.transform,
                                                  valid ? found->second : mesh.transform,
                                                  {valid ? 1.0f : 0.0f, 0, 0, 0}});
        const auto material =
            graph.create_material({"motion_object",
                                   pipeline_,
                                   {{"MotionCamera", frame, DescriptorType::UniformBuffer, 2},
                                    {"MotionObject", object, DescriptorType::UniformBuffer, 3}},
                                   false});
        append(mesh.vertexBuffer, ResourceUsage::VertexBuffer);
        append(mesh.indexBuffer, ResourceUsage::IndexBuffer);
        append(object, ResourceUsage::UniformBuffer);
        append(material, ResourceUsage::ShaderRead);
        draws.push_back({{mesh.vertexBuffer, mesh.indexBuffer, mesh.indexCount, mesh.transform,
                          mesh.firstIndex, mesh.vertexOffset},
                         material});
    }
    const auto pass = graph.add_pass("motion_vectors_draw", std::move(accesses), {},
                                     RenderQueue::Graphics, true, false);
    graph.set_depth_clear(pass, false);
    graph.set_command_recorder(pass, [draws = std::move(draws)](auto &commands, const auto &) {
        for (const auto &draw : draws) {
            commands.bind_material(draw.material);
            commands.draw_mesh(draw.mesh);
        }
    });
    return true;
}
void MotionVectorRenderer::release(Renderer &renderer) {
    for (const auto handle : {pipeline_, fragment_, vertex_, backgroundPipeline_,
                              backgroundFragment_, backgroundVertex_, sampler_})
        if (handle)
            renderer.destroy_resource(handle);
    pipeline_ = fragment_ = vertex_ = {};
    backgroundPipeline_ = backgroundFragment_ = backgroundVertex_ = sampler_ = {};
}
} // namespace shinkou::render

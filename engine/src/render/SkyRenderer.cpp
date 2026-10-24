#include "shinkou/render/SkyRenderer.h"
#include "shinkou/render/Renderer.h"
#include "BuiltinShaderSources.h"
#include <cmath>
#include <cstring>
#include <fstream>

namespace shinkou::render {
namespace {
ShaderDesc sky_shader(BackendApi api,ShaderStage stage) {
    const auto* variant=stage==ShaderStage::Vertex?"SKY_VERTEX":"SKY_FRAGMENT";
    ShaderDesc shader;shader.name=variant;shader.stage=stage;
    shader.profile=stage==ShaderStage::Vertex?"vs_5_0":"ps_5_0";
    if (api==BackendApi::Vulkan) {
        std::ifstream stream(std::string(SHINKOU_BUILTIN_SHADER_OUTPUT_DIR)+"/"+variant+".spv",std::ios::binary);
        shader.bytecode.assign(std::istreambuf_iterator<char>(stream),{});shader.sourceKind=ShaderSourceKind::SpirV;
    } else shader.source=std::string("#define SHINKOU_")+variant+" 1\n"+std::string(detail::skySource);
    return shader;
}
struct alignas(16) SkyParameters {
    math::Mat4 inverseProjection,cameraOrientation;
    math::Vec4 zenith,horizon,ground,settings,flags;
};
static_assert(sizeof(SkyParameters)==208);
}

bool validate_sky_settings(const SkySettings& settings,std::string& error) {
    error.clear();
    const auto color=[](math::Vec3 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && value.x>=0 && value.y>=0 && value.z>=0;
    };
    if ((settings.environment && settings.environment.kind!=ResourceKind::Texture2D) ||
        !std::isfinite(settings.intensity) || settings.intensity<0 || !std::isfinite(settings.rotation) ||
        !color(settings.zenith) || !color(settings.horizon) || !color(settings.ground)) {
        error="sky requires a 2D environment, finite rotation and non-negative HDR colors/intensity";return false;
    }
    return true;
}
bool SkyRenderer::initialize(Renderer& renderer,std::string colorFormat) {
    release(renderer);error_.clear();
    const auto fail=[&] {error_=renderer.last_error();release(renderer);return false;};
    vertex_=renderer.create_shader(sky_shader(renderer.capabilities().api,ShaderStage::Vertex));if (!vertex_) return fail();
    fragment_=renderer.create_shader(sky_shader(renderer.capabilities().api,ShaderStage::Fragment));if (!fragment_) return fail();
    PipelineDesc pipeline;pipeline.name="hdr_environment_sky"+(colorFormat=="rgba16f"?std::string{}:"_"+colorFormat);
    pipeline.vertexShader=vertex_.id;pipeline.fragmentShader=fragment_.id;pipeline.depthWrite=false;
    pipeline.colorFormat=std::move(colorFormat);pipeline.cullMode="none";
    pipeline_=renderer.create_pipeline(pipeline);if (!pipeline_) return fail();
    SamplerDesc sampler;sampler.addressU="repeat";sampler.addressV=sampler.addressW="clamp";
    sampler_=renderer.create_sampler(sampler);if (!sampler_) return fail();
    TextureDesc fallback;fallback.initialData={255,255,255,255};fallback_=renderer.create_texture(fallback);
    return fallback_?true:fail();
}
bool SkyRenderer::record(Renderer& renderer,RenderGraph& graph,const RenderView& view,
                         const SkySettings& settings,ResourceHandle color,ResourceHandle depth) {
    if (!validate_sky_settings(settings,error_)) return false;
    if (!pipeline_ || !graph.contains_resource(color) || !graph.contains_resource(depth)) {error_="sky renderer or targets unavailable";return false;}
    const auto environment=settings.environment?settings.environment:fallback_;
    const auto* resource=renderer.resource_description(environment);
    const auto* texture=resource?std::get_if<TextureDesc>(resource):nullptr;
    if (!texture || texture->dimension!=TextureDimension::Texture2D || texture->layers!=1 || texture->sampleCount!=1 ||
        (texture->format!="rgba16f" && texture->format!="rgba8" && texture->format!="bgra8")) {
        error_="sky environment must be a single-layer rgba16f/rgba8/bgra8 2D texture";return false;
    }
    SkyParameters values{};
    if (!math::Inverse(view.projectionMatrix,values.inverseProjection) || !math::Inverse(view.viewMatrix,values.cameraOrientation)) {
        error_="sky camera matrices are singular";return false;
    }
    values.zenith={settings.zenith.x,settings.zenith.y,settings.zenith.z,0};
    values.horizon={settings.horizon.x,settings.horizon.y,settings.horizon.z,0};
    values.ground={settings.ground.x,settings.ground.y,settings.ground.z,0};
    values.settings={settings.intensity,settings.rotation,view.projection.orthographic?1.0f:0.0f,settings.environment?1.0f:0.0f};
    values.flags.x=texture->colorSpace=="srgb" || texture->colorSpace=="sRGB"?1.0f:0.0f;
    BufferDesc constants;constants.size=sizeof(values);constants.stride=16;constants.uniformBuffer=true;
    constants.initialData.resize(sizeof(values));std::memcpy(constants.initialData.data(),&values,sizeof(values));
    const auto buffer=graph.create_buffer(constants);
    for (const auto handle:{pipeline_,sampler_,environment}) graph.import_resource(handle,*renderer.resource_description(handle));
    const auto material=graph.create_material({"hdr_environment_sky",pipeline_,{{"Environment",environment,DescriptorType::Texture,0},
        {"EnvironmentSampler",sampler_,DescriptorType::Sampler,1},{"SkyParameters",buffer,DescriptorType::UniformBuffer,4}},false});
    const auto pass=graph.add_pass("sky_draw",{{color,ResourceUsage::ColorAttachment},{depth,ResourceUsage::DepthStencil},
        {buffer,ResourceUsage::UniformBuffer},{environment,ResourceUsage::ShaderRead},{sampler_,ResourceUsage::ShaderRead},
        {pipeline_,ResourceUsage::ShaderRead},{material,ResourceUsage::ShaderRead}},{},RenderQueue::Graphics,true,false);
    graph.set_depth_clear(pass,false);
    graph.set_command_recorder(pass,[material](auto& commands,const auto&){commands.bind_material(material);commands.draw_sprite({});});
    return true;
}
void SkyRenderer::release(Renderer& renderer) {
    for (const auto handle:{pipeline_,fragment_,vertex_,sampler_,fallback_}) if (handle) renderer.destroy_resource(handle);
    pipeline_=fragment_=vertex_=sampler_=fallback_={};
}
}

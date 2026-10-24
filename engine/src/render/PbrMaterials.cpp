#include "shinkou/render/PbrRenderPipeline.h"
#include "shinkou/render/BuiltinShaders.h"
#include "shinkou/render/Renderer.h"
#include <algorithm>
#include <cstring>

namespace shinkou::render {
namespace {
template<class T> BufferDesc uniform_description(const T& data) {
    BufferDesc result;
    result.size=sizeof(T);result.stride=16;result.uniformBuffer=true;
    result.initialData.resize(sizeof(T));std::memcpy(result.initialData.data(),&data,sizeof(T));
    return result;
}
struct alignas(16) MaterialUniform {
    math::Vec4 baseColor{},emissiveMetallic{},surface{},ibl{},flags{};
};
}

bool PbrMaterialLibrary::initialize(Renderer& renderer,std::string colorFormat) {
    release(renderer);error_.clear();colorFormat_=std::move(colorFormat);
    const auto own=[&](ResourceHandle handle){
        if(handle) owned_.push_back(handle);
        else if(error_.empty()) error_=renderer.last_error();
        return handle;
    };
    vertex_=own(renderer.create_shader(builtin_pbr_shader(renderer.capabilities().api,ShaderStage::Vertex)));
    fragment_=own(renderer.create_shader(builtin_pbr_shader(renderer.capabilities().api,ShaderStage::Fragment)));
    sampler_=own(renderer.create_sampler({"linear","repeat","clamp","repeat",1,false}));
    TextureDesc texture;
    texture.initialData={255,255,255,255};white_=own(renderer.create_texture(texture));
    texture.initialData={128,128,255,255};normal_=own(renderer.create_texture(texture));
    texture.initialData={0,0,0,255};black_=own(renderer.create_texture(texture));
    scene_=own(renderer.create_buffer(uniform_description(SceneFrameData{})));
    object_=own(renderer.create_buffer(uniform_description(ObjectFrameData{})));
    shadowFrame_=own(renderer.create_buffer(uniform_description(ShadowFrameData{})));
    const auto shadowFragment=own(renderer.create_shader(builtin_shadow_fragment_shader(renderer.capabilities().api)));
    PipelineDesc shadow;
    shadow.name="pbr_shadow";shadow.vertexShader=vertex_.id;shadow.fragmentShader=shadowFragment.id;
    shadow.vertexLayout.bindings={{0,sizeof(PbrMeshVertex),false,1}};
    shadow.vertexLayout.attributes={{"POSITION",0,0,0,VertexFormat::Float3,0},
        {"TEXCOORD",0,1,0,VertexFormat::Float2,12},{"NORMAL",0,2,0,VertexFormat::Float3,20}};
    shadow.cullMode="none";
    shadowPipeline_=shadowFragment?own(renderer.create_pipeline(shadow)):ResourceHandle{};
    BufferDesc lights;lights.size=lights.stride=64;lights.structuredBuffer=true;lights.initialData.resize(64,0);
    lights_=own(renderer.create_buffer(lights));
    if (!vertex_ || !fragment_ || !sampler_ || !white_ || !normal_ || !black_ || !scene_ || !object_ || !lights_ || !shadowFrame_ || !shadowPipeline_) {
        error_="PBR library initialization failed: "+(error_.empty()?renderer.last_error():error_);release(renderer);return false;
    }
    return true;
}

ResourceHandle PbrMaterialLibrary::create_material(Renderer& renderer,const PbrMaterialDesc& material) {
    error_.clear();
    if (!vertex_ || !fragment_) { error_="PBR library is not initialized";return {}; }
    const auto validation=validate_pbr_material(material);
    if (!validation.valid()) { error_=validation.errors.front();return {}; }
    const auto valid_texture=[&](ResourceHandle handle) {
        if (!handle) return true;
        const auto* description=renderer.resource_description(handle);
        return description && std::holds_alternative<TextureDesc>(*description);
    };
    for (auto texture:{material.baseColorTexture,material.normalTexture,material.metallicRoughnessTexture,
        material.occlusionTexture,material.emissiveTexture,material.ibl.irradianceTexture,material.ibl.prefilteredEnvironment,material.ibl.brdfLut})
        if (!valid_texture(texture)) { error_="PBR material references an unknown texture";return {}; }
    MaterialUniform values;
    values.baseColor=material.baseColorFactor;
    values.emissiveMetallic={material.emissiveFactor.x*material.emissiveStrength,
        material.emissiveFactor.y*material.emissiveStrength,material.emissiveFactor.z*material.emissiveStrength,material.metallic};
    values.surface={material.roughness,material.normalScale,material.occlusionStrength,material.alphaCutoff};
    float maxLod=0;
    if (material.ibl.prefilteredEnvironment) {
        const auto* description=renderer.resource_description(material.ibl.prefilteredEnvironment);
        maxLod=static_cast<float>(std::max(1u,std::get<TextureDesc>(*description).mipLevels)-1);
    }
    values.ibl={material.ibl.intensity,material.ibl.rotation,maxLod,material.ibl.irradianceTexture?1.0f:0.0f};
    float srgb=0;
    if (material.baseColorTexture) {
        const auto& texture=std::get<TextureDesc>(*renderer.resource_description(material.baseColorTexture));
        srgb=texture.colorSpace=="srgb" || texture.colorSpace=="sRGB" ? 1.0f:0.0f;
    }
    values.flags={material.normalTexture?1.0f:0.0f,material.alphaTest?1.0f:0.0f,srgb,0};
    PipelineDesc description;
    description.name="pbr_"+colorFormat_+(material.doubleSided?"_double_sided":"_back_culled");
    description.vertexShader=vertex_.id;description.fragmentShader=fragment_.id;
    description.colorFormat=colorFormat_;description.cullMode=material.doubleSided?"none":"back";
    description.vertexLayout.bindings={{0,sizeof(PbrMeshVertex),false,1}};
    description.vertexLayout.attributes={{"POSITION",0,0,0,VertexFormat::Float3,0},
        {"TEXCOORD",0,1,0,VertexFormat::Float2,12},{"NORMAL",0,2,0,VertexFormat::Float3,20}};
    const auto pipeline=renderer.create_pipeline(description);
    const auto parameters=renderer.create_buffer(uniform_description(values));
    if (!pipeline || !parameters) {
        if (parameters) renderer.destroy_resource(parameters);
        if (pipeline && std::none_of(owned_.begin(),owned_.end(),[&](auto handle){return handle.id==pipeline.id;})) owned_.push_back(pipeline);
        error_=renderer.last_error();return {};
    }
    if (std::none_of(owned_.begin(),owned_.end(),[&](auto handle){return handle.id==pipeline.id;})) owned_.push_back(pipeline);
    owned_.push_back(parameters);
    const auto choose=[](ResourceHandle texture,ResourceHandle fallback){return texture?texture:fallback;};
    const auto result=renderer.create_material({material.name,pipeline,
        {{"BaseColor",choose(material.baseColorTexture,white_),DescriptorType::Texture,0},
         {"MaterialSampler",sampler_,DescriptorType::Sampler,1},
         {"SceneFrame",scene_,DescriptorType::UniformBuffer,2},{"ObjectFrame",object_,DescriptorType::UniformBuffer,3},
         {"MaterialParameters",parameters,DescriptorType::UniformBuffer,4},{"lights",lights_,DescriptorType::StructuredBuffer,5},
         {"NormalMap",choose(material.normalTexture,normal_),DescriptorType::Texture,6},
         {"MetallicRoughness",choose(material.metallicRoughnessTexture,white_),DescriptorType::Texture,7},
         {"Occlusion",choose(material.occlusionTexture,white_),DescriptorType::Texture,8},
         {"Emissive",choose(material.emissiveTexture,white_),DescriptorType::Texture,9},
         {"Irradiance",choose(material.ibl.irradianceTexture,black_),DescriptorType::Texture,10},
         {"PrefilteredEnvironment",choose(material.ibl.prefilteredEnvironment,black_),DescriptorType::Texture,11},
         {"BrdfLut",choose(material.ibl.brdfLut,white_),DescriptorType::Texture,12},
         {"ShadowAtlas",white_,DescriptorType::Texture,14},{"ShadowFrame",shadowFrame_,DescriptorType::UniformBuffer,13}},false});
    if (result) owned_.push_back(result);
    else { renderer.destroy_resource(parameters);owned_.pop_back();error_=renderer.last_error(); }
    return result;
}

void PbrMaterialLibrary::release(Renderer& renderer) {
    // Materials from several calls can reference the same cached pipeline.
    // Destroy by dependency tier rather than relying on creation order.
    for (const auto kind:{ResourceKind::Material,ResourceKind::Pipeline,ResourceKind::Buffer,ResourceKind::Texture2D,ResourceKind::Sampler,ResourceKind::Shader})
        for (auto it=owned_.rbegin();it!=owned_.rend();++it) if (it->kind==kind) renderer.destroy_resource(*it);
    owned_.clear();vertex_=fragment_=sampler_=white_=normal_=black_=scene_=object_=lights_=shadowFrame_=shadowPipeline_={};
}
}

#include "shinkou/render/RenderFeatureGraphBuilder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace shinkou::render {
namespace {
using Stage = RenderFeatureGraphStage;
using Domain = RenderImageDomain;
constexpr auto InvalidPass = static_cast<std::size_t>(-1);

bool writes(ResourceUsage usage) {
    return usage == ResourceUsage::ShaderWrite || usage == ResourceUsage::StorageWrite ||
        (usage >= ResourceUsage::ColorAttachment0 && usage <= ResourceUsage::ColorAttachment7) ||
        usage == ResourceUsage::DepthStencil || usage == ResourceUsage::CopyDestination;
}
std::optional<RenderFeature> feature(Stage stage) {
    switch (stage) {
    case Stage::DepthPrepass: return RenderFeature::DepthPrepass;
    case Stage::ShadowCascades: return RenderFeature::CascadedShadows;
    case Stage::SSAO: return RenderFeature::ScreenSpaceAmbientOcclusion;
    case Stage::SSR: return RenderFeature::ScreenSpaceReflections;
    case Stage::TAA: return RenderFeature::TemporalAntiAliasing;
    case Stage::Bloom: return RenderFeature::Bloom;
    case Stage::Exposure: return RenderFeature::Exposure;
    default: return {};
    }
}
PostProcessStageKind image_kind(Stage stage) {
    switch (stage) {
    case Stage::SSAO: return PostProcessStageKind::SSAO;
    case Stage::SSR: return PostProcessStageKind::SSR;
    case Stage::TAA: return PostProcessStageKind::TAA;
    case Stage::Bloom: return PostProcessStageKind::Bloom;
    case Stage::Exposure: return PostProcessStageKind::Exposure;
    case Stage::ToneMap: return PostProcessStageKind::ToneMap;
    case Stage::MotionBlur: return PostProcessStageKind::MotionBlur;
    default: return PostProcessStageKind::Custom;
    }
}
TextureDesc image(std::uint32_t width,std::uint32_t height,Domain domain,bool compute=false) {
    TextureDesc result;
    result.width=width; result.height=height;
    result.format=domain==Domain::DisplaySRGB ? "rgba8" : "rgba16f";
    result.hdr=domain!=Domain::DisplaySRGB;
    result.renderTarget=!compute; result.storage=compute;
    result.colorSpace=domain==Domain::DisplaySRGB ? "srgb" : "linear";
    return result;
}
bool valid_texture(const TextureDesc& texture) {
    return texture.width && texture.height && texture.width<=16384 && texture.height<=16384 &&
        texture.layers==1 && texture.depth==1 && texture.sampleCount==1 && texture.mipLevels==1;
}
void append_unique(std::vector<std::string>& values,const std::string& value) {
    if (!value.empty() && std::find(values.begin(),values.end(),value)==values.end()) values.push_back(value);
}
PostProcessStageDesc parameters(const RenderFeatureGraphDesc& desc,const RenderPipelineStage& node) {
    PostProcessStageDesc result;
    result.kind=image_kind(node.stage);
    const PostProcessStageDesc* match=nullptr;
    for (const auto& candidate:desc.postProcessStages)
        if (candidate.kind==result.kind && candidate.name==node.name) match=&candidate;
    if (!match) for (const auto& candidate:desc.postProcessStages) if (candidate.kind==result.kind) {
        if (match) throw std::runtime_error("ambiguous parameters for stage '"+node.name+"'; name each instance");
        match=&candidate;
    }
    if (match) result=*match;
    result.name=node.name;
    return result;
}
struct NamedResource {
    ResourceHandle handle;
    ResourceDesc description;
    Domain domain{Domain::Data};
    bool available{false};
    std::string producer;
};

RenderFeatureGraphPlan describe(RenderGraph& graph,const RenderFeatureGraphDesc& desc) {
    RenderFeatureGraphPlan plan;
    if (!validate_sky_settings(desc.sky,plan.error)) throw std::runtime_error(plan.error);
    if (!desc.width || !desc.height || desc.width>16384 || desc.height>16384 || !desc.maxLights ||
        !std::isfinite(desc.deltaSeconds) || desc.deltaSeconds<0)
        throw std::runtime_error("feature graph requires valid dimensions, light capacity and frame time");
    if (desc.depthTarget && desc.depthTarget.kind!=ResourceKind::DepthStencil)
        throw std::runtime_error("depth target must be a depth-stencil resource");
    if (desc.sceneColor && (!desc.depthTarget || desc.sceneColor.kind!=ResourceKind::Texture2D))
        throw std::runtime_error("external scene color requires an explicit depth target");
    if (desc.presentTarget && desc.presentTarget.kind!=ResourceKind::Texture2D)
        throw std::runtime_error("present target must be a color texture");
    if (desc.shadows.enabled && (!desc.shadows.atlasSize || desc.shadows.cascadeCount<1 || desc.shadows.cascadeCount>8 ||
        !std::isfinite(desc.shadows.maxDistance) || desc.shadows.maxDistance<=0 || !std::isfinite(desc.shadows.bias) || desc.shadows.bias<0))
        throw std::runtime_error("invalid shadow settings");
    std::set<std::pair<PostProcessStageKind,std::string>> configurations;
    for (const auto& config:desc.postProcessStages) {
        std::string failure;
        if (!validate_post_process_parameters(config,failure)) throw std::runtime_error(failure);
        if (!configurations.emplace(config.kind,config.name).second)
            throw std::runtime_error("duplicate post-process configuration");
    }
    auto stages=desc.stages.empty()?RenderFeatureGraphBuilder::default_stages():desc.stages;
    std::set<std::string> names;
    const std::set<std::string> reserved{"$color","depth","hdr_color","shadow_atlas","light_buffer",
        "cluster_buffer","cluster_light_indices","taa_history","exposure_history"};
    for (const auto& node:stages) {
        if (node.name.empty() || reserved.count(node.name) || !names.insert(node.name).second ||
            static_cast<unsigned>(node.stage)>static_cast<unsigned>(Stage::Sky))
            throw std::runtime_error("pipeline stage names must be valid, unique and not reserved");
    }
    auto requests=desc.featureRequests;
    if (requests.empty()) for (unsigned index=0;index<=static_cast<unsigned>(Stage::Present);++index)
        if (const auto requested=feature(static_cast<Stage>(index))) requests.push_back({*requested,false,true});
    if (!desc.stages.empty()) for (auto& request:requests) {
        const bool present=std::any_of(stages.begin(),stages.end(),[&](const auto& node) {
            return node.enabled && feature(node.stage)==request.feature;
        });
        // Depth is also produced by an opaque/external scene; capability
        // dependencies do not force an extra depth prepass into that path.
        if (!present && request.feature!=RenderFeature::DepthPrepass) {
            if (request.required && request.enabled) throw std::runtime_error("required feature omitted from pipeline");
            request.enabled=false;
        }
    }
    plan.featurePlan=build_render_feature_plan(desc.capabilities,requests);
    if (!plan.featurePlan.valid) throw std::runtime_error(plan.featurePlan.error);
    LightingPipeline lighting;
    lighting.description()=desc.lighting;
    lighting.description().binning.viewportWidth=desc.width;
    lighting.description().binning.viewportHeight=desc.height;
    plan.lightingPlan=lighting.build_plan(desc.view,desc.lights);
    if (!plan.lightingPlan.valid) throw std::runtime_error(plan.lightingPlan.error);
    const bool externalScene=desc.sceneColor && !desc.recordSceneColor;
    const auto active=[&](const RenderPipelineStage& node) {
        if (!node.enabled) return false;
        if (node.stage==Stage::Sky && !desc.sky.enabled) return false;
        if (node.stage==Stage::DepthPrepass || node.stage==Stage::Opaque) {if (externalScene) return false;}
        if (node.stage==Stage::ShadowCascades && !desc.shadows.enabled) return false;
        if (node.stage==Stage::ClusterCulling && desc.lighting.binning.mode!=LightBinningMode::Clusters) return false;
        if (const auto requested=feature(node.stage)) {
            const auto* decision=plan.featurePlan.decision(*requested);
            return decision && decision->enabled;
        }
        return true;
    };
    const auto selected=[&](Stage kind) {
        return std::any_of(stages.begin(),stages.end(),[&](const auto& node){return node.stage==kind && active(node);});
    };
    std::map<std::string,NamedResource> resources;
    const auto resource=[&](std::string name,ResourceDesc description,ResourceKind kind,Domain domain,
                            ResourceHandle supplied=ResourceHandle{},bool available=false) {
        if (resources.count(name)) throw std::runtime_error("pipeline resource name collision: "+name);
        ResourceHandle handle=supplied;
        if (handle) graph.import_resource(handle,description);
        else if (const auto* texture=std::get_if<TextureDesc>(&description))
            handle=kind==ResourceKind::DepthStencil?graph.create_depth_stencil(*texture):graph.create_texture(*texture);
        else handle=graph.create_buffer(std::get<BufferDesc>(description));
        if (!handle || !graph.contains_resource(handle)) throw std::runtime_error("cannot create/import resource '"+name+"'");
        resources[name]={handle,description,domain,available,{}};
        plan.resources.push_back({name,handle,std::move(description),bool(supplied)});
        return name;
    };
    auto depth=desc.depthDescription;
    depth.width=desc.width; depth.height=desc.height; depth.depthStencil=true;
    if (!valid_texture(depth)) throw std::runtime_error("invalid scene depth layout");
    resource("depth",depth,ResourceKind::DepthStencil,Domain::Data,desc.depthTarget,externalScene);
    auto hdr=desc.sceneColor?desc.sceneColorDescription:image(desc.width,desc.height,Domain::LinearHDR);
    if (!valid_texture(hdr) || hdr.width!=desc.width || hdr.height!=desc.height || hdr.format!="rgba16f")
        throw std::runtime_error("invalid scene color layout");
    resource("hdr_color",hdr,ResourceKind::Texture2D,Domain::LinearHDR,desc.sceneColor,externalScene);
    for (const auto& external:desc.resources) {
        if (!external.handle || external.name.empty() || reserved.count(external.name) || names.count(external.name) || resources.count(external.name))
            throw std::runtime_error("external pipeline resources require valid handles and unique names");
        resource(external.name,external.description,external.handle.kind,external.domain,external.handle,true);
    }
    if (selected(Stage::ShadowCascades)) {
        auto shadow=desc.shadowTarget?desc.shadowDescription:depth;
        if (!desc.shadowTarget) {shadow.width=shadow.height=desc.shadows.atlasSize;shadow.format="d32f";}
        shadow.depthStencil=true;
        if (!valid_texture(shadow)) throw std::runtime_error("invalid shadow atlas layout");
        resource("shadow_atlas",shadow,ResourceKind::DepthStencil,Domain::Data,desc.shadowTarget);
    }
    if (selected(Stage::ClusterCulling)) {
        const auto bins=std::max<std::size_t>(1,plan.lightingPlan.binning.bins.size());
        const auto capacity=desc.lighting.binning.maxLightsPerBin;
        if (!capacity || bins>(256u<<20)/sizeof(std::uint32_t)/capacity ||
            desc.lights.size()>desc.maxLights || desc.maxLights>(256u<<20)/sizeof(GpuLightData))
            throw std::runtime_error("cluster graph buffers exceed capacity/memory budget");
        BufferDesc lights;
        lights.size=desc.maxLights*sizeof(GpuLightData);lights.stride=sizeof(GpuLightData);lights.structuredBuffer=true;
        lights.initialData.resize(lights.size,0);
        const auto values=make_gpu_light_data(desc.lights);
        if (!values.empty()) std::memcpy(lights.initialData.data(),values.data(),values.size()*sizeof(GpuLightData));
        resource("light_buffer",lights,ResourceKind::Buffer,Domain::Data,{},true);
        BufferDesc clusters;clusters.size=bins*8;clusters.stride=8;clusters.structuredBuffer=clusters.storageBuffer=true;
        resource("cluster_buffer",clusters,ResourceKind::Buffer,Domain::Data);
        clusters.size=bins*capacity*4;clusters.stride=4;
        resource("cluster_light_indices",clusters,ResourceKind::Buffer,Domain::Data);
    }
    std::string currentColor="hdr_color";
    std::set<std::string> completed;
    if (externalScene) completed={"depth_prepass","opaque"};
    std::map<Stage,std::string> latest;
    const auto kind_before=[&](Stage kind)->std::string {
        const auto found=latest.find(kind);
        if (found==latest.end()) throw std::runtime_error(std::string("stage requires an earlier enabled stage: ")+to_string(kind));
        return found->second;
    };
    bool presented=false;
    for (const auto& node:stages) {
        RenderFeatureGraphPass pass;
        pass.stage=node.stage;pass.name=node.name;pass.enabled=active(node);
        pass.queue=RenderQueue::Graphics;pass.description.stage=node.stage;pass.description.name=node.name;
        pass.description.enabled=pass.enabled;
        if (!pass.enabled) {pass.reason="disabled or supplied by caller";plan.passes.push_back(std::move(pass));continue;}
        if (presented) throw std::runtime_error("Present must be the last enabled pipeline stage");
        pass.description.dependsOn=node.dependsOn;
        pass.postProcess=parameters(desc,node);
        const bool imageStage=node.stage==Stage::SSAO || node.stage==Stage::SSR || node.stage==Stage::TAA ||
            node.stage==Stage::Bloom || node.stage==Stage::Exposure || node.stage==Stage::MotionBlur;
        const bool compute=(imageStage && desc.preferComputePostProcess && desc.capabilities.supportsCompute) ||
            (node.stage==Stage::Custom && node.queue==RenderQueue::Compute);
        if (imageStage) {
            pass.queue=compute?RenderQueue::Compute:RenderQueue::Graphics;
            pass.fallback=desc.preferComputePostProcess && !compute;
        }
        if (node.stage==Stage::Custom) pass.queue=node.queue;
        const auto access=[&](const std::string& reference,ResourceUsage usage,std::string alias={}) {
            const auto key=reference=="$color"?currentColor:reference;
            const auto found=resources.find(key);
            if (found==resources.end() || (!writes(usage) && !found->second.available))
                throw std::runtime_error("stage '"+node.name+"' references a missing or not-yet-produced resource: "+key);
            const auto& value=found->second;
            if (!writes(usage)) append_unique(pass.description.dependsOn,value.producer);
            if (std::any_of(pass.accesses.begin(),pass.accesses.end(),[&](const auto& item){return item.resource.id==value.handle.id;}))
                throw std::runtime_error("stage '"+node.name+"' aliases input/output or binds a resource more than once: "+key);
            pass.accesses.push_back({value.handle,usage});
            RenderFeatureGraphResourceContract contract{alias.empty()?key:alias,value.handle,value.description,usage};
            (writes(usage)?pass.description.outputs:pass.description.inputs).push_back(std::move(contract));
        };
        const auto produce=[&](const std::string& key) {resources.at(key).available=true;resources.at(key).producer=node.name;};
        const auto source=[&]() {
            const auto key=node.source=="$color"?currentColor:node.source;
            access(key,ResourceUsage::ShaderRead);
            pass.description.inputDomain=resources.at(key).domain;
            return key;
        };
        const auto color_output=[&](Domain domain,bool replace,std::uint32_t width=0,std::uint32_t height=0,const char* format=nullptr) {
            auto layout=image(width?width:desc.width,height?height:desc.height,domain,compute);
            if (format) {layout.format=format;layout.hdr=layout.format=="rgba16f";}
            auto output=node.output.value_or(layout);
            if (!valid_texture(output) || (!compute && !output.renderTarget) || (compute && !output.storage))
                throw std::runtime_error("invalid image output for stage '"+node.name+"'");
            if (domain==Domain::LinearHDR && output.format!="rgba16f")
                throw std::runtime_error("linear HDR output requires a floating-point color format: "+node.name);
            resource(node.name,output,ResourceKind::Texture2D,domain);
            access(node.name,compute?ResourceUsage::StorageWrite:ResourceUsage::ColorAttachment);
            produce(node.name);pass.description.outputDomain=domain;
            pass.postProcess.output=output;
            if (replace) currentColor=node.name;
        };
        switch (node.stage) {
        case Stage::DepthPrepass:
            pass.description.outputDomain=Domain::Data;
            access("depth",ResourceUsage::DepthStencil);produce("depth");resources[node.name]=resources.at("depth");break;
        case Stage::ShadowCascades:
            pass.description.outputDomain=Domain::Data;
            access("shadow_atlas",ResourceUsage::DepthStencil);produce("shadow_atlas");resources[node.name]=resources.at("shadow_atlas");break;
        case Stage::ClusterCulling:
            pass.description.outputDomain=Domain::Data;
            pass.fallback=!desc.capabilities.supportsCompute;
            pass.queue=pass.fallback?RenderQueue::Graphics:RenderQueue::Compute;
            if (selected(Stage::DepthPrepass)) access("depth",ResourceUsage::ShaderRead);
            access("light_buffer",ResourceUsage::ShaderRead);
            access("cluster_buffer",ResourceUsage::StorageWrite);produce("cluster_buffer");
            access("cluster_light_indices",ResourceUsage::StorageWrite);produce("cluster_light_indices");break;
        case Stage::Opaque:
            if (selected(Stage::DepthPrepass)) kind_before(Stage::DepthPrepass);
            if (selected(Stage::ShadowCascades)) access("shadow_atlas",ResourceUsage::ShaderRead);
            if (selected(Stage::ClusterCulling)) {
                access("light_buffer",ResourceUsage::ShaderRead);access("cluster_buffer",ResourceUsage::ShaderRead);
                access("cluster_light_indices",ResourceUsage::ShaderRead);
            }
            access("depth",ResourceUsage::DepthStencil);produce("depth");
            access("hdr_color",ResourceUsage::ColorAttachment);produce("hdr_color");
            currentColor="hdr_color";resources[node.name]=resources.at(currentColor);break;
        case Stage::MotionVectors: {
            pass.description.outputDomain=Domain::Data;
            access("depth",ResourceUsage::DepthStencil);
            if (!resources.at("depth").available) throw std::runtime_error("motion vectors require produced scene depth");
            append_unique(pass.description.dependsOn,resources.at("depth").producer);
            auto output=desc.motionVectorTarget?desc.motionVectorDescription:image(desc.width,desc.height,Domain::Data);
            resource(node.name,output,ResourceKind::Texture2D,Domain::Data,desc.motionVectorTarget);
            access(node.name,ResourceUsage::ColorAttachment);produce(node.name);break;
        }
        case Stage::Sky: {
            const auto key=node.source=="$color"?currentColor:node.source;
            const auto found=resources.find(key);
            if (found==resources.end() || !found->second.available || found->second.domain!=Domain::LinearHDR || !resources.at("depth").available)
                throw std::runtime_error("sky requires earlier HDR scene color and depth");
            const auto* texture=std::get_if<TextureDesc>(&found->second.description);
            if (!texture || !texture->renderTarget || texture->width!=desc.width || texture->height!=desc.height)
                throw std::runtime_error("sky target must match scene depth dimensions");
            append_unique(pass.description.dependsOn,found->second.producer);
            append_unique(pass.description.dependsOn,resources.at("depth").producer);
            access(key,ResourceUsage::ColorAttachment);produce(key);
            access("depth",ResourceUsage::DepthStencil);
            resources[node.name]=resources.at(key);currentColor=key;break;
        }
        case Stage::SSAO:
            access("depth",ResourceUsage::ShaderRead);color_output(Domain::Data,false,0,0,"rgba8");break;
        case Stage::SSR: case Stage::TAA: case Stage::Bloom: case Stage::Exposure: case Stage::MotionBlur: {
            source();
            if (pass.description.inputDomain!=Domain::LinearHDR)
                throw std::runtime_error("HDR stage '"+node.name+"' cannot consume display-encoded color");
            if (node.stage==Stage::SSR || node.stage==Stage::TAA || node.stage==Stage::MotionBlur)
                access("depth",ResourceUsage::ShaderRead,"depth");
            if ((node.stage==Stage::MotionBlur || node.stage==Stage::TAA) && selected(Stage::MotionVectors))
                access(kind_before(Stage::MotionVectors),ResourceUsage::ShaderRead,"motion_vectors");
            else if (node.stage==Stage::MotionBlur && std::none_of(node.inputs.begin(),node.inputs.end(),[](const auto& input){return input.name=="motion_vectors";}))
                throw std::runtime_error("motion blur requires an earlier motion-vector stage or a named external motion input");
            const bool temporal=node.stage==Stage::TAA;
            const auto* exposure=std::get_if<ExposureStageParameters>(&pass.postProcess.parameters);
            const bool temporalExposure=node.stage==Stage::Exposure && exposure && exposure->adaptationRate>0;
            color_output(node.stage==Stage::Exposure?Domain::Data:Domain::LinearHDR,
                node.stage==Stage::SSR || temporal || node.stage==Stage::MotionBlur,
                node.stage==Stage::Exposure?1u:0u,node.stage==Stage::Exposure?1u:0u);
            if (temporal || temporalExposure) {
                const auto& config=pass.postProcess;
                const auto width=temporal?desc.width:1u,height=temporal?desc.height:1u;
                if (!config.historyTexture || !config.historyOutput || config.historyTexture.id==config.historyOutput.id ||
                    !valid_texture(config.historyDescription) || !valid_texture(config.historyOutputDescription) ||
                    config.historyDescription.width!=width || config.historyDescription.height!=height ||
                    config.historyOutputDescription.width!=width || config.historyOutputDescription.height!=height ||
                    config.historyDescription.format!="rgba16f" || config.historyOutputDescription.format!="rgba16f" ||
                    !config.historyOutputDescription.renderTarget)
                    throw std::runtime_error("stage '"+node.name+"' requires distinct persistent rgba16f history targets matching its output");
                const auto history=node.name+"_history";
                resource(history,config.historyDescription,ResourceKind::Texture2D,Domain::Data,config.historyTexture,true);
                access(history,ResourceUsage::ShaderRead,temporal?"taa_history":"exposure_history");
                const auto output=node.name+"_history_output";
                resource(output,config.historyOutputDescription,ResourceKind::Texture2D,Domain::Data,config.historyOutput);
                access(output,compute?ResourceUsage::StorageWrite:ResourceUsage::ColorAttachment1);produce(output);
            }
            break;
        }
        case Stage::ToneMap:
            source();
            if (pass.description.inputDomain!=Domain::LinearHDR) throw std::runtime_error("tone mapping requires linear HDR color");
            for (const auto kind:{Stage::Bloom,Stage::Exposure,Stage::SSAO}) if (selected(kind))
                access(kind_before(kind),ResourceUsage::ShaderRead,to_string(kind));
            color_output(Domain::DisplaySRGB,true);break;
        case Stage::Present: {
            source();
            pass.description.outputDomain=pass.description.inputDomain;
            if (pass.description.inputDomain!=Domain::DisplaySRGB &&
                !(desc.presentTarget && desc.presentDescription.format=="rgba16f"))
                throw std::runtime_error("presentation requires display color or an explicit HDR texture target");
            if (desc.presentTarget) {
                if (!valid_texture(desc.presentDescription) || desc.presentDescription.width!=desc.width || desc.presentDescription.height!=desc.height)
                    throw std::runtime_error("invalid presentation target layout");
                resource("present",desc.presentDescription,ResourceKind::Texture2D,pass.description.inputDomain,desc.presentTarget);
                access("present",ResourceUsage::ColorAttachment);produce("present");
            }
            presented=true;break;
        }
        case Stage::Custom: {
            Domain input=Domain::LinearHDR;
            if (!node.source.empty()) {source();input=pass.description.inputDomain;}
            if (node.producesImage) color_output(node.outputDomain==Domain::Inherit?input:node.outputDomain,node.replacesColor);
            break;
        }
        }
        for (const auto& input:node.inputs) access(input.resource,input.usage,input.name);
        for (const auto& dependency:pass.postProcess.dependsOn) append_unique(pass.description.dependsOn,dependency);
        for (const auto& dependency:pass.description.dependsOn) if (!completed.count(dependency))
            throw std::runtime_error("stage '"+node.name+"' requires an earlier enabled stage: "+dependency);
        pass.postProcess.compute=compute;pass.description.postProcess=pass.postProcess;
        pass.description.queue=pass.queue;pass.description.fallback=pass.fallback;
        pass.reason=pass.fallback?"compute unavailable; graphics fallback selected":"explicit pipeline stage";
        pass.description.reason=pass.reason;
        if (pass.fallback) plan.diagnostics.push_back(pass.name+": "+pass.reason);
        completed.insert(node.name);latest[node.stage]=node.name;
        plan.passes.push_back(std::move(pass));
    }
    plan.valid=true;
    return plan;
}
}

const char* to_string(RenderFeatureGraphStage stage) noexcept {
    switch (stage) {
    case Stage::DepthPrepass: return "depth_prepass";
    case Stage::ShadowCascades: return "shadow_cascades";
    case Stage::ClusterCulling: return "cluster_culling";
    case Stage::Opaque: return "opaque";
    case Stage::SSAO: return "ssao";
    case Stage::SSR: return "ssr";
    case Stage::TAA: return "taa";
    case Stage::Bloom: return "bloom";
    case Stage::Exposure: return "exposure";
    case Stage::ToneMap: return "tone_map";
    case Stage::Present: return "present";
    case Stage::Custom: return "custom";
    case Stage::MotionVectors: return "motion_vectors";
    case Stage::MotionBlur: return "motion_blur";
    case Stage::Sky: return "sky";
    }
    return "unknown";
}
std::vector<RenderPipelineStage> RenderFeatureGraphBuilder::default_stages() {
    std::vector<RenderPipelineStage> result;
    for (unsigned i=0;i<=static_cast<unsigned>(Stage::Present);++i) {
        RenderPipelineStage node;
        node.stage=static_cast<Stage>(i);node.name=to_string(node.stage);
        result.push_back(std::move(node));
    }
    return result;
}
RenderFeatureGraphPlan RenderFeatureGraphBuilder::plan(RenderGraph& graph,const RenderFeatureGraphDesc& desc) {
    auto staged=graph;
    try {
        auto result=describe(staged,desc);
        if (result.valid) graph=std::move(staged);
        return result;
    } catch (const std::exception& failure) {RenderFeatureGraphPlan result;result.error=failure.what();return result;}
}
RenderFeatureGraphPlan RenderFeatureGraphBuilder::build(RenderGraph& graph,const RenderFeatureGraphDesc& desc) {
    auto staged=graph;
    auto result=plan(staged,desc);
    if (!result.valid) return result;
    const auto fail=[&](std::string error) {result.valid=false;result.error=std::move(error);return result;};
    std::set<std::pair<Stage,std::string>> executorNames;
    for (const auto& executor:desc.executors)
        if ((!executor.prepare && !executor.record) || !executorNames.emplace(executor.stage,executor.name).second)
            return fail("render feature executors must be non-empty and unique by kind/name");
    std::vector<std::size_t> previous;
    for (auto& pass:result.passes) {
        if (!pass.enabled) continue;
        auto executor=std::find_if(desc.executors.begin(),desc.executors.end(),[&](const auto& candidate) {
            return candidate.stage==pass.stage && candidate.name==pass.name;
        });
        if (executor==desc.executors.end()) executor=std::find_if(desc.executors.begin(),desc.executors.end(),[&](const auto& candidate) {
            return candidate.stage==pass.stage && candidate.name.empty();
        });
        if (executor==desc.executors.end()) return fail("render stage has no executable implementation: "+pass.name);
        const auto begin=staged.passes().size();
        RenderFeatureGraphCommands commands;
        try {
            if (executor->prepare) commands=executor->prepare(staged,pass.description);
            else commands.record=executor->record;
        } catch (const std::exception& failure) {return fail("stage '"+pass.name+"' preparation failed: "+failure.what());}
        if (!commands.error.empty()) return fail("stage '"+pass.name+"' preparation failed: "+commands.error);
        commands.accesses.insert(commands.accesses.end(),executor->accesses.begin(),executor->accesses.end());
        for (const auto& access:commands.accesses) {
            if (std::any_of(pass.accesses.begin(),pass.accesses.end(),[&](const auto& existing){return existing.resource.id==access.resource.id;}))
                return fail("executor duplicates a stage resource access: "+pass.name);
            pass.accesses.push_back(access);
            const auto resource=std::find_if(staged.resources().begin(),staged.resources().end(),[&](const auto& item){return item.handle.id==access.resource.id;});
            if (resource==staged.resources().end()) return fail("executor accesses an unregistered resource: "+pass.name);
            RenderFeatureGraphResourceContract contract{"executor_"+std::to_string(access.resource.id),access.resource,resource->description,access.usage};
            (writes(access.usage)?pass.description.outputs:pass.description.inputs).push_back(std::move(contract));
        }
        const auto intermediateEnd=staged.passes().size();
        for (const auto index:commands.expandedPasses) if (index<begin || index>=intermediateEnd)
            return fail("expanded pass belongs to a different stage: "+pass.name);
        if (commands.record || commands.recordCommands) {
            pass.passIndex=staged.add_pass(pass.name,pass.accesses,std::move(commands.record),pass.queue,true,pass.stage==Stage::Sky?false:commands.clearAttachments);
            if (commands.recordCommands) staged.set_command_recorder(pass.passIndex,std::move(commands.recordCommands));
            if ((pass.stage==Stage::Opaque && std::any_of(result.passes.begin(),result.passes.end(),[](const auto& node){return node.enabled && node.stage==Stage::DepthPrepass;})) ||
                pass.stage==Stage::MotionVectors || pass.stage==Stage::Sky) staged.set_depth_clear(pass.passIndex,false);
        }
        if (begin==staged.passes().size()) return fail("stage has no executable passes: "+pass.name);
        for (auto i=begin;i<staged.passes().size();++i) pass.passIndices.push_back(i);
        if (pass.passIndex==InvalidPass) pass.passIndex=pass.passIndices.back();
        // Preserve authored stage order even for side effects, separate queues,
        // expanded subgraphs and passes without shared resource hazards.
        for (const auto from:previous) for (const auto to:pass.passIndices) staged.add_dependency(from,to);
        previous=pass.passIndices;
        for (const auto& output:pass.description.outputs) {
            const bool produced=std::any_of(pass.passIndices.begin(),pass.passIndices.end(),[&](auto index) {
                const auto& accesses=staged.passes()[index].accesses;
                return std::any_of(accesses.begin(),accesses.end(),[&](const auto& access) {
                    return access.resource.id==output.handle.id && writes(access.usage);
                });
            });
            if (!produced) return fail("expanded stage did not produce declared output: "+pass.name+"/"+output.name);
        }
    }
    if (!staged.compile(&result.error)) {result.valid=false;return result;}
    result.executable=true;graph=std::move(staged);
    return result;
}
}

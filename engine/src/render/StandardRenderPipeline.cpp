#include "shinkou/render/StandardRenderPipeline.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace shinkou::render {
namespace {
using Stage=RenderFeatureGraphStage;
float halton(std::uint32_t index,std::uint32_t base) {
    float value=0,fraction=1;
    while (index) {fraction/=base;value+=fraction*(index%base);index/=base;}
    return value;
}
bool contains(const std::vector<RenderPipelineStage>& stages,Stage kind) {
    return std::any_of(stages.begin(),stages.end(),[&](const auto& node){return node.stage==kind && node.enabled;});
}
PostProcessStageDesc& configure(std::vector<PostProcessStageDesc>& stages,PostProcessStageKind kind) {
    auto found=std::find_if(stages.begin(),stages.end(),[&](const auto& value){return value.kind==kind;});
    if (found==stages.end()) {stages.emplace_back();found=std::prev(stages.end());found->kind=kind;}
    return *found;
}
}

bool StandardRenderPipeline::initialize(Renderer& renderer,const StandardRenderPipelineDesc& description) {
    release(renderer);error_.clear();description_=description;
    if (!validate_sky_settings(description.sky,error_)) return false;
    const auto& shadows=description.shadows;
    if (shadows.enabled && (!shadows.atlasSize || shadows.cascadeCount<1 || shadows.cascadeCount>8 ||
        !std::isfinite(shadows.maxDistance) || shadows.maxDistance<=0 || !std::isfinite(shadows.bias) || shadows.bias<0)) {
        error_="invalid standard pipeline shadow settings";return false;
    }
    if (!materials_.initialize(renderer) || !images_.initialize(renderer,description.presentationFormat) || !motion_.initialize(renderer) || !sky_.initialize(renderer)) {
        error_=materials_.last_error()+images_.last_error()+motion_.last_error()+sky_.last_error();release(renderer);return false;
    }
    if (!exposureHistory_.resize(renderer,1,1) || !resize(renderer,description.width,description.height)) {
        if (error_.empty()) error_=renderer.last_error();
        release(renderer);return false;
    }
    initialized_=true;return true;
}

bool StandardRenderPipeline::resize(Renderer& renderer,std::uint32_t width,std::uint32_t height) {
    error_.clear();
    if (pending_) {error_="complete the pending frame before resizing";return false;}
    if (!width || !height || width>16384 || height>16384) {error_="invalid standard pipeline viewport";return false;}
    if (color_ && width==description_.width && height==description_.height) return true;
    TextureDesc color;
    color.width=width;color.height=height;color.format="rgba16f";color.hdr=true;color.renderTarget=true;
    TextureDesc depth;depth.width=width;depth.height=height;depth.format="d24s8";
    TextureDesc shadow=depth;shadow.width=shadow.height=description_.shadows.atlasSize;
    const auto nextColor=renderer.create_texture(color);
    const auto nextDepth=renderer.create_depth_stencil(depth);
    const auto nextShadow=description_.shadows.enabled?renderer.create_depth_stencil(shadow):ResourceHandle{};
    const auto nextMotion=renderer.create_texture(color);
    if (!nextColor || !nextDepth || !nextMotion || (description_.shadows.enabled && !nextShadow) || !history_.resize(renderer,width,height)) {
        error_="standard pipeline target allocation failed: "+renderer.last_error();
        for (const auto handle:{nextColor,nextDepth,nextShadow,nextMotion}) if (handle) renderer.destroy_resource(handle);
        return false;
    }
    for (const auto handle:{color_,depth_,shadow_,motionVectors_}) if (handle) renderer.destroy_resource(handle);
    color_=nextColor;depth_=nextDepth;shadow_=nextShadow;motionVectors_=nextMotion;
    colorDescription_=motionDescription_=color;depthDescription_=depth;shadowDescription_=shadow;
    description_.width=width;description_.height=height;invalidate_history();
    return true;
}
ResourceHandle StandardRenderPipeline::create_material(Renderer& renderer,const PbrMaterialDesc& material) {
    const auto handle=materials_.create_material(renderer,material);error_=materials_.last_error();return handle;
}

bool StandardRenderPipeline::build(Renderer& renderer,const RenderScene& scene,ResourceHandle presentationTarget,float deltaSeconds) {
    error_.clear();
    if (!initialized_ || pending_) {
        error_=pending_?"complete the previous standard pipeline frame before building another":"standard pipeline is not initialized";
        return false;
    }
    TextureDesc presentation;
    if (presentationTarget) {
        const auto* resource=renderer.resource_description(presentationTarget);
        const auto* texture=resource?std::get_if<TextureDesc>(resource):nullptr;
        if (!texture || !texture->renderTarget || texture->width!=description_.width ||
            texture->height!=description_.height || texture->format!=description_.presentationFormat) {
            error_="standard presentation target does not match its viewport/format";return false;
        }
        presentation=*texture;
    }
    auto stages=description_.stages;
    if (stages.empty()) {
        stages=RenderFeatureGraphBuilder::default_stages();
        for (auto& node:stages) {
            if (node.stage==Stage::DepthPrepass) node.enabled=description_.depthPrepass;
            if (node.stage==Stage::ClusterCulling) node.enabled=false;
        }
        if (description_.sky.enabled) {
            const auto opaque=std::find_if(stages.begin(),stages.end(),[](const auto& node){return node.stage==Stage::Opaque;});
            RenderPipelineStage sky;sky.name="sky";sky.stage=Stage::Sky;stages.insert(std::next(opaque),std::move(sky));
        }
        const auto position=std::find_if(stages.begin(),stages.end(),[](const auto& node){return node.stage==Stage::SSAO;});
        if (description_.motionVectors && (description_.taa || description_.motionBlur)) {
            RenderPipelineStage motion;motion.name="motion_vectors";motion.stage=Stage::MotionVectors;
            stages.insert(position,std::move(motion));
        }
        if (description_.motionBlur) {
            const auto bloom=std::find_if(stages.begin(),stages.end(),[](const auto& node){return node.stage==Stage::Bloom;});
            RenderPipelineStage blur;blur.name="motion_blur";blur.stage=Stage::MotionBlur;
            stages.insert(bloom,std::move(blur));
        }
    }
    const bool temporalAA=description_.taa && contains(stages,Stage::TAA);
    RenderView view=scene.view();
    if (description_.temporalJitter && temporalAA) {
        const auto sample=(jitterFrame_%8)+1;
        const auto x=(halton(sample,2)-0.5f)*2/description_.width;
        const auto y=-(halton(sample,3)-0.5f)*2/description_.height;
        for (unsigned column=0;column<4;++column) {
            view.projectionMatrix.m[column*4]+=x*view.projectionMatrix.m[column*4+3];
            view.projectionMatrix.m[column*4+1]+=y*view.projectionMatrix.m[column*4+3];
        }
        view.viewProjection=math::Multiply(view.projectionMatrix,view.viewMatrix);
    }
    RenderFeatureGraphDesc frame;
    frame.capabilities=renderer.capabilities();frame.width=description_.width;frame.height=description_.height;
    frame.sceneColor=color_;frame.sceneColorDescription=colorDescription_;frame.recordSceneColor=true;
    frame.depthTarget=depth_;frame.depthDescription=depthDescription_;
    frame.shadowTarget=shadow_;frame.shadowDescription=shadowDescription_;frame.shadows=description_.shadows;
    frame.motionVectorTarget=motionVectors_;frame.motionVectorDescription=motionDescription_;
    frame.presentTarget=presentationTarget;frame.presentDescription=presentation;
    frame.view=view;frame.lights=scene.lights();frame.previousViewProjection=previousViewProjection_;
    frame.historyValid=history_.valid();frame.exposureHistoryValid=exposureHistory_.valid();frame.deltaSeconds=deltaSeconds;
    frame.lighting.binning.mode=LightBinningMode::Tiles;frame.stages=std::move(stages);
    frame.sky=description_.sky;
    frame.featureRequests={{RenderFeature::DepthPrepass,false,true},{RenderFeature::CascadedShadows,false,description_.shadows.enabled},
        {RenderFeature::ScreenSpaceAmbientOcclusion,false,description_.ssao},
        {RenderFeature::ScreenSpaceReflections,false,description_.ssr},{RenderFeature::TemporalAntiAliasing,false,description_.taa},
        {RenderFeature::Bloom,false,description_.bloom},{RenderFeature::Exposure,false,description_.exposure}};
    frame.postProcessStages=description_.imageParameters;
    // The standard owner has one TAA and exposure history. Additional named
    // temporal instances can be composed directly through the graph builder.
    if (temporalAA) {
        if (std::count_if(frame.stages.begin(),frame.stages.end(),[](const auto& node){return node.enabled && node.stage==Stage::TAA;})>1) {
            error_="standard pipeline owns one TAA history; provide independent histories through the feature graph for multiple instances";return false;
        }
        history_.configure(configure(frame.postProcessStages,PostProcessStageKind::TAA));
    }
    if (description_.exposure && contains(frame.stages,Stage::Exposure)) {
        if (std::count_if(frame.stages.begin(),frame.stages.end(),[](const auto& node){return node.enabled && node.stage==Stage::Exposure;})>1) {
            error_="standard pipeline owns one exposure history; provide independent histories through the feature graph for multiple instances";return false;
        }
        auto& config=configure(frame.postProcessStages,PostProcessStageKind::Exposure);
        if (std::holds_alternative<std::monostate>(config.parameters)) config.parameters=description_.exposureParameters;
        const auto* params=std::get_if<ExposureStageParameters>(&config.parameters);
        if (params && params->adaptationRate>0) exposureHistory_.configure(config,PostProcessStageKind::Exposure);
    }
    frame.executors=description_.executors;
    const auto scene_executor=[&](Stage kind,auto prepare) {
        if (std::any_of(frame.executors.begin(),frame.executors.end(),[&](const auto& executor){return executor.stage==kind && executor.name.empty();})) return;
        RenderFeatureGraphExecutor executor;executor.stage=kind;executor.prepare=prepare;frame.executors.push_back(std::move(executor));
    };
    const bool preserveDepth=std::any_of(frame.stages.begin(),frame.stages.end(),[](const auto& node){return node.enabled && node.stage==Stage::DepthPrepass;});
    scene_executor(Stage::DepthPrepass,[&,view](RenderGraph& graph,const auto&) {
        forward_.build_depth(renderer,graph,scene,view,materials_.shadow_pipeline(),depth_,depthDescription_);
        return RenderFeatureGraphCommands{};
    });
    scene_executor(Stage::ShadowCascades,[&](RenderGraph& graph,const auto&) {
        const auto before=graph.passes().size();
        forward_.build_shadows(renderer,scene,description_.shadows,materials_.shadow_pipeline(),shadow_,shadowDescription_,&graph);
        if (before==graph.passes().size()) graph.add_pass("empty_shadow_clear",{{shadow_,ResourceUsage::DepthStencil}},{});
        return RenderFeatureGraphCommands{};
    });
    scene_executor(Stage::Opaque,[&,view,preserveDepth](RenderGraph& graph,const auto&) {
        forward_.build(renderer,scene,color_,colorDescription_,depth_,depthDescription_,{},&graph,&view,preserveDepth);
        return RenderFeatureGraphCommands{};
    });
    scene_executor(Stage::MotionVectors,[&,view](RenderGraph& graph,const auto&) {
        RenderFeatureGraphCommands result;
        if (!motion_.record(renderer,graph,scene,view,previousViewProjection_,previousObjects_,history_.valid(),motionVectors_,depth_))
            result.error=motion_.last_error();
        return result;
    });
    scene_executor(Stage::Sky,[&,view](RenderGraph& graph,const auto& contract) {
        RenderFeatureGraphCommands result;
        const auto color=std::find_if(contract.outputs.begin(),contract.outputs.end(),[](const auto& output){return output.usage==ResourceUsage::ColorAttachment;});
        if (color==contract.outputs.end() || !sky_.record(renderer,graph,view,description_.sky,color->handle,depth_)) result.error=sky_.last_error();
        return result;
    });
    if (!images_.install(frame)) {error_="standard image programs are unavailable";return false;}
    plan_=RenderFeatureGraphBuilder::build(renderer.graph(),frame);
    if (!plan_.valid) {error_=plan_.error;return false;}
    pendingViewProjection_=view.viewProjection;pendingObjects_.clear();
    for (const auto& mesh:scene.meshes()) pendingObjects_[mesh.entity]=mesh.transform;
    pending_=true;return true;
}
void StandardRenderPipeline::complete_frame(bool submitted) {
    if (!pending_) return;
    if (submitted) {
        history_.advance();exposureHistory_.advance();previousViewProjection_=pendingViewProjection_;
        previousObjects_=std::move(pendingObjects_);++jitterFrame_;
    } else invalidate_history();
    pending_=false;
}
void StandardRenderPipeline::invalidate_history() {
    history_.invalidate();exposureHistory_.invalidate();previousObjects_.clear();pendingObjects_.clear();jitterFrame_=0;
}
bool StandardRenderPipeline::set_stages(std::vector<RenderPipelineStage> stages,std::vector<RenderFeatureGraphExecutor> executors) {
    if (pending_) {error_="complete the pending frame before reconfiguring stages";return false;}
    std::set<std::string> names;
    for (const auto& stage:stages) if (stage.name.empty() || !names.insert(stage.name).second) {
        error_="stage names must be non-empty and unique";return false;
    }
    description_.stages=std::move(stages);description_.executors=std::move(executors);invalidate_history();error_.clear();return true;
}
bool StandardRenderPipeline::set_sky(const SkySettings& settings) {
    if (pending_) {error_="complete the pending frame before reconfiguring sky";return false;}
    if (!validate_sky_settings(settings,error_)) return false;
    description_.sky=settings;invalidate_history();return true;
}
void StandardRenderPipeline::release(Renderer& renderer) {
    forward_.release(renderer);motion_.release(renderer);sky_.release(renderer);history_.release(renderer);exposureHistory_.release(renderer);
    images_.release(renderer);materials_.release(renderer);
    for (const auto handle:{color_,depth_,shadow_,motionVectors_}) if (handle) renderer.destroy_resource(handle);
    color_=depth_=shadow_=motionVectors_={};pending_=initialized_=false;plan_={};
    previousViewProjection_=pendingViewProjection_=math::Mat4::Identity();invalidate_history();
}
}

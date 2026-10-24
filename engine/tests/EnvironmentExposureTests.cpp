#include "shinkou/render/StandardRenderPipeline.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace shinkou::render;
namespace {
void require(bool value,const std::string& error) {if (!value) throw std::runtime_error(error);}
template<class T> std::vector<std::uint8_t> bytes(const T& value) {
    const auto* first=reinterpret_cast<const std::uint8_t*>(&value);return {first,first+sizeof(value)};
}
float half_float(std::uint16_t value) {
    const auto exponent=(value>>10)&31u,fraction=value&1023u;
    const auto magnitude=exponent==0?std::ldexp(float(fraction),-24):std::ldexp(1+fraction/1024.0f,int(exponent)-15);
    return value&0x8000u?-magnitude:magnitude;
}
std::array<float,4> pixel(Renderer& renderer,ResourceHandle texture,unsigned x,unsigned y) {
    TextureReadbackRequest request{texture};request.x=x;request.y=y;request.width=request.height=1;
    TextureReadback result;require(renderer.read_texture(request,result),renderer.last_error());
    std::uint16_t values[4];std::memcpy(values,result.data.data(),8);
    return {half_float(values[0]),half_float(values[1]),half_float(values[2]),half_float(values[3])};
}
std::vector<std::uint8_t> pixels(unsigned width,unsigned height,std::uint16_t radiance) {
    std::vector<std::uint8_t> result(width*height*8);
    const std::uint16_t value[]{radiance,radiance,radiance,0x3c00};
    for (unsigned i=0;i<width*height;++i) std::memcpy(result.data()+i*8,value,8);
    return result;
}
void execute(Renderer& renderer) {
    std::string error;require(renderer.execute_graph(renderer.graph(),&error),error);
}

void sky_and_background_motion(Renderer& renderer) {
    using namespace shinkou;
    StandardRenderPipeline pipeline;StandardRenderPipelineDesc settings;
    settings.width=settings.height=64;settings.presentationFormat="rgba8";settings.shadows.enabled=false;
    settings.ssao=settings.ssr=settings.bloom=settings.exposure=false;settings.temporalJitter=false;settings.sky.enabled=true;
    settings.sky.zenith=settings.sky.horizon=settings.sky.ground={2,.5f,.1f};
    require(pipeline.initialize(renderer,settings),pipeline.last_error());
    PbrMaterialDesc description;description.doubleSided=true;description.baseColorFactor={0,0,0,1};description.emissiveFactor={4,0,0};
    const auto material=pipeline.create_material(renderer,description);require(bool(material),pipeline.last_error());
    const PbrMeshVertex vertices[]{{{-1,-1,0},{0,1},{0,0,-1}},{{-1,1,0},{0,0},{0,0,-1}},
        {{1,1,0},{1,0},{0,0,-1}},{{1,-1,0},{1,1},{0,0,-1}}};
    const std::uint32_t indices[]{0,1,2,0,2,3};
    const auto vb=renderer.create_buffer({sizeof(vertices),sizeof(PbrMeshVertex),true,false,bytes(vertices)});
    const auto ib=renderer.create_buffer({sizeof(indices),4,false,true,bytes(indices)});
    TextureDesc output;output.width=output.height=64;output.renderTarget=true;const auto target=renderer.create_texture(output);
    World world;const auto camera=world.ecs().create(),object=world.ecs().create();
    TransformComponent cameraTransform;cameraTransform.local.position={0,0,-5};
    world.ecs().emplace<TransformComponent>(camera,cameraTransform);
    CameraComponent projection;projection.verticalFieldOfView=math::Pi/2;projection.nearPlane=.1f;projection.farPlane=20;
    world.ecs().emplace<CameraComponent>(camera,projection);world.ecs().emplace<TransformComponent>(object);
    world.ecs().emplace<MeshRendererComponent>(object,MeshRendererComponent{vb,ib,material,6});
    const bool read=renderer.capabilities().supportsTextureReadback;
    unsigned frameNumber=0;RenderView previousView;
    const auto frame=[&] {
        RenderScene scene;scene.extract(world,renderer,1);renderer.begin_graph();
        require(pipeline.build(renderer,scene,target),pipeline.last_error());
        if (frameNumber++==0) require(!pipeline.set_sky(settings.sky),"pending sky configuration was overwritten");
        const auto& plan=pipeline.last_plan();
        const auto sky=std::find_if(plan.passes.begin(),plan.passes.end(),[](const auto& pass){return pass.stage==RenderFeatureGraphStage::Sky;});
        require(sky!=plan.passes.end() && sky->enabled && sky->passIndices.size()==1,"sky did not become a native stage");
        const auto& native=renderer.graph().passes()[sky->passIndex];
        require(!native.clearAttachments && native.clearDepthAttachments==false,"sky clears scene color/depth");
        execute(renderer);pipeline.complete_frame(true);return scene.view();
    };
    previousView=frame();
    if (read) {
        const auto sky=pixel(renderer,pipeline.scene_color(),2,2),geometry=pixel(renderer,pipeline.scene_color(),32,32);
        require(std::abs(sky[0]-2)<.01f && std::abs(sky[1]-.5f)<.01f,"sky did not preserve linear HDR colors");
        require(geometry[0]>3.9f && geometry[1]<.01f,"sky overwrote opaque geometry");
        require(pixel(renderer,pipeline.motion_vectors(),10,10)[3]<.5f,"first sky frame accepted history");
    }
    world.ecs().get<TransformComponent>(camera).local.position.x=.25f;previousView=frame();
    if (read) {
        const auto velocity=pixel(renderer,pipeline.motion_vectors(),10,10);
        require(std::abs(velocity[0])<.0002f && std::abs(velocity[1])<.0002f && velocity[3]>.5f,"translation introduced sky parallax");
        require(pixel(renderer,pipeline.motion_vectors(),30,32)[0]>.02f,"background fill erased geometry motion");
    }
    world.ecs().get<TransformComponent>(camera).local.rotation=math::FromAxisAngle({0,1,0},.12f);
    const auto current=frame();
    if (read) {
        math::Mat4 inverse;require(math::Inverse(current.viewProjection,inverse),"inverse test camera");
        const float u=10.5f/64,v=10.5f/64;
        const auto point=math::TransformVector4(inverse,{2*u-1,1-2*v,1,1});const auto position=current.transform.position;
        const auto old=math::TransformVector4(previousView.viewProjection,{point.x-position.x*point.w,point.y-position.y*point.w,point.z-position.z*point.w,0});
        const float expectedX=old.x/old.w*.5f+.5f-u,expectedY=-old.y/old.w*.5f+.5f-v;
        const auto velocity=pixel(renderer,pipeline.motion_vectors(),10,10);
        require(std::abs(velocity[0]-expectedX)<.001f && std::abs(velocity[1]-expectedY)<.001f && velocity[3]>.5f,
            "background rotation reprojection differs from the projected direction");
    }
    pipeline.invalidate_history();frame();if (read) require(pixel(renderer,pipeline.motion_vectors(),10,10)[3]<.5f,"sky camera cut retained history");
    auto invalid=settings.sky;invalid.intensity=-1;require(!pipeline.set_sky(invalid),"negative sky intensity accepted");

    // Orthographic rays are parallel; a longitudinal HDR ramp gives exact
    // bilinear values for environment rotation and confirms wrap sampling.
    world.ecs().get<MeshRendererComponent>(object).visible=false;
    world.ecs().get<CameraComponent>(camera).orthographic=true;
    world.ecs().get<TransformComponent>(camera).local.rotation={};
    TextureDesc environment;environment.width=8;environment.height=2;environment.format="rgba16f";environment.hdr=true;
    environment.initialData.resize(8*2*8);const std::uint16_t ramp[]{0x3c00,0x4000,0x4200,0x4400,0x4500,0x4600,0x4700,0x4800};
    for (unsigned y=0;y<2;++y) for (unsigned x=0;x<8;++x) {
        const std::uint16_t value[]{ramp[x],0,0,0x3c00};std::memcpy(environment.initialData.data()+(y*8+x)*8,value,8);
    }
    const auto map=renderer.create_texture(environment);require(bool(map),renderer.last_error());
    settings.sky.environment=map;require(pipeline.set_sky(settings.sky),pipeline.last_error());frame();
    if (read) require(std::abs(pixel(renderer,pipeline.scene_color(),32,32)[0]-6.5f)<.02f,"equirectangular sky lookup is incorrect");
    settings.sky.rotation=math::Pi;require(pipeline.set_sky(settings.sky),pipeline.last_error());frame();
    if (read) require(std::abs(pixel(renderer,pipeline.scene_color(),32,32)[0]-2.5f)<.02f,"environment rotation did not affect sky");
    pipeline.release(renderer);require(renderer.resource_description(map)!=nullptr,"sky destroyed a caller-owned environment");
    for (const auto handle:{vb,ib,target,map}) renderer.destroy_resource(handle);
}

float histogram_reference(const std::vector<std::uint8_t>& source,const ExposureStageParameters& parameters) {
    std::array<unsigned,256> counts{};
    for (std::size_t offset=0;offset<source.size();offset+=8) {
        std::uint16_t values[4];std::memcpy(values,source.data()+offset,8);
        const auto luminance=half_float(values[0])*.2126f+half_float(values[1])*.7152f+half_float(values[2])*.0722f;
        const auto normalized=std::clamp((std::log2(std::max(luminance,1e-6f))-parameters.minLogLuminance)/
            (parameters.maxLogLuminance-parameters.minLogLuminance),0.0f,1.0f);
        ++counts[luminance<=1e-6f?0:1+unsigned(normalized*254)];
    }
    unsigned total=0;for (unsigned i=1;i<256;++i) total+=counts[i];if (!total) return parameters.maxExposure;
    const float lower=total*parameters.lowPercentile,upper=total*parameters.highPercentile;
    float prefix=0,sum=0,weight=0;
    for (unsigned i=1;i<256;++i) {
        const float retained=std::max(0.0f,std::min(prefix+counts[i],upper)-std::max(prefix,lower));
        const float value=parameters.minLogLuminance+(parameters.maxLogLuminance-parameters.minLogLuminance)*std::min(1.0f,(i-1+.5f)/254);
        sum+=value*retained;weight+=retained;prefix+=counts[i];
    }
    return std::clamp(parameters.middleGray/std::exp2(sum/std::max(weight,1e-5f))*parameters.exposure*std::exp2(parameters.compensation),parameters.minExposure,parameters.maxExposure);
}
void histogram_exposure(Renderer& renderer) {
    using Stage=RenderFeatureGraphStage;
    BuiltinRenderFeatures images;require(images.initialize(renderer,"rgba16f"),images.last_error());
    TextureDesc hdr;hdr.width=31;hdr.height=17;hdr.format="rgba16f";hdr.hdr=true;hdr.renderTarget=true;
    hdr.initialData=pixels(31,17,0x3c00);const auto source=renderer.create_texture(hdr);
    auto output=hdr;output.initialData.clear();const auto target=renderer.create_texture(output);
    TextureDesc z;z.width=31;z.height=17;z.format="d24s8";const auto depth=renderer.create_depth_stencil(z);
    TemporalRenderHistory history;require(history.resize(renderer,1,1),"exposure histories");
    float adapted=0,previousExposure=0;
    for (unsigned frame=0;frame<7;++frame) {
        auto data=pixels(31,17,frame==2?0:frame>=5?0x4400:0x3c00);
        if (frame==1) {const std::uint16_t bright[]{0x6c00,0x6c00,0x6c00,0x3c00};std::memcpy(data.data(),bright,8);}
        if (frame==3) std::fill(data.begin(),data.begin()+(31*17/2)*8,0);
        TextureUpdate update;update.texture=source;update.width=31;update.height=17;update.rowPitch=31*8;update.data=data;
        require(renderer.update_texture(update),renderer.last_error());renderer.begin_graph();
        RenderFeatureGraphDesc desc;desc.capabilities=renderer.capabilities();desc.width=31;desc.height=17;
        desc.lighting.binning.mode=LightBinningMode::Tiles;desc.sceneColor=source;desc.sceneColorDescription=hdr;
        desc.depthTarget=depth;desc.depthDescription=z;desc.presentTarget=target;desc.presentDescription=output;
        desc.stages={{"meter",Stage::Exposure},{"copy",Stage::Present}};desc.stages.back().source="meter";
        ExposureStageParameters parameters;parameters.metering=ExposureMetering::Histogram;
        parameters.minLogLuminance=-8;parameters.maxLogLuminance=8;
        if (frame>=4) parameters.adaptationRate=2;
        PostProcessStageDesc config;config.kind=PostProcessStageKind::Exposure;config.parameters=parameters;
        if (frame>=4) history.configure(config,PostProcessStageKind::Exposure);
        if (frame==6) {history.invalidate();parameters.compensation=1;config.parameters=parameters;}
        desc.exposureHistoryValid=history.valid();desc.deltaSeconds=.5f;desc.postProcessStages={config};
        require(images.install(desc),"histogram install");const auto plan=RenderFeatureGraphBuilder::build(renderer.graph(),desc);require(plan.valid,plan.error);
        require(plan.passes.front().passIndices.size()==3,"histogram did not expand into clear/build/resolve");
        const auto& graph=renderer.graph();
        require(std::any_of(graph.transitions().begin(),graph.transitions().end(),[](const auto& transition){return transition.before==ResourceUsage::StorageWrite && transition.after==ResourceUsage::StorageWrite;}),
            "histogram clear/build lost their write-after-write memory dependency");
        const auto before=renderer.stats().dispatchCalls;
        try {execute(renderer);} catch (const std::exception& failure) {throw std::runtime_error("frame "+std::to_string(frame)+": "+failure.what());}
        require(renderer.stats().dispatchCalls-before==2,"histogram compute commands were not dispatched");
        const auto reference=histogram_reference(data,parameters);
        adapted=frame==5?previousExposure+(reference-previousExposure)*(1-std::exp(-1.0f)):reference;
        if (renderer.capabilities().supportsTextureReadback) {
            const auto actual=pixel(renderer,target,15,8)[0];
            require(std::abs(actual-adapted)<std::max(.001f,adapted*.002f),"histogram exposure CPU reference mismatch: "+std::to_string(frame)+" actual "+std::to_string(actual)+" expected "+std::to_string(adapted));
        }
        previousExposure=adapted;if (frame>=4) history.advance();
    }
    history.release(renderer);images.release(renderer);
    for (const auto handle:{source,target,depth}) renderer.destroy_resource(handle);
}
}
int main(int argc,char** argv) {
    try {
        const std::string requested=argc>1?argv[1]:"dx11";
        const auto api=requested=="dx12"?BackendApi::DirectX12:requested=="vulkan"?BackendApi::Vulkan:BackendApi::DirectX11;
        Renderer renderer(api,{nullptr,64,64,false,true});
        if (!renderer.initialize() || !renderer.capabilities().deviceReady) return 77;
        try {sky_and_background_motion(renderer);} catch (const std::exception& failure) {throw std::runtime_error(std::string("sky: ")+failure.what());}
        try {histogram_exposure(renderer);} catch (const std::exception& failure) {throw std::runtime_error(std::string("histogram: ")+failure.what());}
        std::cout<<"Verified HDR sky, background motion and GPU histogram exposure on "<<requested<<'\n';return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

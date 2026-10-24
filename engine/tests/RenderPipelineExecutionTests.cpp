#include "shinkou/render/BuiltinRenderFeatures.h"
#include "shinkou/render/PbrRenderPipeline.h"
#include "shinkou/render/StandardRenderPipeline.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <map>
#include <atomic>
#include <thread>
#include <fstream>

using namespace shinkou::render;
namespace {
void require(bool value,const std::string& message) { if (!value) throw std::runtime_error(message); }
std::vector<std::uint8_t> bytes(const void* data,std::size_t size) {
    std::vector<std::uint8_t> result(size); std::memcpy(result.data(),data,size); return result;
}
float half_float(std::uint16_t value) {
    const auto exponent=(value>>10)&31u, fraction=value&1023u;
    const float magnitude=exponent==0?std::ldexp(static_cast<float>(fraction),-24):
        exponent==31?std::numeric_limits<float>::infinity():std::ldexp(1.0f+fraction/1024.0f,static_cast<int>(exponent)-15);
    return value&0x8000u?-magnitude:magnitude;
}
void configuration_tests() {
    PipelineDesc pipeline;
    std::string error;
    require(validate_pipeline_state(pipeline,error),error);
    const auto legacy=resolve_vertex_layout(PipelineDesc{"legacy",1,2,0,true,true,false,true});
    require(legacy.bindings.front().stride==12,"legacy vertex layout changed");
    pipeline.vertexLayout.bindings={{0,12,false,1},{1,16,true,1}};
    pipeline.vertexLayout.attributes={{"POSITION",0,0,0,VertexFormat::Float3,0},{"COLOR",0,1,1,VertexFormat::Float4,0}};
    require(validate_pipeline_state(pipeline,error),error);
    const auto original=pipeline_state_key(pipeline);
    pipeline.depthStencilState=DepthStencilState{};
    pipeline.depthStencilState->depthCompare=CompareOp::Greater;
    require(original!=pipeline_state_key(pipeline),"depth state is absent from the pipeline cache key");
    pipeline.vertexLayout.attributes[1].offset=8;
    require(!validate_pipeline_state(pipeline,error),"out-of-stride vertex attribute accepted");
    pipeline.vertexLayout.attributes[1].offset=0;
    pipeline.vertexLayout.attributes[1].location=0;
    require(!validate_pipeline_state(pipeline,error),"duplicate vertex location accepted");
    Renderer nullRenderer;
    require(nullRenderer.initialize(),"null renderer init");
    RenderGraph invalid;
    invalid.add_pass("unknown",{{{999,ResourceKind::Texture2D},ResourceUsage::ShaderRead}},[](auto&,const auto&){});
    require(!nullRenderer.execute_graph(invalid),"execute_graph swallowed an error when no output pointer was passed");
    RenderGraph constants;
    BufferDesc first;first.size=16;first.stride=16;first.uniformBuffer=true;first.initialData.resize(16,1);
    auto second=first;second.initialData.assign(16,2);
    const auto a=constants.create_buffer(first),b=constants.create_buffer(second);
    constants.add_pass("first_constants",{{a,ResourceUsage::UniformBuffer}},[](auto&,const auto&){});
    constants.add_pass("second_constants",{{b,ResourceUsage::UniformBuffer}},[](auto&,const auto&){});
    require(constants.compile(&error),error);
    require(constants.physical_resource(a).id!=constants.physical_resource(b).id,"distinct initialized constant buffers were aliased");

    RenderGraph recorded;
    const auto submissionThread=std::this_thread::get_id();
    std::atomic<unsigned> workers{0};
    for (unsigned i=0;i<2;++i) {
        const auto index=recorded.add_pass("record_"+std::to_string(i),std::vector<ResourceAccess>{},
            [](auto&,const auto&) { throw std::runtime_error("legacy callback ran for a recorded pass"); });
        recorded.set_command_recorder(index,[&](auto& commands,const auto&) {
            if (std::this_thread::get_id()!=submissionThread) ++workers;
            commands.draw_sprite({});
        });
    }
    const auto beforeDraws=nullRenderer.stats().drawCalls;
    require(nullRenderer.execute_graph(recorded,&error),error);
    require(workers==2 && recorded.diagnostics().recordedPassCount==2 &&
        nullRenderer.stats().drawCalls-beforeDraws==2,"worker command packets did not reach backend submission");
    RenderGraph rejected;
    const auto rejectedPass=rejected.add_pass("invalid_commands",std::vector<ResourceAccess>{},{});
    rejected.set_command_recorder(rejectedPass,[](auto& commands,const auto&) {commands.bind_material({123,ResourceKind::Material});});
    const auto beforeFrames=nullRenderer.stats().frames;
    require(!nullRenderer.execute_graph(rejected,&error) && error.find("undeclared")!=std::string::npos &&
        nullRenderer.stats().frames==beforeFrames,"bad command recording reached native frame execution");
    rejected.set_command_recorder(rejectedPass,[](auto&,const auto&) {throw std::runtime_error("recording failure");});
    require(!nullRenderer.execute_graph(rejected,&error) && error.find("recording failure")!=std::string::npos,
        "worker recording failure was swallowed");
}

void image_pipeline(Renderer& renderer) {
    BuiltinRenderFeatures images;
    require(images.initialize(renderer),images.last_error());
    PbrMaterialLibrary materials;
    require(materials.initialize(renderer),materials.last_error());
    PbrMaterialDesc emissive;
    emissive.name="image_pipeline_hdr_seed";emissive.baseColorFactor={0,0,0,1};
    emissive.emissiveFactor={4,0.5f,0.1f};emissive.doubleSided=true;
    const auto seedMaterial=materials.create_material(renderer,emissive);
    require(static_cast<bool>(seedMaterial),materials.last_error());
    const PbrMeshVertex vertices[]{{{-1,-1,0},{0,1},{0,0,-1}},{{-1,3,0},{0,-1},{0,0,-1}},{{3,-1,0},{2,1},{0,0,-1}}};
    const std::uint32_t indices[]{0,1,2};
    const auto vb=renderer.create_buffer({sizeof(vertices),sizeof(PbrMeshVertex),true,false,bytes(vertices,sizeof(vertices))});
    const auto ib=renderer.create_buffer({sizeof(indices),4,false,true,bytes(indices,sizeof(indices))});
    std::vector<Renderer::PersistentResource> dependencies;
    for (const auto handle:{seedMaterial,vb,ib}) dependencies.push_back({handle,*renderer.resource_description(handle)});
    const auto nativeSeed=std::get<MaterialDesc>(*renderer.resource_description(seedMaterial));
    for (const auto& binding:nativeSeed.bindings)
        if (std::none_of(dependencies.begin(),dependencies.end(),[&](const auto& resource){return resource.handle.id==binding.resource.id;}))
            dependencies.push_back({binding.resource,*renderer.resource_description(binding.resource)});
    const bool canRead=renderer.capabilities().supportsTextureReadback;
    TextureDesc targetDescription;
    targetDescription.width=targetDescription.height=32; targetDescription.renderTarget=true;
    const auto target=renderer.create_texture(targetDescription);
    TemporalRenderHistory history;
    require(history.resize(renderer,32,32),"history allocation");
    const auto initialHistory=history.input();
    for (unsigned frame=0;frame<3;++frame) {
        renderer.begin_graph();
        auto& graph=renderer.graph();
        RenderFeatureGraphDesc description;
        description.capabilities=renderer.capabilities(); description.width=description.height=32;
        description.lighting.binning.mode=LightBinningMode::Tiles;
        description.presentTarget=target; description.presentDescription=targetDescription;
        description.historyValid=history.valid();
        description.featureRequests={
            {RenderFeature::DepthPrepass,false,true},{RenderFeature::CascadedShadows,false,false},
            {RenderFeature::ScreenSpaceAmbientOcclusion,false,true},{RenderFeature::ScreenSpaceReflections,false,true},
            {RenderFeature::TemporalAntiAliasing,false,true},{RenderFeature::Bloom,false,true},{RenderFeature::Exposure,false,true}};
        PostProcessStageDesc taa; history.configure(taa); description.postProcessStages.push_back(taa);
        // Depth dependencies are explicit: this pass clears depth to the far
        // plane, representing a scene with a constant HDR background.
        description.executors.push_back({RenderFeatureGraphStage::DepthPrepass,
            [](auto& backend,const auto&) { backend.set_viewport(0,0,32,32); },{},{}});
        RenderFeatureGraphExecutor opaque;
        opaque.stage=RenderFeatureGraphStage::Opaque;
        opaque.prepare=[seedMaterial,vb,ib,dependencies](auto& graph,const auto&) {
            RenderFeatureGraphCommands result;
            for (const auto& resource:dependencies) {
                graph.import_resource(resource.handle,resource.description);
                const auto* buffer=std::get_if<BufferDesc>(&resource.description);
                auto usage=buffer && buffer->uniformBuffer?ResourceUsage::UniformBuffer:ResourceUsage::ShaderRead;
                if (resource.handle.id==vb.id) usage=ResourceUsage::VertexBuffer;
                if (resource.handle.id==ib.id) usage=ResourceUsage::IndexBuffer;
                result.accesses.push_back({resource.handle,usage});
            }
            result.record=[=](auto& backend,const auto&) { backend.bind_material(seedMaterial); backend.draw_mesh({vb,ib,3}); };
            return result;
        };
        description.executors.push_back(opaque);
        require(images.install(description),"image programs install");
        std::map<RenderFeatureGraphStage,std::array<float,4>> observed;
        for (auto& executor:description.executors) {
            if (!canRead || !executor.prepare || executor.stage==RenderFeatureGraphStage::Opaque) continue;
            const auto prepare=executor.prepare;
            executor.prepare=[prepare,&observed,stage=executor.stage](auto& graph,const auto& contract) {
                auto commands=prepare(graph,contract);
                if (contract.outputs.empty() || !commands.record) return commands;
                const auto output=contract.outputs.front();
                const auto record=commands.record;
                // The observer reads native results during the pass. Keep
                // this diagnostic callback on the submission thread.
                commands.recordCommands={};
                commands.record=[record,output,&observed,stage](auto& backend,const auto& context) {
                    record(backend,context);
                    TextureReadback readback;
                    TextureReadbackRequest request{output.handle}; request.width=request.height=1;
                    require(backend.read_texture(request,readback),"stage readback failed: "+backend.last_error());
                    std::array<float,4> pixel{};
                    if (std::get<TextureDesc>(output.description).format=="rgba16f") {
                        std::uint16_t halves[4];std::memcpy(halves,readback.data.data(),8);
                        for (unsigned i=0;i<4;++i) pixel[i]=half_float(halves[i]);
                    } else for (unsigned i=0;i<4;++i) pixel[i]=readback.data[i]/255.0f;
                    observed[stage]=pixel;
                };
                return commands;
            };
        }
        const auto plan=RenderFeatureGraphBuilder::build(graph,description);
        require(plan.valid && plan.executable,plan.error);
        require(plan.featurePlan.decision(RenderFeature::TemporalAntiAliasing)->enabled,"TAA was silently disabled");
        std::string error;
        if (!renderer.execute_graph(graph,&error)) throw std::runtime_error("native image pipeline: "+error);
        if (canRead) {
        TextureReadback readback;
        require(renderer.read_texture({target},readback),"pixel readback: "+renderer.last_error());
        const auto offset=(readback.height/2)*readback.rowPitch+(readback.width/2)*4;
        const auto red=readback.data[offset],green=readback.data[offset+1],blue=readback.data[offset+2];
        require(std::abs(observed.at(RenderFeatureGraphStage::SSAO)[0]-1.0f)<0.01f,"a planar surface must remain unoccluded");
        require(std::abs(observed.at(RenderFeatureGraphStage::Exposure)[0]-0.1481f)<0.005f,"automatic exposure used the wrong luminance/parameters");
        require(std::abs(observed.at(RenderFeatureGraphStage::Bloom)[0]-2.4f)<0.02f,"bloom threshold/intensity parameters were not applied");
        require(red>green && green>blue && red>100 && blue>5,"HDR/effects/tone mapping did not reach final pixels: "+
            std::to_string(red)+","+std::to_string(green)+","+std::to_string(blue)+" frame="+std::to_string(frame));
        }
        require(graph.diagnostics().executedPassCount>=9,"image stages were not executed");
        if (!canRead) require(graph.diagnostics().recordedPassCount>=7,"image command recording did not participate in execution");
        history.advance();
    }
    require(history.input().id!=initialHistory.id,"history did not ping-pong");
    require(renderer.stats().transientTextureReuses>0,"transient render targets were recreated on every frame");
    require(history.resize(renderer,16,16) && !history.valid(),"resize retained stale temporal feedback");
    history.release(renderer);
    images.release(renderer);
    materials.release(renderer);
}

void vertex_stream_pixels(Renderer& renderer) {
    ShaderDesc vertex;
    vertex.name="multi_stream_vertex";
    vertex.source="struct O { float4 p:SV_Position; float4 c:COLOR0; }; O main(float3 p:POSITION,float4 c:COLOR0) { O o; o.p=float4(p,1); o.c=c; return o; }";
    ShaderDesc fragment;
    fragment.stage=ShaderStage::Fragment; fragment.name="multi_stream_fragment";
    fragment.source="float4 main(float4 p:SV_Position,float4 c:COLOR0):SV_Target { return c; }";
    const auto vs=renderer.create_shader(vertex),ps=renderer.create_shader(fragment);
    PipelineDesc pipeline;
    pipeline.name="multi_stream_test"; pipeline.vertexShader=vs.id; pipeline.fragmentShader=ps.id;
    pipeline.depthFormat="none"; pipeline.depthTest=pipeline.depthWrite=false; pipeline.cullMode="none";
    pipeline.vertexLayout.bindings={{0,12,false,1},{1,16,true,1}};
    pipeline.vertexLayout.attributes={{"POSITION",0,0,0,VertexFormat::Float3,0},{"COLOR",0,1,1,VertexFormat::Float4,0}};
    const auto program=renderer.create_pipeline(pipeline);
    require(vs && ps && program,"custom vertex pipeline: "+renderer.last_error());
    auto other=pipeline;
    other.depthStencilState=DepthStencilState{}; other.depthStencilState->depthCompare=CompareOp::Greater;
    const auto different=renderer.create_pipeline(other);
    require(different.id!=program.id,"distinct custom PSO states aliased in cache");
    const float positions[]{-1,-1,0, -1,3,0, 3,-1,0};
    const float colors[]{1,0,0,1, 0,1,0,1};
    const std::uint32_t indices[]{0,1,2};
    const auto vb=renderer.create_buffer({sizeof(positions),12,true,false,bytes(positions,sizeof(positions))});
    const auto instances=renderer.create_buffer({sizeof(colors),16,true,false,bytes(colors,sizeof(colors))});
    const auto ib=renderer.create_buffer({sizeof(indices),4,false,true,bytes(indices,sizeof(indices))});
    TextureDesc output; output.width=output.height=16; output.renderTarget=true;
    const auto target=renderer.create_texture(output);
    renderer.begin_graph();
    auto& graph=renderer.graph();
    for (const auto resource:{vb,instances,ib,program,target}) graph.import_resource(resource,*renderer.resource_description(resource));
    const auto material=graph.create_material({"multi_stream",program,{}});
    graph.add_pass("custom_vertex_draw",{{target,ResourceUsage::ColorAttachment},{vb,ResourceUsage::VertexBuffer},
        {instances,ResourceUsage::VertexBuffer},{ib,ResourceUsage::IndexBuffer},{material,ResourceUsage::ShaderRead}},
        [=](auto& backend,const auto&) {
            backend.bind_material(material);
            MeshDraw draw{vb,ib,3}; draw.vertexBindings={{vb,0,0},{instances,1,0}}; draw.firstInstance=1;
            backend.draw_mesh(draw);
        });
    std::string error;
    if (!renderer.execute_graph(graph,&error)) throw std::runtime_error("custom vertex draw: "+error);
    TextureReadback readback;
    require(renderer.read_texture({target},readback),renderer.last_error());
    const auto center=8*readback.rowPitch+8*4;
    require(readback.data[center]<5 && readback.data[center+1]>250 && readback.data[center+2]<5,
        "custom stream or first-instance vertex fetch did not reach final pixels");
}

void pbr_material_pixels(Renderer& renderer) {
    PbrMaterialLibrary materials;
    require(materials.initialize(renderer),materials.last_error());
    PbrMaterialDesc description;
    description.name="pbr_emissive_reference";description.baseColorFactor={0,0,0,1};
    description.emissiveFactor={4,0.5f,0.1f};description.doubleSided=true;
    const auto material=materials.create_material(renderer,description);
    require(static_cast<bool>(material),materials.last_error());
    const PbrMeshVertex vertices[]{{{-1,-1,0},{0,1},{0,0,-1}},{{-1,3,0},{0,-1},{0,0,-1}},{{3,-1,0},{2,1},{0,0,-1}}};
    const std::uint32_t indices[]{0,1,2};
    const auto vb=renderer.create_buffer({sizeof(vertices),sizeof(PbrMeshVertex),true,false,bytes(vertices,sizeof(vertices))});
    const auto ib=renderer.create_buffer({sizeof(indices),4,false,true,bytes(indices,sizeof(indices))});
    TextureDesc output;output.width=output.height=16;output.renderTarget=true;output.format="rgba16f";output.hdr=true;
    const auto target=renderer.create_texture(output);
    auto depthDescription=output;depthDescription.format="d24s8";depthDescription.hdr=false;depthDescription.renderTarget=false;
    const auto depth=renderer.create_depth_stencil(depthDescription);
    renderer.begin_graph();auto& graph=renderer.graph();
    for (const auto handle:{material,vb,ib,target,depth}) graph.import_resource(handle,*renderer.resource_description(handle));
    const auto& nativeMaterial=std::get<MaterialDesc>(*renderer.resource_description(material));
    std::vector<ResourceAccess> accesses{{material,ResourceUsage::ShaderRead},{vb,ResourceUsage::VertexBuffer},
        {ib,ResourceUsage::IndexBuffer},{target,ResourceUsage::ColorAttachment},{depth,ResourceUsage::DepthStencil}};
    for (const auto& binding:nativeMaterial.bindings) {
        graph.import_resource(binding.resource,*renderer.resource_description(binding.resource));
        if (std::none_of(accesses.begin(),accesses.end(),[&](const auto& value){return value.resource.id==binding.resource.id;}))
            accesses.push_back({binding.resource,binding.type==DescriptorType::UniformBuffer?ResourceUsage::UniformBuffer:ResourceUsage::ShaderRead});
    }
    graph.add_pass("pbr_reference",std::move(accesses),[=](auto& backend,const auto&) {
        backend.bind_material(material);backend.draw_mesh({vb,ib,3});
    });
    std::string error;
    if (!renderer.execute_graph(graph,&error)) throw std::runtime_error("PBR material rendering failed: "+error);
    if (renderer.capabilities().supportsTextureReadback) {
    TextureReadback readback;TextureReadbackRequest request{target};request.x=request.y=8;request.width=request.height=1;
    require(renderer.read_texture(request,readback),renderer.last_error());
    std::uint16_t pixel[4];std::memcpy(pixel,readback.data.data(),8);
    require(std::abs(half_float(pixel[0])-4)<0.01 && std::abs(half_float(pixel[1])-0.5f)<0.01 &&
        std::abs(half_float(pixel[2])-0.1f)<0.01,"PBR emissive did not preserve HDR radiance");
    }
    materials.release(renderer);
}

void shadowed_scene(Renderer& renderer) {
    PbrMaterialLibrary materials;
    require(materials.initialize(renderer),materials.last_error());
    PbrMaterialDesc description;description.name="shadow_receiver";description.doubleSided=true;
    const auto material=materials.create_material(renderer,description);
    require(static_cast<bool>(material),materials.last_error());
    const PbrMeshVertex vertices[]{{{-1,-1,0},{0,1},{0,0,-1}},{{-1,1,0},{0,0},{0,0,-1}},
        {{1,1,0},{1,0},{0,0,-1}},{{1,-1,0},{1,1},{0,0,-1}}};
    const std::uint32_t indices[]{0,1,2,0,2,3};
    const auto vb=renderer.create_buffer({sizeof(vertices),sizeof(PbrMeshVertex),true,false,bytes(vertices,sizeof(vertices))});
    const auto ib=renderer.create_buffer({sizeof(indices),4,false,true,bytes(indices,sizeof(indices))});
    shinkou::World world;
    const auto camera=world.ecs().create();
    TransformComponent cameraTransform;cameraTransform.local.position={0,0,-5};
    world.ecs().emplace<TransformComponent>(camera,cameraTransform);
    CameraComponent cameraDescription;cameraDescription.nearPlane=0.1f;cameraDescription.farPlane=20;
    world.ecs().emplace<CameraComponent>(camera,cameraDescription);
    const auto lightEntity=world.ecs().create();
    TransformComponent lightTransform;lightTransform.local.rotation=shinkou::math::FromAxisAngle({0,1,0},shinkou::math::Pi/4);
    world.ecs().emplace<TransformComponent>(lightEntity,lightTransform);
    LightComponent light;light.intensity=3;light.castsShadow=true;
    world.ecs().emplace<LightComponent>(lightEntity,light);
    for (unsigned index=0;index<3;++index) {
        const auto entity=world.ecs().create();
        TransformComponent transform;
        if (index==0) transform.local.scale={2,2,1};
        else if (index==1) { transform.local.position={-0.5f,0,-1};transform.local.scale={0.35f,0.35f,1}; }
        else transform.local.position={100,0,0}; // Must remain available as a shadow caster.
        world.ecs().emplace<TransformComponent>(entity,transform);
        world.ecs().emplace<MeshRendererComponent>(entity,MeshRendererComponent{vb,ib,material,6});
    }
    RenderScene scene;scene.extract(world,renderer,1);
    require(scene.shadow_meshes().size()==3 && scene.meshes().size()==2,"camera culling removed off-screen shadow casters");
    TextureDesc color;color.width=color.height=64;color.format="rgba16f";color.hdr=true;color.renderTarget=true;
    const auto target=renderer.create_texture(color);
    TextureDesc depth;color.initialData.clear();depth.width=depth.height=64;depth.format="d24s8";
    const auto depthTarget=renderer.create_depth_stencil(depth);
    TextureDesc atlas=depth;atlas.width=atlas.height=512;
    const auto shadowTarget=renderer.create_depth_stencil(atlas);
    ShadowSettings settings;settings.atlasSize=512;settings.cascadeCount=2;settings.maxDistance=10;
    ForwardRenderer forward;
    std::vector<float> reference;
    for (unsigned frame=0;frame<3;++frame) {
        renderer.begin_graph();
        if (frame==1) forward.build_shadows(renderer,scene,settings,materials.shadow_pipeline(),shadowTarget,atlas);
        forward.build(renderer,scene,target,color,depthTarget,depth);
        std::string error;
        if (!renderer.execute_graph(renderer.graph(),&error)) throw std::runtime_error("shadow scene execution: "+error);
        if (!renderer.capabilities().supportsTextureReadback) continue;
        TextureReadback image;require(renderer.read_texture({target},image),renderer.last_error());
        std::vector<float> pixels;
        for (unsigned y=0;y<64;++y) for (unsigned x=0;x<64;++x) {
            std::uint16_t red;std::memcpy(&red,image.data.data()+y*image.rowPitch+x*8,2);pixels.push_back(half_float(red));
        }
        if (frame==0) reference=pixels;
        else if (frame==1) {
            unsigned shadowed=0;
            for (std::size_t i=0;i<pixels.size();++i) if (reference[i]>0.1f && pixels[i]<reference[i]*0.75f) ++shadowed;
            require(shadowed>10,"cascade atlas/PCF did not affect PBR pixels: "+std::to_string(shadowed));
        } else {
            for (std::size_t i=0;i<pixels.size();++i)
                require(std::abs(reference[i]-pixels[i])<0.01f,"previous frame shadow resources leaked across graph reset");
        }
    }
    materials.release(renderer);
}

void standard_pipeline_lifecycle(Renderer& renderer) {
    StandardRenderPipeline pipeline;
    StandardRenderPipelineDesc description;
    description.width=description.height=16;description.presentationFormat="rgba8";description.shadows.enabled=false;
    description.ssao=description.ssr=description.taa=description.bloom=description.exposure=false;
    if (!pipeline.initialize(renderer,description)) throw std::runtime_error(pipeline.last_error());
    TextureDesc targetDescription;targetDescription.width=targetDescription.height=16;targetDescription.renderTarget=true;
    const auto target=renderer.create_texture(targetDescription);
    shinkou::World world;RenderScene scene;scene.extract(world,renderer,1);
    renderer.begin_graph();
    if (!pipeline.build(renderer,scene,target)) throw std::runtime_error(pipeline.last_error());
    const auto passCount=renderer.graph().passes().size();
    require(!pipeline.build(renderer,scene,target) && renderer.graph().passes().size()==passCount,"pending standard frame was silently overwritten");
    require(!pipeline.resize(renderer,32,32),"resize invalidated a pending graph");
    std::string error;
    if (!renderer.execute_graph(renderer.graph(),&error)) throw std::runtime_error("standard empty scene: "+error);
    pipeline.complete_frame(true);
    require(renderer.graph().diagnostics().recordedPassCount==3,"standard pipeline did not record depth and image commands");
    const auto original=pipeline.scene_color();
    require(!pipeline.resize(renderer,0,32) && pipeline.scene_color().id==original.id,"failed resize discarded working targets");
    require(pipeline.resize(renderer,32,32) && pipeline.scene_color().id!=original.id,"standard targets did not resize");
    const auto resized=pipeline.scene_color();
    pipeline.release(renderer);
    require(renderer.resource_description(resized)==nullptr,"standard pipeline release retained owned targets");
    renderer.destroy_resource(target);
}

std::array<float,4> hdr_pixel(Renderer& renderer,ResourceHandle texture,unsigned x=0,unsigned y=0) {
    TextureReadback image;TextureReadbackRequest request{texture};request.x=x;request.y=y;request.width=request.height=1;
    require(renderer.read_texture(request,image),"HDR readback: "+renderer.last_error());
    std::uint16_t values[4];std::memcpy(values,image.data.data(),8);
    return {half_float(values[0]),half_float(values[1]),half_float(values[2]),half_float(values[3])};
}
std::vector<std::uint8_t> half_pixels(unsigned size,std::array<std::uint16_t,4> pixel) {
    std::vector<std::uint8_t> result(size*size*8);
    for (unsigned i=0;i<size*size;++i) std::memcpy(result.data()+i*8,pixel.data(),8);
    return result;
}
void motion_vector_history(Renderer& renderer) {
    StandardRenderPipeline pipeline;StandardRenderPipelineDesc settings;
    settings.width=settings.height=64;settings.presentationFormat="rgba8";settings.shadows.enabled=false;
    settings.ssao=settings.ssr=settings.bloom=settings.exposure=false;settings.motionBlur=true;settings.temporalJitter=false;
    require(pipeline.initialize(renderer,settings),pipeline.last_error());
    PbrMaterialDesc material;material.name="moving_quad";material.doubleSided=true;material.emissiveFactor={1,0,0};
    const auto surface=pipeline.create_material(renderer,material);require(bool(surface),pipeline.last_error());
    const PbrMeshVertex vertices[]{{{-.25f,-.25f,0},{0,1},{0,0,-1}},{{-.25f,.25f,0},{0,0},{0,0,-1}},
        {{.25f,.25f,0},{1,0},{0,0,-1}},{{.25f,-.25f,0},{1,1},{0,0,-1}}};
    const std::uint32_t indices[]{0,1,2,0,2,3};
    const auto vb=renderer.create_buffer({sizeof(vertices),sizeof(PbrMeshVertex),true,false,bytes(vertices,sizeof(vertices))});
    const auto ib=renderer.create_buffer({sizeof(indices),4,false,true,bytes(indices,sizeof(indices))});
    shinkou::World world;const auto camera=world.ecs().create(),object=world.ecs().create();
    TransformComponent cameraPose;cameraPose.local.position={0,0,-5};
    world.ecs().emplace<TransformComponent>(camera,cameraPose);
    CameraComponent projection;projection.orthographic=true;projection.orthographicSize=2;projection.nearPlane=.1f;projection.farPlane=20;
    world.ecs().emplace<CameraComponent>(camera,projection);
    TransformComponent pose;pose.local.position.x=-.2f;world.ecs().emplace<TransformComponent>(object,pose);
    world.ecs().emplace<MeshRendererComponent>(object,MeshRendererComponent{vb,ib,surface,6});
    TextureDesc output;output.width=output.height=64;output.renderTarget=true;
    const auto target=renderer.create_texture(output);
    const bool read=renderer.capabilities().supportsTextureReadback;
    unsigned frameIndex=0;
    const auto frame=[&](bool commit,unsigned x) {
        RenderScene scene;scene.extract(world,renderer,1);renderer.begin_graph();
        require(pipeline.build(renderer,scene,target),pipeline.last_error());
        if (frameIndex++==0) require(!pipeline.set_stages({}),"stage changes invalidated a pending frame");
        std::string error;require(renderer.execute_graph(renderer.graph(),&error),error);
        const auto pixel=read?hdr_pixel(renderer,pipeline.motion_vectors(),x,32):std::array<float,4>{};
        pipeline.complete_frame(commit);return pixel;
    };
    auto pixel=frame(true,25);if (read) require(pixel[3]<.5f,"first frame object motion must reject history");
    world.ecs().get<TransformComponent>(object).local.position.x=.2f;
    pixel=frame(true,38);
    if (read) require(std::abs(pixel[0]+.2f)<.002f && std::abs(pixel[1])<.002f && pixel[3]>.5f,
        "object motion must be previous UV minus current UV: "+std::to_string(pixel[0])+","+std::to_string(pixel[3]));
    world.ecs().get<TransformComponent>(camera).local.position.x=.2f;
    pixel=frame(true,32);
    if (read) require(std::abs(pixel[0]-.1f)<.002f && pixel[3]>.5f,"camera translation missing from motion vectors");
    pipeline.invalidate_history();pixel=frame(false,32);
    if (read) require(pixel[3]<.5f,"camera cut retained previous object history");
    pixel=frame(true,32);if (read) require(pixel[3]<.5f,"failed frame committed motion history");
    pixel=frame(true,32);if (read) require(std::abs(pixel[0])<.001f && pixel[3]>.5f,"stationary motion is not zero");
    using Stage=RenderFeatureGraphStage;
    require(pipeline.set_stages({{"z",Stage::DepthPrepass},{"scene",Stage::Opaque},{"velocity",Stage::MotionVectors},
        {"resolve",Stage::TAA},{"display",Stage::ToneMap},{"screen",Stage::Present}}),pipeline.last_error());
    pixel=frame(true,32);if (read) require(pixel[3]<.5f,"stage reconfiguration retained stale motion history");
    pipeline.release(renderer);
    settings.temporalJitter=true;require(pipeline.initialize(renderer,settings),pipeline.last_error());
    world.ecs().get<MeshRendererComponent>(object).material=pipeline.create_material(renderer,material);
    frame(true,32);pixel=frame(true,32);
    if (read) require(std::abs(pixel[0]-.25f/64)<.0002f && std::abs(pixel[1]+(1.0f/3)/64)<.0002f,
        "jitter was not included consistently in scene and motion projections: "+std::to_string(pixel[0])+","+std::to_string(pixel[1]));
    pipeline.release(renderer);for (auto handle:{vb,ib,target}) renderer.destroy_resource(handle);
}

void temporal_image_references(Renderer& renderer) {
    using Stage=RenderFeatureGraphStage;
    BuiltinRenderFeatures images;require(images.initialize(renderer,"rgba16f"),images.last_error());
    TextureDesc hdr;hdr.width=hdr.height=8;hdr.format="rgba16f";hdr.hdr=true;hdr.renderTarget=true;
    hdr.initialData=half_pixels(8,{0x3c00,0x3c00,0x3c00,0x3c00});
    const auto source=renderer.create_texture(hdr);
    auto clean=hdr;clean.initialData.clear();const auto target=renderer.create_texture(clean);
    TextureDesc z;z.width=z.height=8;z.format="d24s8";const auto depth=renderer.create_depth_stencil(z);
    TemporalRenderHistory exposure;require(exposure.resize(renderer,1,1),"exposure history");
    const auto base=[&] {
        RenderFeatureGraphDesc desc;desc.width=desc.height=8;desc.capabilities=renderer.capabilities();
        desc.lighting.binning.mode=LightBinningMode::Tiles;desc.preferComputePostProcess=false;
        desc.sceneColor=source;desc.sceneColorDescription=hdr;desc.depthTarget=depth;desc.depthDescription=z;
        desc.presentTarget=target;desc.presentDescription=clean;return desc;
    };
    float expected=.18f;
    for (unsigned frame=0;frame<5;++frame) {
        renderer.begin_graph();auto desc=base();desc.stages={{"exposure",Stage::Exposure}};
        PostProcessStageDesc config;exposure.configure(config,PostProcessStageKind::Exposure);
        ExposureStageParameters params;params.adaptationRate=1;params.darkAdaptationRate=2;config.parameters=params;
        desc.postProcessStages={config};desc.exposureHistoryValid=exposure.valid();desc.deltaSeconds=frame==2?0:1;
        if (frame==1) {
            TextureUpdate update;update.texture=source;update.width=update.height=8;update.rowPitch=64;
            update.data=half_pixels(8,{0x4400,0x4400,0x4400,0x3c00});
            require(renderer.update_texture(update),renderer.last_error());
            expected=.18f+(.045f-.18f)*(1-std::exp(-1.0f));
        }
        if (frame==3) {exposure.invalidate();desc.exposureHistoryValid=false;expected=.045f;}
        if (frame==4) {
            TextureUpdate change;change.texture=source;change.width=change.height=8;change.rowPitch=64;
            change.data=half_pixels(8,{0x3c00,0x3c00,0x3c00,0x3c00});
            require(renderer.update_texture(change),renderer.last_error());
            expected=.045f+(.18f-.045f)*(1-std::exp(-2.0f));
        }
        require(images.install(desc),"exposure install");
        const auto plan=RenderFeatureGraphBuilder::build(renderer.graph(),desc);require(plan.valid,plan.error);
        std::string error;require(renderer.execute_graph(renderer.graph(),&error),error);
        if (renderer.capabilities().supportsTextureReadback) {
            const auto actual=hdr_pixel(renderer,exposure.output())[0];
            require(std::abs(actual-expected)<.0008f,"exposure adaptation reference mismatch: "+std::to_string(actual)+" expected "+std::to_string(expected));
        }
        exposure.advance();
    }
    auto current=half_pixels(8,{0,0,0,0x3c00});
    for (unsigned y=0;y<8;++y) for (unsigned x=4;x<8;++x) {
        const std::array<std::uint16_t,4> p{x==4?std::uint16_t(0x3800):std::uint16_t(0x3c00),0,0,0x3c00};
        std::memcpy(current.data()+(y*8+x)*8,p.data(),8);
    }
    TextureUpdate update;update.texture=source;update.width=update.height=8;update.rowPitch=64;update.data=current;
    require(renderer.update_texture(update),renderer.last_error());
    auto old=hdr;old.initialData=half_pixels(8,{0x3400,0,0,0x3c00});
    for (unsigned y=0;y<8;++y) {const std::uint16_t one=0x3c00;std::memcpy(old.initialData.data()+(y*8+6)*8,&one,2);}
    const auto history=renderer.create_texture(old),historyOutput=renderer.create_texture(clean);
    auto velocity=hdr;velocity.initialData=half_pixels(8,{0x3400,0,0x3c00,0x3c00});
    const auto motion=renderer.create_texture(velocity);
    for (unsigned scenario=0;scenario<4;++scenario) {
        renderer.begin_graph();auto& graph=renderer.graph();graph.import_resource(depth,z);
        graph.add_pass("clear_reference_depth",{{depth,ResourceUsage::DepthStencil}},{});
        auto desc=base();desc.stages={{"resolve",Stage::TAA},{"screen",Stage::Present}};
        desc.stages.front().inputs={{"motion_vectors","velocity"}};
        desc.resources={{"velocity",motion,velocity,RenderImageDomain::Data}};desc.historyValid=true;
        PostProcessStageDesc config;config.kind=PostProcessStageKind::TAA;
        config.historyTexture=history;config.historyDescription=old;config.historyOutput=historyOutput;config.historyOutputDescription=clean;
        TaaStageParameters params;params.feedback=1;params.sharpness=params.motionRejection=0;config.parameters=params;desc.postProcessStages={config};
        if (scenario) {
            TextureUpdate change;change.width=change.height=8;change.rowPitch=64;
            if (scenario==1) {change.texture=history;change.data=half_pixels(8,{0x3c00,0,0,0});}
            else {
                if (scenario==2) {
                    TextureUpdate restore;restore.texture=history;restore.width=restore.height=8;restore.rowPitch=64;restore.data=old.initialData;
                    require(renderer.update_texture(restore),renderer.last_error());
                }
                change.texture=motion;change.data=half_pixels(8,scenario==2?
                std::array<std::uint16_t,4>{0x3400,0,0x3c00,0}:std::array<std::uint16_t,4>{0x3c00,0,0x3c00,0x3c00});}
            require(renderer.update_texture(change),renderer.last_error());
        }
        require(images.install(desc),"TAA install");const auto plan=RenderFeatureGraphBuilder::build(graph,desc);require(plan.valid,plan.error);
        std::string error;require(renderer.execute_graph(graph,&error),error);
        if (renderer.capabilities().supportsTextureReadback) {
            const auto pixel=hdr_pixel(renderer,target,4,4);
            require(std::abs(pixel[0]-(scenario==0?1.0f:.5f))<.005f,"TAA motion/disocclusion reference mismatch: "+std::to_string(scenario)+" = "+std::to_string(pixel[0]));
        }
    }
    update.data=half_pixels(8,{0,0,0,0x3c00});
    for (unsigned y=0;y<8;++y) {const std::uint16_t one=0x3c00;std::memcpy(update.data.data()+(y*8+4)*8,&one,2);}
    require(renderer.update_texture(update),renderer.last_error());
    for (unsigned moving=0;moving<2;++moving) {
        renderer.begin_graph();auto desc=base();desc.stages={{"blur",Stage::MotionBlur},{"screen",Stage::Present}};
        desc.stages.front().inputs={{"motion_vectors","velocity"}};desc.resources={{"velocity",motion,velocity,RenderImageDomain::Data}};
        TextureUpdate change;change.texture=motion;change.width=change.height=8;change.rowPitch=64;
        change.data=half_pixels(8,{moving?std::uint16_t(0x3800):std::uint16_t(0),0,0x3c00,0x3c00});
        require(renderer.update_texture(change),renderer.last_error());
        PostProcessStageDesc config;config.kind=PostProcessStageKind::MotionBlur;MotionBlurStageParameters params;
        params.shutterFraction=1;params.sampleCount=9;config.parameters=params;desc.postProcessStages={config};
        require(images.install(desc),"motion blur install");const auto plan=RenderFeatureGraphBuilder::build(renderer.graph(),desc);require(plan.valid,plan.error);
        std::string error;require(renderer.execute_graph(renderer.graph(),&error),error);
        if (renderer.capabilities().supportsTextureReadback) {
            const auto center=hdr_pixel(renderer,target,4,4)[0],neighbor=hdr_pixel(renderer,target,3,4)[0];
            require(moving?(center>.1f && center<.8f && neighbor>.05f):(center>.99f && neighbor<.01f),
                "motion blur did not preserve stationary pixels or integrate moving radiance");
        }
    }
    // A bright impulse must spread spatially, and increasing pyramid levels
    // must increase its reach. Inspect the final full-resolution Bloom image.
    update.data=half_pixels(8,{0,0,0,0x3c00});
    for (unsigned y=3;y<=4;++y) for (unsigned x=3;x<=4;++x) {
        const std::uint16_t bright=0x4800;std::memcpy(update.data.data()+(y*8+x)*8,&bright,2);
    }
    require(renderer.update_texture(update),renderer.last_error());
    float narrow=0;
    for (unsigned levels:{1u,8u}) {
        renderer.begin_graph();auto desc=base();desc.stages={{"glow",Stage::Bloom},{"screen",Stage::Present}};
        desc.stages.back().source="glow";
        PostProcessStageDesc config;config.kind=PostProcessStageKind::Bloom;
        BloomStageParameters params;params.mipLevels=levels;params.intensity=1;params.scatter=1;config.parameters=params;
        desc.postProcessStages={config};require(images.install(desc),"Bloom install");
        const auto plan=RenderFeatureGraphBuilder::build(renderer.graph(),desc);require(plan.valid,plan.error);
        require(plan.passes.front().passIndices.size()==(levels==1?2u:6u),"Bloom pyramid did not stop at 1x1 or omitted reconstruction passes");
        std::string error;require(renderer.execute_graph(renderer.graph(),&error),error);
        if (renderer.capabilities().supportsTextureReadback) {
            const auto halo=hdr_pixel(renderer,target,0,0)[0];
            if (levels==1) narrow=halo;
            else require(halo>narrow+.02f,"additional Bloom levels did not spread radiance to distant pixels");
        }
    }
    exposure.release(renderer);images.release(renderer);
    for (auto handle:{source,target,depth,history,historyOutput,motion}) renderer.destroy_resource(handle);
}

void custom_stage_order_pixels(Renderer& renderer) {
    using Stage=RenderFeatureGraphStage;
    const auto api=renderer.capabilities().api;
    const auto vertex=renderer.create_shader(builtin_fullscreen_vertex_shader(api));
    ShaderDesc fragment;fragment.stage=ShaderStage::Fragment;fragment.name="pipeline_order_reference";
    std::ifstream shader(api==BackendApi::Vulkan?SHINKOU_PIPELINE_TEST_SPIRV:SHINKOU_PIPELINE_TEST_SHADER,std::ios::binary);
    require(bool(shader),"custom pipeline test shader is missing");
    if (api==BackendApi::Vulkan) {fragment.sourceKind=ShaderSourceKind::SpirV;fragment.bytecode.assign(std::istreambuf_iterator<char>(shader),{});}
    else fragment.source.assign(std::istreambuf_iterator<char>(shader),{});
    const auto pixel=renderer.create_shader(fragment);PipelineDesc pso;
    pso.name="pipeline_order_reference";pso.vertexShader=vertex.id;pso.fragmentShader=pixel.id;
    pso.vertexInput=false;pso.depthTest=pso.depthWrite=false;pso.depthFormat="none";pso.cullMode="none";pso.colorFormat="rgba16f";
    const auto pipeline=renderer.create_pipeline(pso);require(bool(pipeline),renderer.last_error());
    SamplerDesc linear;linear.addressU=linear.addressV=linear.addressW="clamp";
    const auto sampler=renderer.create_sampler(linear);
    TextureDesc hdr;hdr.width=hdr.height=8;hdr.format="rgba16f";hdr.hdr=true;hdr.renderTarget=true;
    hdr.initialData=half_pixels(8,{0,0,0,0x3c00});const auto source=renderer.create_texture(hdr);
    auto output=hdr;output.initialData.clear();const auto target=renderer.create_texture(output);
    TextureDesc z;z.width=z.height=8;z.format="d24s8";const auto depth=renderer.create_depth_stencil(z);
    BuiltinRenderFeatures images;require(images.initialize(renderer,"rgba16f"),images.last_error());
    for (unsigned reverse=0;reverse<2;++reverse) {
        renderer.begin_graph();RenderFeatureGraphDesc desc;
        desc.capabilities=renderer.capabilities();desc.width=desc.height=8;desc.lighting.binning.mode=LightBinningMode::Tiles;
        desc.sceneColor=source;desc.sceneColorDescription=hdr;desc.depthTarget=depth;desc.depthDescription=z;
        desc.presentTarget=target;desc.presentDescription=output;
        desc.stages={{"gain",Stage::Custom},{"bias",Stage::Custom},{"screen",Stage::Present}};
        if (reverse) std::swap(desc.stages[0],desc.stages[1]);
        for (unsigned index=0;index<2;++index) {
            RenderFeatureGraphExecutor executor;executor.stage=Stage::Custom;executor.name=index==0?"gain":"bias";
            executor.prepare=[=](auto& graph,const auto& contract) {
                graph.import_resource(pipeline,pso);graph.import_resource(sampler,linear);
                const float parameters[]{index==0?2.0f:1.0f,1,1,1,index==0?0.0f:.25f,0,0,0};
                BufferDesc buffer;buffer.size=sizeof(parameters);buffer.stride=16;buffer.uniformBuffer=true;buffer.initialData=bytes(parameters,sizeof(parameters));
                const auto constants=graph.create_buffer(buffer);
                const auto material=graph.create_material({contract.name,pipeline,{{"Source",contract.inputs.front().handle,DescriptorType::Texture,0},
                    {"LinearClamp",sampler,DescriptorType::Sampler,1},{"Transform",constants,DescriptorType::UniformBuffer,4}},false});
                RenderFeatureGraphCommands commands;commands.accesses={{pipeline,ResourceUsage::ShaderRead},{sampler,ResourceUsage::ShaderRead},
                    {constants,ResourceUsage::UniformBuffer},{material,ResourceUsage::ShaderRead}};
                commands.recordCommands=[material](auto& list,const auto&){list.bind_material(material);list.draw_sprite({});};
                return commands;
            };
            desc.executors.push_back(std::move(executor));
        }
        require(images.install(desc),"custom pipeline image programs");
        const auto plan=RenderFeatureGraphBuilder::build(renderer.graph(),desc);require(plan.valid,plan.error);
        std::string error;require(renderer.execute_graph(renderer.graph(),&error),error);
        if (renderer.capabilities().supportsTextureReadback)
            require(std::abs(hdr_pixel(renderer,target,4,4)[0]-(reverse?.5f:.25f))<.002f,
                "custom pipeline reordering did not change native output pixels");
    }
    images.release(renderer);
    for (auto handle:{pipeline,sampler,source,target,depth,pixel,vertex}) renderer.destroy_resource(handle);
}
}
int main(int argc,char** argv) {
    try {
        configuration_tests();
        const std::string requested=argc>1?argv[1]:"dx11";
        const auto api=requested=="dx12"?BackendApi::DirectX12:requested=="vulkan"?BackendApi::Vulkan:BackendApi::DirectX11;
        Renderer renderer(api,{nullptr,32,32,false,true});
        if (!renderer.initialize() || !renderer.capabilities().deviceReady) {
            std::cout<<requested<<" native device unavailable: "<<renderer.last_error()<<'\n'; return 77;
        }
        pbr_material_pixels(renderer);
        shadowed_scene(renderer);
        standard_pipeline_lifecycle(renderer);
        motion_vector_history(renderer);
        temporal_image_references(renderer);
        custom_stage_order_pixels(renderer);
        image_pipeline(renderer);
        if (api==BackendApi::DirectX11) vertex_stream_pixels(renderer);
        std::cout<<"Verified native PBR, HDR effects, temporal history and resource reuse\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}

#include "shinkou/render/RenderFeatureGraphBuilder.h"
#include "shinkou/render/RenderGraphScheduler.h"
#include "shinkou/render/BuiltinRenderFeatures.h"
#include <iostream>
#include <stdexcept>

using namespace shinkou::render;
namespace {
using Stage=RenderFeatureGraphStage;
void require(bool value,const std::string& error) {if (!value) throw std::runtime_error(error);}
RenderFeatureGraphDesc description(std::vector<std::string>& calls) {
    RenderFeatureGraphDesc desc;
    desc.capabilities.deviceReady=true;desc.width=desc.height=16;
    desc.lighting.binning.mode=LightBinningMode::Tiles;desc.preferComputePostProcess=false;
    desc.stages={{"scene",Stage::Opaque},{"gain",Stage::Custom},{"bias",Stage::Custom},
        {"display",Stage::ToneMap},{"screen",Stage::Present}};
    for (auto kind:{Stage::Opaque,Stage::DepthPrepass,Stage::ShadowCascades,Stage::ToneMap,Stage::Present})
        desc.executors.push_back({kind,[&](auto& backend,const auto& context){calls.emplace_back(context.name);backend.draw_sprite({});}});
    for (auto name: {"gain","bias"}) {
        RenderFeatureGraphExecutor executor;
        executor.stage=Stage::Custom;executor.name=name;
        executor.record=[&](auto& backend,const auto& context){calls.emplace_back(context.name);backend.draw_sprite({});};
        desc.executors.push_back(std::move(executor));
    }
    return desc;
}
}
int main() {
    try {
        std::vector<std::string> calls;
        auto desc=description(calls);
        RenderGraph graph;
        auto plan=RenderFeatureGraphBuilder::build(graph,desc);
        require(plan.valid,plan.error);
        require(plan.passes[2].description.inputs.front().handle.id==plan.passes[1].description.outputs.front().handle.id,
            "color stream was not connected in authored order");
        auto backend=create_backend(BackendApi::Null);require(backend->initialize({}),"null init");
        std::string error;graph.execute(*backend,&error);require(error.empty(),error);
        require(calls==std::vector<std::string>{"scene","gain","bias","display","screen"},"authored order was ignored");
        std::swap(desc.stages[1],desc.stages[2]);calls.clear();graph.reset();
        plan=RenderFeatureGraphBuilder::build(graph,desc);require(plan.valid,plan.error);
        graph.execute(*backend,&error);require(error.empty(),error);
        require(calls==std::vector<std::string>{"scene","bias","gain","display","screen"},"reordering did not reach execution");
        require(plan.passes[2].description.inputs.front().name=="bias","reordering did not rewire color inputs");

        auto geometry=desc;
        geometry.stages={{"sun",Stage::ShadowCascades},{"z",Stage::DepthPrepass},{"scene",Stage::Opaque},
            {"display",Stage::ToneMap},{"screen",Stage::Present}};
        RenderGraph geometryGraph;calls.clear();
        auto geometryPlan=RenderFeatureGraphBuilder::build(geometryGraph,geometry);require(geometryPlan.valid,geometryPlan.error);
        geometryGraph.execute(*backend,&error);require(error.empty(),error);
        require(calls.front()=="sun" && calls[1]=="z","geometry stage order was fixed internally");
        std::swap(geometry.stages[0],geometry.stages[1]);geometryGraph.reset();calls.clear();
        geometryPlan=RenderFeatureGraphBuilder::build(geometryGraph,geometry);require(geometryPlan.valid,geometryPlan.error);
        geometryGraph.execute(*backend,&error);require(error.empty(),error);
        require(calls.front()=="z" && calls[1]=="sun","shadow/depth order did not change");

        RenderGraph untouched;untouched.create_buffer({16,4});
        auto invalid=desc;invalid.stages[1].dependsOn={invalid.stages[2].name};
        require(!RenderFeatureGraphBuilder::build(untouched,invalid).valid && untouched.passes().empty() && untouched.resources().size()==1,
            "invalid ordering mutated the destination graph");
        invalid=desc;invalid.stages.back().name=invalid.stages.front().name;
        require(!RenderFeatureGraphBuilder::build(untouched,invalid).valid,"duplicate instance names accepted");
        invalid=desc;invalid.stages[2].source="missing_texture";
        require(!RenderFeatureGraphBuilder::build(untouched,invalid).valid,"missing named resource accepted");
        invalid=desc;invalid.stages.insert(invalid.stages.end()-1,{"too_late",Stage::Exposure});
        require(!RenderFeatureGraphBuilder::build(untouched,invalid).valid,"HDR effect accepted after display conversion");
        invalid=desc;std::swap(invalid.stages[0],invalid.stages.back());
        require(!RenderFeatureGraphBuilder::build(untouched,invalid).valid,"early presentation accepted");
        invalid=desc;TextureDesc ldr;ldr.width=ldr.height=16;ldr.renderTarget=true;invalid.stages[1].output=ldr;
        require(!RenderFeatureGraphBuilder::build(untouched,invalid).valid,"HDR metadata accepted an 8-bit output");
        invalid=desc;invalid.deltaSeconds=-1;
        require(!RenderFeatureGraphBuilder::build(untouched,invalid).valid,"negative frame duration accepted");
        invalid=desc;invalid.sky.enabled=true;invalid.stages.insert(invalid.stages.begin(),{"background",Stage::Sky});
        require(!RenderFeatureGraphBuilder::build(untouched,invalid).valid,"sky accepted before scene attachments were produced");
        invalid=desc;invalid.sky.enabled=true;invalid.stages.insert(invalid.stages.end()-1,{"background",Stage::Sky});
        require(!RenderFeatureGraphBuilder::build(untouched,invalid).valid,"HDR sky accepted after display conversion");
        PostProcessStageDesc parameters;parameters.kind=PostProcessStageKind::Exposure;
        ExposureStageParameters exposure;exposure.minExposure=2;exposure.maxExposure=1;parameters.parameters=exposure;
        require(!validate_post_process_parameters(parameters,error),"inverted exposure range accepted");
        exposure={};exposure.metering=ExposureMetering::Histogram;exposure.lowPercentile=.8f;exposure.highPercentile=.2f;parameters.parameters=exposure;
        require(!validate_post_process_parameters(parameters,error),"inverted histogram percentile range accepted");
        exposure.lowPercentile=0;exposure.highPercentile=1;exposure.minLogLuminance=exposure.maxLogLuminance;parameters.parameters=exposure;
        require(!validate_post_process_parameters(parameters,error),"zero histogram luminance range accepted");
        parameters.kind=PostProcessStageKind::TAA;TaaStageParameters temporal;temporal.depthRejection=-1;parameters.parameters=temporal;
        require(!validate_post_process_parameters(parameters,error),"negative history-depth rejection accepted");
        parameters.kind=PostProcessStageKind::Bloom;BloomStageParameters bloom;bloom.scatter=2;parameters.parameters=bloom;
        require(!validate_post_process_parameters(parameters,error),"invalid Bloom reconstruction weight accepted");

        auto expanded=desc;
        auto& bias=expanded.executors.back();bias.record={};
        bias.prepare=[&](auto& staged,const auto& contract) {
            RenderFeatureGraphCommands commands;
            for (auto name:{"bias/prefilter","bias/finish"}) {
                const auto index=staged.add_pass(name,{{contract.inputs.front().handle,ResourceUsage::ShaderRead},
                    {contract.outputs.front().handle,ResourceUsage::ColorAttachment}},
                    [&](auto& native,const auto& context){calls.emplace_back(context.name);native.draw_sprite({});});
                commands.expandedPasses.push_back(index);
            }
            return commands;
        };
        RenderGraph expandedGraph;calls.clear();
        const auto expandedPlan=RenderFeatureGraphBuilder::build(expandedGraph,expanded);require(expandedPlan.valid,expandedPlan.error);
        expandedGraph.execute(*backend,&error);require(error.empty(),error);
        require(calls==std::vector<std::string>{"scene","bias/prefilter","bias/finish","gain","display","screen"},
            "expanded subgraph escaped its configured position");
        require(expandedPlan.passes[1].passIndices.size()==2,"expanded native passes were not exposed");

        RenderGraph order;
        const auto a=order.add_pass("a",std::vector<ResourceAccess>{},{});
        const auto b=order.add_pass("b",std::vector<ResourceAccess>{},{});
        require(order.add_dependency(b,a) && order.compile(&error),error);
        require(order.execution_order()==std::vector<std::size_t>{b,a},"explicit side-effect ordering was ignored");
        const auto recording=RenderGraphScheduler{}.build(order);
        require(recording.valid && recording.levels.size()==2 && recording.tasks.front().passIndex==b,
            "recording scheduler ignored explicit ordering");
        require(order.add_dependency(a,b) && !order.compile(&error),"explicit dependency cycle accepted");
        RenderGraph queues;
        const auto producer=queues.add_pass("graphics_side_effect",std::vector<ResourceAccess>{},{},RenderQueue::Graphics);
        const auto consumer=queues.add_pass("compute_side_effect",std::vector<ResourceAccess>{},{},RenderQueue::Compute);
        require(queues.add_dependency(producer,consumer) && queues.compile(&error),error);
        require(queues.queue_dependencies().size()==1 && queues.queue_batches().size()==2 &&
            queues.queue_batches().back().waitBatches==std::vector<std::uint32_t>{0},"cross-queue side effects lost their explicit wait");
        queues.reset();require(queues.explicit_dependencies().empty(),"graph reset retained ordering edges");

        Renderer nullRenderer;require(nullRenderer.initialize(),"null renderer init");
        BuiltinRenderFeatures images;require(images.initialize(nullRenderer),images.last_error());
        RenderFeatureGraphDesc unavailable;unavailable.width=unavailable.height=16;unavailable.capabilities=nullRenderer.capabilities();
        unavailable.lighting.binning.mode=LightBinningMode::Tiles;
        unavailable.stages={{"scene",Stage::Opaque},{"meter",Stage::Exposure}};
        unavailable.executors={{Stage::Opaque,[](auto&,const auto&){}}};
        PostProcessStageDesc meter;meter.kind=PostProcessStageKind::Exposure;
        exposure={};exposure.metering=ExposureMetering::Histogram;meter.parameters=exposure;unavailable.postProcessStages={meter};
        require(images.install(unavailable),"image install");
        const auto unsupported=RenderFeatureGraphBuilder::build(untouched,unavailable);
        require(!unsupported.valid && unsupported.error.find("compute support")!=std::string::npos && untouched.resources().size()==1,
            "unsupported histogram metering was silently replaced or mutated the graph");
        images.release(nullRenderer);
        std::cout<<"Verified named pipeline stages, geometry/image order, expanded subgraphs and dependency validation\n";
        return 0;
    } catch (const std::exception& failure) {std::cerr<<failure.what()<<'\n';return 1;}
}

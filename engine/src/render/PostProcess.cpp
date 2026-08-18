#include "shinkou/render/PostProcess.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace shinkou::render {

namespace {
bool finite(float value) noexcept { return std::isfinite(value) != 0; }

bool valid_texture_description(const TextureDesc& description) noexcept {
    return description.width != 0 && description.height != 0 &&
        description.layers == 1 && description.mipLevels != 0 &&
        !description.format.empty() &&
        description.dimension == TextureDimension::Texture2D &&
        description.depth == 1 && description.sampleCount == 1 &&
        !description.resolve && description.resolveMode == TextureResolveMode::None &&
        !description.depthStencil;
}

bool same_handle(ResourceHandle left, ResourceHandle right) noexcept {
    return left && right && left.id == right.id && left.kind == right.kind;
}

bool same_texture_layout(const TextureDesc& left, const TextureDesc& right) noexcept {
    return left.width == right.width && left.height == right.height &&
        left.layers == right.layers && left.mipLevels == right.mipLevels &&
        left.format == right.format && left.hdr == right.hdr &&
        left.colorSpace == right.colorSpace && left.dimension == right.dimension &&
        left.depth == right.depth && left.formatKind == right.formatKind &&
        left.sampleCount == right.sampleCount && left.resolve == right.resolve &&
        left.resolveMode == right.resolveMode && left.depthStencil == right.depthStencil;
}

bool has_output_capabilities(const TextureDesc& description, bool compute) noexcept {
    return compute ? description.storage && !description.renderTarget
                   : description.renderTarget && !description.storage;
}

bool valid_stage_name(const std::string& name) noexcept {
    return !name.empty() && name.find_first_not_of(" \t\r\n") != std::string::npos;
}

void add_error(PostProcessValidationResult& result, const std::string& message) {
    result.errors.emplace_back(message);
}

void append_errors(PostProcessValidationResult& destination,
                   const PostProcessValidationResult& source) {
    destination.errors.insert(destination.errors.end(), source.errors.begin(), source.errors.end());
}

bool valid_stage_kind(PostProcessStageKind kind) noexcept {
    switch (kind) {
    case PostProcessStageKind::Custom:
    case PostProcessStageKind::Exposure:
    case PostProcessStageKind::Bloom:
    case PostProcessStageKind::SSAO:
    case PostProcessStageKind::TAA:
    case PostProcessStageKind::SSR:
    case PostProcessStageKind::ToneMap:
        return true;
    }
    return false;
}

PostProcessValidationResult validate_stage_sequence(
    const std::vector<PostProcessStageDesc>& stages) {
    PostProcessValidationResult result;
    std::unordered_map<std::string, std::size_t> stageIndices;
    stageIndices.reserve(stages.size());

    for (std::size_t index = 0; index < stages.size(); ++index) {
        const auto& stage = stages[index];
        append_errors(result, validate_post_process_stage(stage));
        if (!stageIndices.emplace(stage.name, index).second && !stage.name.empty()) {
            add_error(result, "post-process stage names must be unique");
        }
    }

    for (std::size_t index = 0; index < stages.size(); ++index) {
        const auto& stage = stages[index];
        std::unordered_set<std::string> dependencies;
        for (const auto& dependency : stage.dependsOn) {
            if (dependency.empty() || dependency.find_first_not_of(" \t\r\n") == std::string::npos) {
                add_error(result, "post-process stage dependency name must not be empty");
                continue;
            }
            if (!dependencies.emplace(dependency).second) {
                add_error(result, "post-process stage dependencies must be unique");
                continue;
            }
            if (dependency == stage.name) {
                add_error(result, "post-process stage cannot depend on itself");
                continue;
            }
            const auto found = stageIndices.find(dependency);
            if (found == stageIndices.end()) {
                add_error(result, "post-process stage depends on an unknown stage");
            } else if (found->second >= index) {
                add_error(result, "post-process stage dependency must refer to an earlier stage");
            }
        }
    }
    return result;
}

const TextureDesc* graph_texture_description(const RenderGraph& graph,
                                             ResourceHandle handle) noexcept {
    const auto found = std::find_if(graph.resources().begin(), graph.resources().end(),
        [handle](const RenderGraph::ResourceNode& resource) {
            return resource.handle.id == handle.id && resource.handle.kind == handle.kind;
        });
    if (found == graph.resources().end()) return nullptr;
    return std::get_if<TextureDesc>(&found->description);
}

bool graph_texture_matches(const RenderGraph& graph, ResourceHandle handle,
                           const TextureDesc& expected) noexcept {
    const auto* actual = graph_texture_description(graph, handle);
    return actual && same_texture_layout(*actual, expected);
}

void add_slot(PostProcessValidationResult& result, std::unordered_map<std::uint64_t, const char*>& slots,
              std::uint32_t slot, std::uint32_t space, const char* name) {
    const std::uint64_t key = (static_cast<std::uint64_t>(space) << 32u) | slot;
    const auto [found, inserted] = slots.emplace(key, name);
    if (!inserted) {
        add_error(result, std::string("post-process descriptor slots conflict between ") +
            found->second + " and " + name);
    }
}
}

PostProcessValidationResult validate_post_process_stage(const PostProcessStageDesc& stage) {
    PostProcessValidationResult result;
    if (!valid_stage_name(stage.name)) add_error(result, "stage name must not be empty");
    if (!stage.pipeline || stage.pipeline.kind != ResourceKind::Pipeline) add_error(result, "stage pipeline must be a pipeline resource");
    if (stage.sampler && stage.sampler.kind != ResourceKind::Sampler) add_error(result, "stage sampler must be a sampler resource");
    if (stage.historyTexture && stage.historyTexture.kind != ResourceKind::Texture2D) add_error(result, "history texture must be a 2D texture resource");
    if (stage.historyOutput && stage.historyOutput.kind != ResourceKind::Texture2D) add_error(result, "history output must be a 2D texture resource");
    if (stage.auxiliaryTexture && stage.auxiliaryTexture.kind != ResourceKind::Texture2D) add_error(result, "auxiliary texture must be a 2D texture resource");
    if (!valid_stage_kind(stage.kind)) add_error(result, "post-process stage kind is invalid");
    if (!valid_texture_description(stage.output)) add_error(result, "stage output description is invalid");
    else if (!has_output_capabilities(stage.output, stage.compute)) {
        add_error(result, stage.compute
            ? "compute stage output must be storage-capable and not a render target"
            : "graphics stage output must be a render target and not storage-only");
    }
    if (stage.historyTexture && !valid_texture_description(stage.historyDescription)) {
        add_error(result, "history texture description is invalid");
    }
    if (stage.historyTexture && valid_texture_description(stage.historyDescription) &&
        valid_texture_description(stage.output) &&
        !same_texture_layout(stage.historyDescription, stage.output)) {
        add_error(result, "history texture description must match the stage output layout");
    }
    if (stage.historyOutput && !valid_texture_description(stage.historyOutputDescription)) {
        add_error(result, "history output description is invalid");
    }
    if (stage.historyOutput && valid_texture_description(stage.historyOutputDescription) &&
        valid_texture_description(stage.output) &&
        (!same_texture_layout(stage.historyOutputDescription, stage.output) ||
         !has_output_capabilities(stage.historyOutputDescription, stage.compute))) {
        add_error(result, "history output description must match the stage output layout and access capabilities");
    }
    if (stage.auxiliaryTexture && !valid_texture_description(stage.auxiliaryDescription)) {
        add_error(result, "auxiliary texture description is invalid");
    }
    if (same_handle(stage.historyTexture, stage.historyOutput)) {
        add_error(result, "history texture and history output must be different resources");
    }
    if (!finite(stage.exposure) || stage.exposure <= 0.0f) add_error(result, "stage exposure must be finite and greater than zero");
    if (!finite(stage.blendFactor) || stage.blendFactor < 0.0f || stage.blendFactor > 1.0f) add_error(result, "stage blend factor must be in [0, 1]");

    std::unordered_map<std::uint64_t, const char*> slots;
    add_slot(result, slots, stage.textureSlot, 0, "sourceImage");
    if (stage.sampler) add_slot(result, slots, stage.samplerSlot, 0, "sourceSampler");
    if (stage.historyTexture) add_slot(result, slots, stage.historySlot, 0, "historyImage");
    if (stage.compute) add_slot(result, slots, stage.outputSlot, 0, "outputImage");
    if (stage.compute && stage.historyOutput) add_slot(result, slots, stage.historyOutputSlot, 0, "historyOutputImage");
    if (stage.useParameters) add_slot(result, slots, stage.parameterSlot, stage.parameterSpace, "postParameters");
    if (stage.auxiliaryTexture) add_slot(result, slots, stage.auxiliarySlot, 0, "auxiliaryImage");

    const auto is_default = [&stage] { return std::holds_alternative<std::monostate>(stage.parameters); };
    switch (stage.kind) {
    case PostProcessStageKind::Custom:
    case PostProcessStageKind::ToneMap:
        break;
    case PostProcessStageKind::Exposure: {
        if (!is_default() && !std::holds_alternative<ExposureStageParameters>(stage.parameters)) { add_error(result, "exposure stage parameters have the wrong type"); break; }
        const auto values = is_default() ? ExposureStageParameters{stage.exposure, 0.0f} : std::get<ExposureStageParameters>(stage.parameters);
        if (!finite(values.exposure) || values.exposure <= 0.0f || !finite(values.adaptationRate) || values.adaptationRate < 0.0f) add_error(result, "exposure parameters are outside their valid range");
        break;
    }
    case PostProcessStageKind::Bloom: {
        if (!is_default() && !std::holds_alternative<BloomStageParameters>(stage.parameters)) { add_error(result, "bloom stage parameters have the wrong type"); break; }
        const auto values = is_default() ? BloomStageParameters{} : std::get<BloomStageParameters>(stage.parameters);
        if (!finite(values.threshold) || values.threshold < 0.0f || !finite(values.knee) || values.knee < 0.0f || values.knee > 1.0f || !finite(values.intensity) || values.intensity < 0.0f || values.mipLevels == 0 || values.mipLevels > 12) add_error(result, "bloom parameters are outside their valid range");
        break;
    }
    case PostProcessStageKind::SSAO: {
        if (!is_default() && !std::holds_alternative<SsaoStageParameters>(stage.parameters)) { add_error(result, "SSAO stage parameters have the wrong type"); break; }
        const auto values = is_default() ? SsaoStageParameters{} : std::get<SsaoStageParameters>(stage.parameters);
        if (!finite(values.radius) || values.radius <= 0.0f || !finite(values.power) || values.power <= 0.0f || !finite(values.bias) || values.bias < 0.0f || values.sampleCount == 0 || values.sampleCount > 256) add_error(result, "SSAO parameters are outside their valid range");
        break;
    }
    case PostProcessStageKind::TAA: {
        if (!stage.historyTexture) add_error(result, "TAA stage requires a history texture");
        if (!is_default() && !std::holds_alternative<TaaStageParameters>(stage.parameters)) { add_error(result, "TAA stage parameters have the wrong type"); break; }
        const auto values = is_default() ? TaaStageParameters{stage.blendFactor, 0.0f, 1.0f} : std::get<TaaStageParameters>(stage.parameters);
        if (!finite(values.feedback) || values.feedback < 0.0f || values.feedback > 1.0f || !finite(values.sharpness) || values.sharpness < 0.0f || !finite(values.motionRejection) || values.motionRejection < 0.0f || values.motionRejection > 1.0f) add_error(result, "TAA parameters are outside their valid range");
        break;
    }
    case PostProcessStageKind::SSR: {
        if (!is_default() && !std::holds_alternative<SsrStageParameters>(stage.parameters)) { add_error(result, "SSR stage parameters have the wrong type"); break; }
        const auto values = is_default() ? SsrStageParameters{} : std::get<SsrStageParameters>(stage.parameters);
        if (!finite(values.maxDistance) || values.maxDistance <= 0.0f || !finite(values.thickness) || values.thickness < 0.0f || !finite(values.stride) || values.stride <= 0.0f || values.maxSteps == 0 || values.maxSteps > 1024) add_error(result, "SSR parameters are outside their valid range");
        break;
    }
    }
    return result;
}

PostProcessValidationResult validate_post_process_chain(const PostProcessChainDesc& chain) {
    PostProcessValidationResult result;
    if (!chain.source || chain.source.kind != ResourceKind::Texture2D) add_error(result, "post-process chain source must be a 2D texture resource");
    if (!valid_texture_description(chain.sourceDescription)) add_error(result, "post-process chain source description is invalid");
    else if (chain.sourceDescription.depthStencil) add_error(result, "post-process chain source must be a color texture");
    if (chain.finalTarget && chain.finalTarget.kind != ResourceKind::Texture2D) add_error(result, "post-process chain final target must be a 2D texture resource");
    if (chain.finalTarget && !valid_texture_description(chain.finalDescription)) {
        add_error(result, "post-process chain final target description is invalid");
    }
    if (chain.stages.size() > 0 && same_handle(chain.source, chain.finalTarget)) {
        add_error(result, "post-process chain source and final target must be different resources");
    }
    append_errors(result, validate_stage_sequence(chain.stages));
    return result;
}

std::size_t PostProcessPipeline::add_stage(PostProcessStageDesc stage) {
    stages_.push_back(std::move(stage));
    return stages_.size() - 1;
}

ResourceHandle PostProcessPipeline::build(RenderGraph& graph,
                                           ResourceHandle source,
                                           const TextureDesc& sourceDescription,
                                           ResourceHandle finalTarget,
    const TextureDesc& finalDescription) const {
    if (!source || source.kind != ResourceKind::Texture2D) return {};
    if (stages_.empty()) return source;
    if (finalTarget && finalTarget.kind != ResourceKind::Texture2D) return {};

    PostProcessChainDesc contract;
    contract.source = source;
    contract.sourceDescription = sourceDescription;
    contract.finalTarget = finalTarget;
    contract.finalDescription = finalDescription;
    contract.stages = stages_;
    if (!validate_post_process_chain(contract).valid()) return {};

    const auto& lastStage = stages_.back();
    if (finalTarget && !same_texture_layout(finalDescription, lastStage.output)) return {};

    if (graph.contains_resource(source)) {
        if (!graph_texture_matches(graph, source, sourceDescription)) return {};
    } else {
        graph.import_texture(source, sourceDescription);
        if (!graph.contains_resource(source) || !graph_texture_matches(graph, source, sourceDescription)) return {};
    }

    if (finalTarget && graph.contains_resource(finalTarget)) {
        const auto* targetDescription = graph_texture_description(graph, finalTarget);
        if (!targetDescription || !same_texture_layout(*targetDescription, lastStage.output) ||
            !has_output_capabilities(*targetDescription, lastStage.compute)) return {};
    }

    ResourceHandle current = source;
    for (std::size_t index = 0; index < stages_.size(); ++index) {
        const auto& stage = stages_[index];
        const bool last = index + 1 == stages_.size();
        const auto outputDescription = stage.output;
        const auto output = last && finalTarget ? finalTarget : graph.create_texture(outputDescription);
        if (!output) return {};
        if (same_handle(current, output) || same_handle(current, stage.historyTexture) ||
            same_handle(current, stage.historyOutput) || same_handle(current, stage.auxiliaryTexture) ||
            same_handle(output, stage.historyTexture) || same_handle(output, stage.historyOutput) ||
            same_handle(output, stage.auxiliaryTexture)) {
            return {};
        }
        if (last && finalTarget && !graph.contains_resource(finalTarget)) {
            graph.import_texture(finalTarget, finalDescription);
            if (!graph.contains_resource(finalTarget) ||
                !graph_texture_matches(graph, finalTarget, lastStage.output)) return {};
        }

        std::vector<DescriptorBinding> bindings;
        bindings.push_back({"sourceImage", current, DescriptorType::Texture, stage.textureSlot, 0, 1, false, {}});
        if (stage.sampler) {
            bindings.push_back({"sourceSampler", stage.sampler, DescriptorType::Sampler, stage.samplerSlot, 0, 1, false, {}});
        }
        if (stage.historyTexture) {
            if (!graph.contains_resource(stage.historyTexture)) {
                graph.import_texture(stage.historyTexture, stage.historyDescription);
            }
            if (!graph.contains_resource(stage.historyTexture) ||
                !graph_texture_matches(graph, stage.historyTexture, stage.historyDescription)) return {};
            bindings.push_back({"historyImage", stage.historyTexture, DescriptorType::Texture, stage.historySlot, 0, 1, false, {}});
        }
        if (stage.historyOutput && !graph.contains_resource(stage.historyOutput)) {
            graph.import_texture(stage.historyOutput, stage.historyOutputDescription);
        }
        if (stage.historyOutput && (!graph.contains_resource(stage.historyOutput) ||
            !graph_texture_matches(graph, stage.historyOutput, stage.historyOutputDescription))) {
            return {};
        }
        if (stage.historyOutput) {
            const auto* historyOutputDescription = graph_texture_description(graph, stage.historyOutput);
            if (!historyOutputDescription ||
                !has_output_capabilities(*historyOutputDescription, stage.compute)) return {};
        }
        if (stage.auxiliaryTexture) {
            if (!graph.contains_resource(stage.auxiliaryTexture)) {
                graph.import_texture(stage.auxiliaryTexture, stage.auxiliaryDescription);
            }
            if (!graph.contains_resource(stage.auxiliaryTexture) ||
                !graph_texture_matches(graph, stage.auxiliaryTexture, stage.auxiliaryDescription)) return {};
        }
        ResourceHandle parameters{};
        BufferDesc parameterDescription{};
        if (stage.useParameters) {
            struct Parameters {
                float exposure;
                float blendFactor;
                float inverseWidth;
                float inverseHeight;
                float stageParameters[4];
            } values{stage.exposure, stage.blendFactor,
                1.0f / static_cast<float>(std::max(1u, outputDescription.width)),
                1.0f / static_cast<float>(std::max(1u, outputDescription.height)), {0.0f, 0.0f, 0.0f, 0.0f}};
            if (const auto* exposure = std::get_if<ExposureStageParameters>(&stage.parameters)) {
                values.stageParameters[0] = exposure->exposure;
                values.stageParameters[1] = exposure->adaptationRate;
            } else if (const auto* bloom = std::get_if<BloomStageParameters>(&stage.parameters)) {
                values.stageParameters[0] = bloom->threshold;
                values.stageParameters[1] = bloom->knee;
                values.stageParameters[2] = bloom->intensity;
                values.stageParameters[3] = static_cast<float>(bloom->mipLevels);
            } else if (const auto* ssao = std::get_if<SsaoStageParameters>(&stage.parameters)) {
                values.stageParameters[0] = ssao->radius;
                values.stageParameters[1] = ssao->power;
                values.stageParameters[2] = ssao->bias;
                values.stageParameters[3] = static_cast<float>(ssao->sampleCount);
            } else if (const auto* taa = std::get_if<TaaStageParameters>(&stage.parameters)) {
                values.stageParameters[0] = taa->feedback;
                values.stageParameters[1] = taa->sharpness;
                values.stageParameters[2] = taa->motionRejection;
            } else if (const auto* ssr = std::get_if<SsrStageParameters>(&stage.parameters)) {
                values.stageParameters[0] = ssr->maxDistance;
                values.stageParameters[1] = ssr->thickness;
                values.stageParameters[2] = ssr->stride;
                values.stageParameters[3] = static_cast<float>(ssr->maxSteps);
            }
            std::vector<std::uint8_t> bytes(sizeof(values));
            std::memcpy(bytes.data(), &values, sizeof(values));
            parameterDescription = {sizeof(values), sizeof(float) * 8, false, false, std::move(bytes)};
            parameters = graph.create_buffer(parameterDescription);
            if (!parameters) return {};
            bindings.push_back({"postParameters", parameters, DescriptorType::UniformBuffer,
                stage.parameterSlot, stage.parameterSpace, 1, false, {}});
        }
        if (stage.compute) bindings.push_back({"outputImage", output, DescriptorType::StorageTexture, stage.outputSlot, 0, 1, false, {}});
        if (stage.compute && stage.historyOutput) {
            bindings.push_back({"historyOutputImage", stage.historyOutput, DescriptorType::StorageTexture,
                stage.historyOutputSlot, 0, 1, false, {}});
        }
        if (stage.auxiliaryTexture) {
            bindings.push_back({"auxiliaryImage", stage.auxiliaryTexture, DescriptorType::Texture,
                stage.auxiliarySlot, 0, 1, false, {}});
        }
        const auto material = graph.create_material({stage.name + "_material", stage.pipeline, std::move(bindings), false});
        if (!material) return {};
        std::vector<ResourceAccess> accesses{
            {current, ResourceUsage::ShaderRead},
            {material, ResourceUsage::ShaderRead},
            {output, stage.compute ? ResourceUsage::StorageWrite : ResourceUsage::ColorAttachment}};
        if (stage.historyTexture) accesses.push_back({stage.historyTexture, ResourceUsage::ShaderRead});
        if (stage.historyOutput) {
            accesses.push_back({stage.historyOutput,
                stage.compute ? ResourceUsage::StorageWrite : ResourceUsage::ColorAttachment1});
        }
        if (stage.auxiliaryTexture) accesses.push_back({stage.auxiliaryTexture, ResourceUsage::ShaderRead});
        if (parameters) accesses.push_back({parameters, ResourceUsage::UniformBuffer});
        const auto outputWidth = std::max(1u, outputDescription.width);
        const auto outputHeight = std::max(1u, outputDescription.height);
        graph.add_pass(stage.name, std::move(accesses), [material, current, output, parameters,
            outputWidth, outputHeight, stage](auto& backend, const auto&) {
            backend.bind_material(material);
            if (stage.useParameters) {
                backend.bind_uniform_buffer(parameters, stage.parameterSlot, stage.parameterSpace);
            }
            if (stage.compute) {
                if (!backend.dispatch({(outputWidth + 7u) / 8u, (outputHeight + 7u) / 8u, 1u})) return;
            } else {
                backend.draw_sprite({current, {0.0f, 0.0f}, {1.0f, 1.0f}, 0.0f});
            }
        }, stage.compute ? RenderQueue::Compute : RenderQueue::Graphics);
        current = output;
    }
    return current;
}

ResourceHandle PostProcessPipeline::build(RenderGraph& graph, const PostProcessChainDesc& chain) const {
    if (!validate_post_process_chain(chain).valid()) return {};
    PostProcessPipeline pipeline;
    for (auto stage : chain.stages) pipeline.add_stage(std::move(stage));
    return pipeline.build(graph, chain.source, chain.sourceDescription, chain.finalTarget, chain.finalDescription);
}
}

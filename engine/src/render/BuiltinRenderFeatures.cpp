#include "shinkou/render/BuiltinRenderFeatures.h"
#include "BuiltinShaderSources.h"
#include <algorithm>
#include <cstring>
#include <fstream>

namespace shinkou::render {
namespace {
using Stage = RenderFeatureGraphStage;
constexpr Stage imageStages[]{Stage::SSAO,     Stage::SSR,     Stage::TAA,     Stage::Bloom,
                              Stage::Exposure, Stage::ToneMap, Stage::Present, Stage::MotionBlur};
constexpr const char *imageDefines[]{"SSAO",     "SSR",      "TAA",     "BLOOM",
                                     "EXPOSURE", "TONE_MAP", "PRESENT", "MOTION_BLUR"};
constexpr const char *expandedDefines[]{"BLOOM_PREFILTER",    "BLOOM_DOWN",
                                        "BLOOM_UP",           "EXPOSURE_TEMPORAL",
                                        "EXPOSURE_HISTOGRAM", "EXPOSURE_HISTOGRAM_TEMPORAL"};
struct alignas(16) ImageParameters {
    math::Vec4 settings{1, 0, 0, 0};
    math::Vec4 viewport{};
    math::Mat4 inverseProjection{math::Mat4::Identity()};
    math::Mat4 projection{math::Mat4::Identity()};
    math::Mat4 reprojection{math::Mat4::Identity()};
    math::Vec4 tuning{};
    math::Vec4 temporal{};
    math::Vec4 metering{};
};
static_assert(sizeof(ImageParameters) == 272);
ShaderDesc image_shader(BackendApi api, ShaderStage stage, const char *define) {
    ShaderDesc shader;
    shader.stage = stage;
    shader.name = std::string("builtin_") + define;
    shader.profile = stage == ShaderStage::Compute  ? "cs_5_0"
                     : stage == ShaderStage::Vertex ? "vs_5_0"
                                                    : "ps_5_0";
    if (api == BackendApi::Vulkan) {
        std::ifstream stream(std::string(SHINKOU_BUILTIN_SHADER_OUTPUT_DIR) + "/" + define + ".spv",
                             std::ios::binary);
        shader.bytecode.assign(std::istreambuf_iterator<char>(stream), {});
        shader.sourceKind = ShaderSourceKind::SpirV;
    } else
        shader.source = std::string("#define SHINKOU_") + define + " 1\n" +
                        std::string(stage == ShaderStage::Compute ? detail::histogramSource
                                                                  : detail::postSource);
    return shader;
}
BufferDesc uniform(const ImageParameters &values) {
    BufferDesc buffer;
    buffer.size = sizeof(values);
    buffer.stride = 16;
    buffer.uniformBuffer = true;
    buffer.initialData.resize(sizeof(values));
    std::memcpy(buffer.initialData.data(), &values, sizeof(values));
    return buffer;
}
void append(std::vector<ResourceAccess> &accesses, ResourceHandle handle, ResourceUsage usage) {
    if (std::none_of(accesses.begin(), accesses.end(),
                     [&](const auto &value) { return value.resource.id == handle.id; }))
        accesses.push_back({handle, usage});
}
} // namespace
ShaderDesc builtin_fullscreen_vertex_shader(BackendApi api) {
    return image_shader(api, ShaderStage::Vertex, "VERTEX");
}

bool BuiltinRenderFeatures::initialize(Renderer &renderer, std::string presentationFormat) {
    release(renderer);
    error_.clear();
    const auto own = [&](ResourceHandle handle) {
        if (handle)
            owned_.push_back(handle);
        else if (error_.empty())
            error_ = renderer.last_error();
        return handle;
    };
    samplerDescription_.addressU = samplerDescription_.addressV = samplerDescription_.addressW =
        "clamp";
    sampler_ = own(renderer.create_sampler(samplerDescription_));
    blackDescription_.initialData = {0, 0, 0, 255};
    whiteDescription_.initialData = {255, 255, 255, 255};
    black_ = own(renderer.create_texture(blackDescription_));
    white_ = own(renderer.create_texture(whiteDescription_));
    const auto vertex = own(renderer.create_shader(
        image_shader(renderer.capabilities().api, ShaderStage::Vertex, "VERTEX")));
    const auto program = [&](const char *define, PipelineDesc &desc, bool mrt) {
        const auto fragment = own(renderer.create_shader(
            image_shader(renderer.capabilities().api, ShaderStage::Fragment, define)));
        desc.name = std::string("builtin_") + define;
        desc.vertexShader = vertex.id;
        desc.fragmentShader = fragment.id;
        desc.depthFormat = "none";
        desc.depthTest = desc.depthWrite = desc.vertexInput = false;
        desc.cullMode = "none";
        if (mrt)
            desc.colorFormats = {"rgba16f", "rgba16f"};
        return fragment ? own(renderer.create_pipeline(desc)) : ResourceHandle{};
    };
    if (!vertex || !sampler_ || !black_ || !white_) {
        release(renderer);
        return false;
    }
    for (std::size_t i = 0; i < pipelines_.size(); ++i) {
        auto &desc = pipelineDescriptions_[i];
        desc = {};
        desc.colorFormat = i == 0 || i == 5 ? "rgba8" : "rgba16f";
        if (imageStages[i] == Stage::Present)
            desc.colorFormat = presentationFormat;
        pipelines_[i] = program(imageDefines[i], desc, imageStages[i] == Stage::TAA);
        if (!pipelines_[i]) {
            release(renderer);
            return false;
        }
    }
    for (std::size_t i = 0; i < expandedPipelines_.size(); ++i) {
        auto &desc = expandedDescriptions_[i];
        desc = {};
        desc.colorFormat = "rgba16f";
        expandedPipelines_[i] = program(expandedDefines[i], desc, i == 3 || i == 5);
        if (!expandedPipelines_[i]) {
            release(renderer);
            return false;
        }
    }
    if (renderer.capabilities().supportsCompute)
        for (std::size_t i = 0; i < histogramPipelines_.size(); ++i) {
            auto &desc = histogramDescriptions_[i];
            desc = {};
            desc.name = i == 0 ? "HISTOGRAM_CLEAR" : "HISTOGRAM_BUILD";
            const auto compute = own(renderer.create_shader(image_shader(
                renderer.capabilities().api, ShaderStage::Compute, desc.name.c_str())));
            desc.computeShader = compute.id;
            desc.depthTest = desc.depthWrite = false;
            desc.depthFormat = "none";
            histogramPipelines_[i] =
                compute ? own(renderer.create_pipeline(desc)) : ResourceHandle{};
            if (!histogramPipelines_[i]) {
                release(renderer);
                return false;
            }
        }
    return true;
}
void BuiltinRenderFeatures::release(Renderer &renderer) {
    for (const auto kind : {ResourceKind::Material, ResourceKind::Pipeline, ResourceKind::Texture2D,
                            ResourceKind::Sampler, ResourceKind::Shader})
        for (auto it = owned_.rbegin(); it != owned_.rend(); ++it)
            if (it->kind == kind)
                renderer.destroy_resource(*it);
    owned_.clear();
    pipelines_ = {};
    expandedPipelines_ = {};
    histogramPipelines_ = {};
    sampler_ = black_ = white_ = {};
}

bool BuiltinRenderFeatures::install(RenderFeatureGraphDesc &description) const {
    if (!pipelines_[6])
        return false;
    description.preferComputePostProcess = false;
    for (std::size_t i = 0; i < pipelines_.size(); ++i) {
        if (std::any_of(description.executors.begin(), description.executors.end(),
                        [&](const auto &existing) {
                            return existing.stage == imageStages[i] && existing.name.empty();
                        }))
            continue;
        RenderFeatureGraphExecutor executor;
        executor.stage = imageStages[i];
        executor.prepare = [stage = imageStages[i], basePipeline = pipelines_[i],
                            baseDescription = pipelineDescriptions_[i], extra = expandedPipelines_,
                            extraDescriptions = expandedDescriptions_, sampler = sampler_,
                            samplerDescription = samplerDescription_,
                            histograms = histogramPipelines_,
                            histogramDescriptions = histogramDescriptions_, black = black_,
                            blackDescription = blackDescription_, white = white_,
                            whiteDescription = whiteDescription_, view = description.view,
                            previous = description.previousViewProjection,
                            historyValid = description.historyValid,
                            exposureValid = description.exposureHistoryValid,
                            delta = description.deltaSeconds](
                               RenderGraph &graph, const RenderFeatureGraphStageDesc &contract) {
            RenderFeatureGraphCommands commands;
            if (contract.inputs.empty()) {
                commands.error = "image stage has no source";
                return commands;
            }
            const auto &config = contract.postProcess;
            if (!validate_post_process_parameters(config, commands.error))
                return commands;
            graph.import_resource(sampler, samplerDescription);
            graph.import_texture(black, blackDescription);
            graph.import_texture(white, whiteDescription);
            auto pipeline = basePipeline;
            auto pipelineDescription = baseDescription;
            auto source = contract.inputs.front().handle;
            auto sourceDesc = std::get<TextureDesc>(contract.inputs.front().description);
            ImageParameters values;
            ResourceHandle histogram;
            values.viewport = {1.0f / sourceDesc.width, 1.0f / sourceDesc.height,
                               historyValid ? 1.0f : 0.0f, 0};
            values.temporal.x = delta;
            values.projection = view.projectionMatrix;
            math::Mat4 inverseViewProjection;
            if (!math::Inverse(view.projectionMatrix, values.inverseProjection) ||
                !math::Inverse(view.viewProjection, inverseViewProjection)) {
                commands.error = "image stage camera matrices are singular";
                return commands;
            }
            values.reprojection = math::Multiply(previous, inverseViewProjection);
            const auto input = [&](const char *name, ResourceHandle fallback) {
                const auto found =
                    std::find_if(contract.inputs.begin(), contract.inputs.end(),
                                 [&](const auto &item) { return item.name == name; });
                return found == contract.inputs.end() ? fallback : found->handle;
            };
            const auto motion = input("motion_vectors", {});
            values.temporal.z = motion ? 1.0f : 0.0f;
            if (stage == Stage::SSAO) {
                const auto *configured = std::get_if<SsaoStageParameters>(&config.parameters);
                const auto p = configured ? *configured : SsaoStageParameters{};
                if (p.sampleCount > 64) {
                    commands.error = "built-in SSAO supports at most 64 samples";
                    return commands;
                }
                values.settings = {p.radius, p.power, p.bias, static_cast<float>(p.sampleCount)};
            } else if (stage == Stage::SSR) {
                const auto *configured = std::get_if<SsrStageParameters>(&config.parameters);
                const auto p = configured ? *configured : SsrStageParameters{};
                if (p.maxSteps > 256) {
                    commands.error = "built-in SSR supports at most 256 steps";
                    return commands;
                }
                values.settings = {p.maxDistance, p.thickness, p.stride,
                                   static_cast<float>(p.maxSteps)};
            } else if (stage == Stage::TAA) {
                const auto *configured = std::get_if<TaaStageParameters>(&config.parameters);
                const auto p = configured ? *configured : TaaStageParameters{};
                values.settings = {p.feedback, p.sharpness, p.motionRejection, p.depthRejection};
            } else if (stage == Stage::MotionBlur) {
                const auto *configured = std::get_if<MotionBlurStageParameters>(&config.parameters);
                const auto p = configured ? *configured : MotionBlurStageParameters{};
                if (!motion) {
                    commands.error = "motion blur requires motion vectors";
                    return commands;
                }
                values.settings = {p.shutterFraction, p.maxRadiusPixels, p.depthRejection,
                                   static_cast<float>(p.sampleCount)};
            } else if (stage == Stage::Exposure) {
                const auto *configured = std::get_if<ExposureStageParameters>(&config.parameters);
                const auto p =
                    configured ? *configured : ExposureStageParameters{config.exposure, 0};
                values.settings = {p.exposure, p.adaptationRate, p.darkAdaptationRate,
                                   p.compensation};
                values.tuning = {p.minExposure, p.maxExposure, p.middleGray, 0};
                values.viewport.z = exposureValid ? 1.0f : 0.0f;
                if (p.adaptationRate > 0) {
                    pipeline = extra[3];
                    pipelineDescription = extraDescriptions[3];
                }
                if (p.metering == ExposureMetering::Histogram) {
                    if (!histograms[0] || !histograms[1]) {
                        commands.error = "histogram exposure requires compute support";
                        return commands;
                    }
                    values.metering = {p.minLogLuminance, p.maxLogLuminance, p.lowPercentile,
                                       p.highPercentile};
                    BufferDesc bins;
                    bins.size = 256 * 4;
                    bins.stride = 4;
                    bins.structuredBuffer = bins.storageBuffer = true;
                    histogram = graph.create_buffer(bins);
                    const auto parameters = graph.create_buffer(uniform(values));
                    for (std::size_t step = 0; step < 2; ++step) {
                        graph.import_resource(histograms[step], histogramDescriptions[step]);
                        std::vector<DescriptorBinding> bindings{
                            {"HistogramOutput", histogram, DescriptorType::StorageBuffer, 0}};
                        std::vector<ResourceAccess> accesses{
                            {histogram, ResourceUsage::StorageWrite},
                            {histograms[step], ResourceUsage::ShaderRead}};
                        if (step) {
                            bindings.push_back({"SourceImage", source, DescriptorType::Texture, 1});
                            bindings.push_back(
                                {"Parameters", parameters, DescriptorType::UniformBuffer, 4});
                            accesses.push_back({source, ResourceUsage::ShaderRead});
                            accesses.push_back({parameters, ResourceUsage::UniformBuffer});
                        }
                        const auto name =
                            contract.name + (step ? "/histogram_build" : "/histogram_clear");
                        const auto material = graph.create_material(
                            {name, histograms[step], std::move(bindings), false});
                        accesses.push_back({material, ResourceUsage::ShaderRead});
                        const auto pass = graph.add_pass(name, std::move(accesses), {},
                                                         RenderQueue::Compute, false);
                        const DispatchDesc dispatch{step ? (sourceDesc.width + 15) / 16 : 1u,
                                                    step ? (sourceDesc.height + 15) / 16 : 1u, 1};
                        graph.set_command_recorder(pass,
                                                   [material, dispatch](auto &list, const auto &) {
                                                       list.bind_material(material);
                                                       list.dispatch(dispatch);
                                                   });
                        commands.expandedPasses.push_back(pass);
                    }
                    const auto index = p.adaptationRate > 0 ? 5u : 4u;
                    pipeline = extra[index];
                    pipelineDescription = extraDescriptions[index];
                }
            } else if (stage == Stage::ToneMap)
                values.settings.x = config.exposure;
            else if (stage == Stage::Bloom) {
                const auto *configured = std::get_if<BloomStageParameters>(&config.parameters);
                const auto p = configured ? *configured : BloomStageParameters{};
                values.settings = {p.threshold, p.knee, p.intensity,
                                   static_cast<float>(p.mipLevels)};
                values.tuning.x = p.scatter;
                std::vector<ResourceHandle> levels;
                std::vector<TextureDesc> layouts;
                const auto add_bloom_pass = [&](const std::string &name, ResourceHandle target,
                                                ResourceHandle high, ResourceHandle low,
                                                std::size_t program,
                                                const TextureDesc &inputLayout) {
                    graph.import_resource(extra[program], extraDescriptions[program]);
                    auto params = values;
                    params.viewport.x = 1.0f / inputLayout.width;
                    params.viewport.y = 1.0f / inputLayout.height;
                    const auto buffer = graph.create_buffer(uniform(params));
                    std::vector<DescriptorBinding> bindings{
                        {"Source", high, DescriptorType::Texture, 0},
                        {"LinearClamp", sampler, DescriptorType::Sampler, 1},
                        {"Parameters", buffer, DescriptorType::UniformBuffer, 4}};
                    std::vector<ResourceAccess> accesses{
                        {target, ResourceUsage::ColorAttachment},
                        {high, ResourceUsage::ShaderRead},
                        {sampler, ResourceUsage::ShaderRead},
                        {buffer, ResourceUsage::UniformBuffer},
                        {extra[program], ResourceUsage::ShaderRead}};
                    if (low) {
                        bindings.push_back({"Secondary", low, DescriptorType::Texture, 3});
                        append(accesses, low, ResourceUsage::ShaderRead);
                    }
                    const auto material =
                        graph.create_material({name, extra[program], std::move(bindings), false});
                    accesses.push_back({material, ResourceUsage::ShaderRead});
                    const auto index =
                        graph.add_pass(name, std::move(accesses), {}, RenderQueue::Graphics, false);
                    graph.set_command_recorder(index, [material](auto &list, const auto &) {
                        list.bind_material(material);
                        list.draw_sprite({});
                    });
                    commands.expandedPasses.push_back(index);
                };
                auto inputLayout = sourceDesc;
                auto high = source;
                for (std::uint32_t level = 0; level < p.mipLevels; ++level) {
                    auto layout = inputLayout;
                    layout.width = std::max(1u, inputLayout.width / 2);
                    layout.height = std::max(1u, inputLayout.height / 2);
                    layout.format = "rgba16f";
                    layout.formatKind = TextureFormat::Unknown;
                    layout.hdr = true;
                    layout.renderTarget = true;
                    layout.storage = false;
                    layout.initialData.clear();
                    layout.initialSubresources.clear();
                    layout.mipLevels = 1;
                    const auto target = graph.create_texture(layout);
                    add_bloom_pass(contract.name + "/down_" + std::to_string(level), target, high,
                                   {}, level == 0 ? 0 : 1, inputLayout);
                    levels.push_back(target);
                    layouts.push_back(layout);
                    high = target;
                    inputLayout = layout;
                    if (layout.width == 1 && layout.height == 1)
                        break;
                }
                for (std::size_t level = levels.size(); level > 1; --level) {
                    const auto target = graph.create_texture(layouts[level - 2]);
                    add_bloom_pass(contract.name + "/up_" + std::to_string(level - 2), target,
                                   levels[level - 2], high, 2, inputLayout);
                    high = target;
                    inputLayout = layouts[level - 2];
                }
                source = high;
                sourceDesc = inputLayout;
                values.viewport.x = 1.0f / sourceDesc.width;
                values.viewport.y = 1.0f / sourceDesc.height;
            }
            for (const auto &output : contract.outputs) {
                const auto *texture = std::get_if<TextureDesc>(&output.description);
                if (!texture) {
                    commands.error = "built-in image stages require texture outputs";
                    return commands;
                }
                const auto attachment = static_cast<unsigned>(output.usage) -
                                        static_cast<unsigned>(ResourceUsage::ColorAttachment);
                const auto &formats = pipelineDescription.colorFormats;
                const auto expected = attachment < formats.size() ? formats[attachment]
                                                                  : pipelineDescription.colorFormat;
                if (texture->format != expected ||
                    (stage == Stage::Exposure && (texture->width != 1 || texture->height != 1))) {
                    commands.error =
                        "built-in image output format/dimensions do not match its native pipeline";
                    return commands;
                }
            }
            graph.import_resource(pipeline, pipelineDescription);
            commands.accesses = {{pipeline, ResourceUsage::ShaderRead},
                                 {sampler, ResourceUsage::ShaderRead}};
            const auto declare = [&](ResourceHandle handle, ResourceUsage usage) {
                if (std::none_of(contract.inputs.begin(), contract.inputs.end(),
                                 [&](const auto &item) { return item.handle.id == handle.id; }))
                    append(commands.accesses, handle, usage);
            };
            declare(source, ResourceUsage::ShaderRead);
            std::vector<DescriptorBinding> bindings;
            if (!histogram)
                bindings = {{"Source", source, DescriptorType::Texture, 0},
                            {"LinearClamp", sampler, DescriptorType::Sampler, 1}};
            else {
                bindings.push_back(
                    {"LuminanceHistogram", histogram, DescriptorType::StructuredBuffer, 9});
                commands.accesses.push_back({histogram, ResourceUsage::ShaderRead});
                if (pipeline.id == extra[5].id)
                    bindings.push_back({"LinearClamp", sampler, DescriptorType::Sampler, 1});
            }
            const auto texture = [&](const char *name, ResourceHandle handle, unsigned slot) {
                bindings.push_back({name, handle, DescriptorType::Texture, slot});
                declare(handle, ResourceUsage::ShaderRead);
            };
            if (stage == Stage::SSR || stage == Stage::TAA || stage == Stage::MotionBlur)
                texture("Depth", input("depth", white), 2);
            if (stage == Stage::TAA) {
                texture("History", input("taa_history", black), 3);
                texture("MotionVectors", motion ? motion : black, 8);
            }
            if (stage == Stage::MotionBlur)
                texture("MotionVectors", motion, 8);
            if (stage == Stage::Exposure &&
                (pipeline.id == extra[3].id || pipeline.id == extra[5].id))
                texture("History", input("exposure_history", black), 3);
            if (stage == Stage::ToneMap) {
                texture("Bloom", input("bloom", black), 5);
                texture("Exposure", input("exposure", white), 6);
                texture("AmbientOcclusion", input("ssao", white), 7);
            }
            if (stage != Stage::Present) {
                const auto buffer = graph.create_buffer(uniform(values));
                bindings.push_back({"Parameters", buffer, DescriptorType::UniformBuffer, 4});
                commands.accesses.push_back({buffer, ResourceUsage::UniformBuffer});
            }
            const auto material = graph.create_material(
                {"builtin_" + contract.name, pipeline, std::move(bindings), false});
            commands.accesses.push_back({material, ResourceUsage::ShaderRead});
            const bool backbuffer = stage == Stage::Present && contract.outputs.empty();
            commands.record = [material, backbuffer](auto &backend, const auto &) {
                if (backbuffer)
                    backend.set_render_targets(ResourceHandle{}, ResourceHandle{}, false);
                backend.bind_material(material);
                backend.draw_sprite({});
            };
            commands.recordCommands = [material, backbuffer](auto &list, const auto &) {
                if (backbuffer)
                    list.set_render_targets({});
                list.bind_material(material);
                list.draw_sprite({});
            };
            return commands;
        };
        description.executors.push_back(std::move(executor));
    }
    return true;
}

bool TemporalRenderHistory::resize(Renderer &renderer, std::uint32_t width, std::uint32_t height) {
    if (!width || !height || width > 16384 || height > 16384)
        return false;
    if (textures_[0] && description_.width == width && description_.height == height)
        return true;
    TextureDesc description;
    description.width = width;
    description.height = height;
    description.format = "rgba16f";
    description.hdr = true;
    description.renderTarget = true;
    const auto first = renderer.create_texture(description);
    const auto second = renderer.create_texture(description);
    if (!first || !second) {
        if (first)
            renderer.destroy_resource(first);
        if (second)
            renderer.destroy_resource(second);
        return false;
    }
    release(renderer);
    textures_ = {first, second};
    description_ = description;
    return true;
}
void TemporalRenderHistory::release(Renderer &renderer) {
    for (const auto texture : textures_)
        if (texture)
            renderer.destroy_resource(texture);
    textures_ = {};
    valid_ = false;
    readIndex_ = 0;
}
void TemporalRenderHistory::configure(PostProcessStageDesc &stage,
                                      PostProcessStageKind kind) const {
    stage.kind = kind;
    stage.historyTexture = input();
    stage.historyOutput = output();
    stage.historyDescription = stage.historyOutputDescription = description_;
}
} // namespace shinkou::render

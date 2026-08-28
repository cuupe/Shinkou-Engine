#include "shinkou/render/Renderer.h"
#if defined(SHINKOU_WITH_UIKIT)
#include "shinkou/uikit/Render.h"
#endif
#include "shinkou/render/ShaderCompiler.h"
#include <algorithm>
#include <string>
#include <unordered_set>
#include <limits>

namespace shinkou::render {
namespace {
std::string pipeline_cache_key(const PipelineDesc& description) {
    std::string key;
    key += std::to_string(description.vertexShader);
    key.push_back(':');
    key += std::to_string(description.fragmentShader);
    key.push_back(':');
    key += std::to_string(description.computeShader);
    key.push_back(':');
    key += description.depthTest ? '1' : '0';
    key += description.depthWrite ? '1' : '0';
    key += description.alphaBlend ? '1' : '0';
    key += description.vertexInput ? '1' : '0';
    key.push_back(':');
    key += description.cullMode;
    key.push_back(':');
    key += description.fillMode;
    key.push_back(':');
    key += description.topology;
    key.push_back(':');
    key += description.colorFormat;
    key.push_back(':');
    key += description.depthFormat;
    key.push_back(':');
    key += std::to_string(description.sampleCount);
    key.push_back(':');
    for (const auto& format : description.colorFormats) {
        key += format;
        key.push_back(',');
    }
    return key;
}

std::string shader_cache_key(const ShaderDesc& description, BackendApi backend) {
    return std::to_string(shader_variant_hash(description, backend));
}

void retain_buffer_update(BufferDesc& description, const BufferUpdate& update) {
    if (!description.retainCpuCopy) return;
    if (description.initialData.size() != description.size) description.initialData.resize(description.size, 0);
    std::copy(update.data.begin(), update.data.end(), description.initialData.begin() + static_cast<std::ptrdiff_t>(update.offset));
}

std::size_t texture_bytes_per_pixel(std::string_view format) {
    return format == "rgba16f" ? 8u : 4u;
}

bool retain_cpu_generated_mips(TextureDesc& description) {
    if (!description.retainCpuCopy || description.format != "rgba8" || description.layers != 1 || description.mipLevels <= 1) return false;
    const auto baseWidth = description.width;
    const auto baseHeight = description.height;
    const auto baseSize = static_cast<std::size_t>(baseWidth) * baseHeight * 4u;
    std::vector<std::uint8_t> source;
    if (description.initialData.size() == baseSize) source = description.initialData;
    else {
        const auto base = std::find_if(description.initialSubresources.begin(), description.initialSubresources.end(),
            [](const TextureDesc::SubresourceData& subresource) { return subresource.mipLevel == 0 && subresource.layer == 0; });
        if (base == description.initialSubresources.end() || base->width != baseWidth || base->height != baseHeight ||
            base->rowPitch < static_cast<std::size_t>(baseWidth) * 4u || base->data.size() < static_cast<std::size_t>(baseHeight) * base->rowPitch) return false;
        source.resize(baseSize);
        for (std::uint32_t row = 0; row < baseHeight; ++row) {
            std::copy_n(base->data.begin() + static_cast<std::ptrdiff_t>(row * base->rowPitch),
                static_cast<std::size_t>(baseWidth) * 4u, source.begin() + static_cast<std::ptrdiff_t>(row * baseWidth * 4u));
        }
    }
    auto sourceWidth = baseWidth;
    auto sourceHeight = baseHeight;
    for (std::uint32_t level = 1; level < description.mipLevels; ++level) {
        const auto destinationWidth = std::max(1u, sourceWidth / 2u);
        const auto destinationHeight = std::max(1u, sourceHeight / 2u);
        std::vector<std::uint8_t> destination(static_cast<std::size_t>(destinationWidth) * destinationHeight * 4u);
        for (std::uint32_t y = 0; y < destinationHeight; ++y) {
            for (std::uint32_t x = 0; x < destinationWidth; ++x) {
                for (std::uint32_t channel = 0; channel < 4; ++channel) {
                    std::uint32_t sum = 0;
                    for (std::uint32_t sampleY = 0; sampleY < 2; ++sampleY) {
                        for (std::uint32_t sampleX = 0; sampleX < 2; ++sampleX) {
                            const auto sourceX = std::min(sourceWidth - 1u, x * 2u + sampleX);
                            const auto sourceY = std::min(sourceHeight - 1u, y * 2u + sampleY);
                            sum += source[(static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) * 4u + channel];
                        }
                    }
                    destination[(static_cast<std::size_t>(y) * destinationWidth + x) * 4u + channel] = static_cast<std::uint8_t>(sum / 4u);
                }
            }
        }
        const auto existing = std::find_if(description.initialSubresources.begin(), description.initialSubresources.end(),
            [level](const TextureDesc::SubresourceData& subresource) { return subresource.mipLevel == level && subresource.layer == 0; });
        TextureDesc::SubresourceData generated{level, 0, destinationWidth, destinationHeight,
            static_cast<std::size_t>(destinationWidth) * 4u, destination};
        if (existing == description.initialSubresources.end()) description.initialSubresources.push_back(std::move(generated));
        else *existing = std::move(generated);
        source = std::move(destination);
        sourceWidth = destinationWidth;
        sourceHeight = destinationHeight;
    }
    return true;
}

void retain_texture_update(TextureDesc& description, const TextureUpdate& update) {
    if (!description.retainCpuCopy) return;
    const auto mipWidth = std::max(1u, description.width >> update.mipLevel);
    const auto mipHeight = std::max(1u, description.height >> update.mipLevel);
    const auto bytesPerPixel = texture_bytes_per_pixel(description.format);
    const auto fullPitch = static_cast<std::size_t>(mipWidth) * bytesPerPixel;
    const auto existing = std::find_if(description.initialSubresources.begin(), description.initialSubresources.end(),
        [&](const TextureDesc::SubresourceData& subresource) {
            return subresource.mipLevel == update.mipLevel && subresource.layer == update.layer;
        });
    TextureDesc::SubresourceData retained;
    retained.mipLevel = update.mipLevel;
    retained.layer = update.layer;
    retained.width = mipWidth;
    retained.height = mipHeight;
    retained.rowPitch = fullPitch;
    retained.data.assign(static_cast<std::size_t>(mipHeight) * fullPitch, 0);
    if (update.mipLevel == 0 && update.layer == 0 && description.initialData.size() == retained.data.size()) {
        retained.data = description.initialData;
    }
    if (existing != description.initialSubresources.end() && existing->width == mipWidth && existing->height == mipHeight &&
        existing->rowPitch >= fullPitch && existing->data.size() >= static_cast<std::size_t>(mipHeight) * existing->rowPitch) {
        for (std::uint32_t row = 0; row < mipHeight; ++row) {
            std::copy_n(existing->data.begin() + static_cast<std::ptrdiff_t>(row * existing->rowPitch), fullPitch,
                retained.data.begin() + static_cast<std::ptrdiff_t>(row * fullPitch));
        }
    }
    const auto sourcePitch = update.rowPitch == 0 ? static_cast<std::size_t>(update.width) * bytesPerPixel : update.rowPitch;
    if (update.mipLevel == 0 && update.layer == 0 && bytesPerPixel == 4u) {
        const auto basePitch = static_cast<std::size_t>(description.width) * bytesPerPixel;
        if (description.initialData.size() != static_cast<std::size_t>(description.width) * description.height * bytesPerPixel) {
            description.initialData.assign(static_cast<std::size_t>(description.height) * basePitch, 0);
        }
        for (std::uint32_t row = 0; row < update.height; ++row) {
            std::copy_n(update.data.begin() + static_cast<std::ptrdiff_t>(row * sourcePitch),
                static_cast<std::size_t>(update.width) * bytesPerPixel,
                description.initialData.begin() + static_cast<std::ptrdiff_t>((update.y + row) * basePitch + update.x * bytesPerPixel));
        }
    }
    for (std::uint32_t row = 0; row < update.height; ++row) {
        std::copy_n(update.data.begin() + static_cast<std::ptrdiff_t>(row * sourcePitch),
            static_cast<std::size_t>(update.width) * bytesPerPixel,
            retained.data.begin() + static_cast<std::ptrdiff_t>((update.y + row) * fullPitch + update.x * bytesPerPixel));
    }
    if (existing == description.initialSubresources.end()) description.initialSubresources.push_back(std::move(retained));
    else *existing = std::move(retained);
}

bool validate_material_bindings(const MaterialDesc& description,
                                const std::vector<Renderer::PersistentResource>& resources,
                                BackendApi backend,
                                std::string& error) {
    if (!description.pipeline || description.pipeline.kind != ResourceKind::Pipeline) {
        error = "material pipeline handle is invalid";
        return false;
    }
    const auto pipeline = std::find_if(resources.begin(), resources.end(),
        [&](const Renderer::PersistentResource& resource) {
            return resource.handle.id == description.pipeline.id &&
                resource.handle.kind == description.pipeline.kind;
        });
    if (pipeline == resources.end() || pipeline->handle.kind != ResourceKind::Pipeline) {
        error = "material references an unknown pipeline resource";
        return false;
    }
    std::vector<ShaderBinding> shaderBindings;
    bool reflectionAvailable = false;
    std::string reflectionError;
    const auto pipelineDescription = std::get_if<PipelineDesc>(&pipeline->description);
    if (pipelineDescription) {
        const auto compiler = create_shader_compiler();
        for (const auto shaderId : {pipelineDescription->vertexShader, pipelineDescription->fragmentShader, pipelineDescription->computeShader}) {
            if (shaderId == 0) continue;
            const auto shader = std::find_if(resources.begin(), resources.end(),
                [shaderId](const Renderer::PersistentResource& resource) { return resource.handle.id == shaderId; });
            if (shader == resources.end()) continue;
            const auto shaderDescription = std::get_if<ShaderDesc>(&shader->description);
            if (!shaderDescription) continue;
            const auto compiled = compiler->compile(*shaderDescription, backend);
            if (!compiled.valid) {
                error = compiled.diagnostics.empty() ? "shader reflection or compilation failed" : compiled.diagnostics;
                return false;
            }
            if (!merge_shader_bindings(compiled.bindings, shaderBindings, reflectionError)) {
                error = reflectionError;
                return false;
            }
            if (compiled.bindings.empty()) continue;
            reflectionAvailable = true;
        }
    }
    std::unordered_set<std::uint64_t> locations;
    const auto resource_count = [](const DescriptorBinding& binding) {
        return binding.resources.empty() ? (binding.resource ? 1u : 0u) : static_cast<std::uint32_t>(binding.resources.size());
    };
    const auto resource_at = [](const DescriptorBinding& binding, std::uint32_t index) {
        return binding.resources.empty() ? binding.resource : binding.resources[index];
    };
    for (const auto& binding : description.bindings) {
        if (binding.count == 0 && !binding.unbounded) {
            error = "material descriptor count must be greater than zero unless unbounded=true";
            return false;
        }
        if (binding.unbounded && !description.bindless) {
            error = "unbounded material descriptors require bindless=true";
            return false;
        }
        if (binding.unbounded && backend == BackendApi::DirectX11) {
            error = "DirectX 11 does not support unbounded material descriptors";
            return false;
        }
        if (binding.slot >= 16 || binding.space >= 16) {
            error = "material binding slot or space exceeds the portable limit of 16";
            return false;
        }
        const auto location = (static_cast<std::uint64_t>(binding.space) << 32) | binding.slot;
        if (!locations.insert(location).second) {
            error = "material contains duplicate descriptor slot and space";
            return false;
        }
        const auto resourceCount = resource_count(binding);
        if (!binding.unbounded && resourceCount != binding.count) {
            error = "material descriptor resource count does not match the reflected/fixed descriptor count";
            return false;
        }
        for (std::uint32_t index = 0; index < resourceCount; ++index) {
            const auto handle = resource_at(binding, index);
            if (!handle) {
                error = "material contains an empty descriptor resource handle";
                return false;
            }
            const auto resource = std::find_if(resources.begin(), resources.end(),
                [&](const Renderer::PersistentResource& candidate) {
                    return candidate.handle.id == handle.id && candidate.handle.kind == handle.kind;
                });
            if (resource == resources.end()) {
                error = "material binding references an unknown resource";
                return false;
            }
            if (binding.type == DescriptorType::Sampler && handle.kind != ResourceKind::Sampler) {
                error = "sampler descriptor must reference a sampler resource";
                return false;
            }
            if ((binding.type == DescriptorType::Texture || binding.type == DescriptorType::StorageTexture) && handle.kind != ResourceKind::Texture2D) {
                error = "texture descriptor must reference a texture resource";
                return false;
            }
            if ((binding.type == DescriptorType::UniformBuffer || binding.type == DescriptorType::StorageBuffer || binding.type == DescriptorType::StructuredBuffer) &&
                handle.kind != ResourceKind::Buffer) {
                error = "buffer descriptor must reference a buffer resource";
                return false;
            }
            if (binding.type == DescriptorType::StorageTexture) {
                const auto* texture = std::get_if<TextureDesc>(&resource->description);
                if (!texture || !texture->storage) {
                    error = "storage texture descriptor requires a texture created with storage=true";
                    return false;
                }
            }
            if (binding.type == DescriptorType::StorageBuffer) {
                const auto* buffer = std::get_if<BufferDesc>(&resource->description);
                if (!buffer || !buffer->storageBuffer) {
                    error = "storage buffer descriptor requires a buffer created with storageBuffer=true";
                    return false;
                }
            }
            if (binding.type == DescriptorType::StructuredBuffer) {
                const auto* buffer = std::get_if<BufferDesc>(&resource->description);
                if (!buffer || !buffer->structuredBuffer || buffer->stride == 0) {
                    error = "structured buffer descriptor requires structuredBuffer=true and a non-zero stride";
                    return false;
                }
            }
        }
        if (reflectionAvailable) {
            const auto matched = std::any_of(shaderBindings.begin(), shaderBindings.end(),
                [&binding](const ShaderBinding& reflected) {
                    const auto type = [&]() {
                        switch (binding.type) {
                        case DescriptorType::Texture: return std::string("texture");
                        case DescriptorType::UniformBuffer: return std::string("uniform_buffer");
                        case DescriptorType::StorageBuffer: return std::string("storage_buffer");
                        case DescriptorType::Sampler: return std::string("sampler");
                        case DescriptorType::StorageTexture: return std::string("storage_texture");
                        case DescriptorType::StructuredBuffer: return std::string("structured_buffer");
                        }
                        return std::string("unknown");
                    }();
                    return reflected.slot == binding.slot && reflected.space == binding.space && reflected.type == type &&
                        reflected.count == binding.count && reflected.unbounded == binding.unbounded;
                });
            if (!matched) {
                error = "material binding does not match the reflected shader descriptor layout";
                return false;
            }
        }
    }
    if (reflectionAvailable) {
        for (const auto& reflected : shaderBindings) {
            // Frame/object constant buffers may be supplied by the render
            // pass through bind_uniform_buffer instead of being owned by a
            // material descriptor set.
            if (reflected.type == "uniform_buffer") continue;
            if (reflected.unbounded && !description.bindless) {
                error = "unbounded shader descriptors require a bindless material at space=" +
                    std::to_string(reflected.space) + " slot=" + std::to_string(reflected.slot);
                return false;
            }
            const auto matched = std::any_of(description.bindings.begin(), description.bindings.end(),
                [&reflected](const DescriptorBinding& binding) {
                    const auto type = [&]() {
                        switch (binding.type) {
                        case DescriptorType::Texture: return std::string("texture");
                        case DescriptorType::UniformBuffer: return std::string("uniform_buffer");
                        case DescriptorType::StorageBuffer: return std::string("storage_buffer");
                        case DescriptorType::Sampler: return std::string("sampler");
                        case DescriptorType::StorageTexture: return std::string("storage_texture");
                        case DescriptorType::StructuredBuffer: return std::string("structured_buffer");
                        }
                        return std::string("unknown");
                    }();
                    return reflected.slot == binding.slot && reflected.space == binding.space && reflected.type == type &&
                        reflected.count == binding.count && reflected.unbounded == binding.unbounded;
                });
            if (!matched) {
                error = "material is missing the reflected shader descriptor array at space=" +
                    std::to_string(reflected.space) + " slot=" + std::to_string(reflected.slot);
                return false;
            }
        }
    }
    return true;
}

bool is_supported_color_format(std::string_view format) {
    return format == "rgba8" || format == "bgra8" || format == "rgba16f" || format == "r32f";
}
}

Renderer::Renderer(BackendApi api, RenderBackendConfig config)
    : backend_(create_backend(api)), api_(api), config_(config) {}

ResourceHandle Renderer::allocate_persistent_resource(ResourceKind kind) noexcept {
    constexpr std::uint32_t first = 0x40000000u;
    constexpr std::uint32_t limit = 0x80000000u;
    for (std::uint64_t attempt = 0; attempt < static_cast<std::uint64_t>(limit - first); ++attempt) {
        if (nextPersistentResource_ < first || nextPersistentResource_ >= limit) nextPersistentResource_ = first;
        const auto id = nextPersistentResource_++;
        const auto used = std::any_of(persistentResources_.begin(), persistentResources_.end(),
            [id](const PersistentResource& resource) { return resource.handle.id == id; });
        if (!used) return {id, kind};
    }
    return {};
}

Renderer::PersistentResource* Renderer::find_persistent_resource(ResourceHandle handle) noexcept {
    const auto found = std::find_if(persistentResources_.begin(), persistentResources_.end(),
        [handle](const PersistentResource& resource) {
            return resource.handle.id == handle.id && resource.handle.kind == handle.kind;
        });
    return found == persistentResources_.end() ? nullptr : &*found;
}

const Renderer::PersistentResource* Renderer::find_persistent_resource(ResourceHandle handle) const noexcept {
    const auto found = std::find_if(persistentResources_.begin(), persistentResources_.end(),
        [handle](const PersistentResource& resource) {
            return resource.handle.id == handle.id && resource.handle.kind == handle.kind;
        });
    return found == persistentResources_.end() ? nullptr : &*found;
}

bool Renderer::restore_persistent_state(IRenderBackend& backend) {
    std::vector<ResourceHandle> createdResources;
    createdResources.reserve(persistentResources_.size());
    std::vector<BindlessTableHandle> createdTables;
    createdTables.reserve(persistentBindlessTables_.size());
    const auto rollback = [&]() {
        for (auto it = createdTables.rbegin(); it != createdTables.rend(); ++it) {
            backend.destroy_bindless_table(*it);
        }
        for (auto it = createdResources.rbegin(); it != createdResources.rend(); ++it) {
            backend.destroy_resource(*it);
        }
        for (auto& table : persistentBindlessTables_) table.backendHandle = {};
    };
    for (const auto& resource : persistentResources_) {
        if (!backend.create_resource(resource.handle, resource.description)) {
            lastError_ = backend.last_error().empty() ? "persistent resource restoration failed" : backend.last_error();
            createdResources.push_back(resource.handle);
            rollback();
            return false;
        }
        createdResources.push_back(resource.handle);
    }
    for (auto& table : persistentBindlessTables_) {
        table.backendHandle = backend.create_bindless_table(table.description);
        if (!table.backendHandle) {
            lastError_ = backend.last_error().empty()
                ? "persistent bindless table restoration failed" : backend.last_error();
            rollback();
            return false;
        }
        createdTables.push_back(table.backendHandle);
        for (const auto& [slot, resource] : table.entries) {
            if (!backend.update_bindless(table.backendHandle, slot, resource, table.description.type)) {
                lastError_ = backend.last_error().empty()
                    ? "persistent bindless descriptor restoration failed" : backend.last_error();
                rollback();
                return false;
            }
        }
    }
    return true;
}

Renderer::~Renderer() {
    if (!backend_) return;
    while (!persistentResources_.empty()) {
        backend_->destroy_resource(persistentResources_.back().handle);
        persistentResources_.pop_back();
    }
}

void Renderer::begin_graph() {
    graph_.reset();
    if (backend_ && initialized_) backend_->begin_upload_batch();
}

bool Renderer::initialize() {
    lastError_.clear();
    if (initialized_) return true;
    if (!backend_) backend_ = create_backend(api_);
    if (!backend_ || !backend_->initialize(config_)) {
        lastError_ = backend_ ? backend_->last_error() : "render backend is unavailable";
        if (lastError_.empty()) lastError_ = "render backend initialization failed";
        return false;
    }
    const auto device = backend_->capabilities();
    if (!device.deviceReady || device.deviceState == RenderDeviceState::Lost) {
        lastError_ = backend_->last_error();
        if (lastError_.empty()) lastError_ = "render backend did not produce a ready device";
        return false;
    }
    initialized_ = true;
    if (!restore_persistent_state(*backend_)) {
        initialized_ = false;
        backend_->wait_idle();
        backend_.reset();
        return false;
    }
    return true;
}

bool Renderer::initialize_imgui() {
    return backend_ && initialized_ && backend_->initialize_imgui();
}

void Renderer::shutdown_imgui() {
    if (backend_) backend_->shutdown_imgui();
    imguiDrawData_ = nullptr;
}

bool Renderer::recover() {
    if (backend_) {
        backend_->wait_idle();
        backend_.reset();
    }
    initialized_ = false;
    auto replacement = create_backend(api_);
    if (!replacement || !replacement->initialize(config_) || !replacement->capabilities().deviceReady ||
        replacement->capabilities().deviceState == RenderDeviceState::Lost) {
        lastError_ = replacement && !replacement->last_error().empty()
            ? replacement->last_error() : "render backend recreation failed during recovery";
        return false;
    }
    backend_ = std::move(replacement);
    initialized_ = true;
    if (!restore_persistent_state(*backend_)) {
        initialized_ = false;
        backend_->wait_idle();
        backend_.reset();
        return false;
    }
    lastError_.clear();
    return true;
}

void Renderer::set_config(RenderBackendConfig config) {
    const bool dimensionsChanged = config.width != config_.width || config.height != config_.height;
    config_ = config;
    if (initialized_ && dimensionsChanged && backend_ && !backend_->resize(config_.width, config_.height)) {
        lastError_ = backend_->last_error();
        if (lastError_.empty()) lastError_ = "render backend rejected the new dimensions";
    }
}

bool Renderer::resize(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        lastError_ = "renderer dimensions must be non-zero";
        return false;
    }
    config_.width = width;
    config_.height = height;
    if (!initialized_ || !backend_) return true;
    const bool success = backend_->resize(width, height);
    if (!success) lastError_ = backend_->last_error();
    return success;
}

ResourceHandle Renderer::create_texture(const TextureDesc& description) {
    if (description.width == 0 || description.height == 0 || description.layers == 0 || description.mipLevels == 0) {
        lastError_ = "texture dimensions, layers, and mipLevels must be non-zero";
        return {};
    }
    auto persistentDescription = description;
    if (persistentDescription.generateMips) retain_cpu_generated_mips(persistentDescription);
    const auto handle = allocate_persistent_resource(ResourceKind::Texture2D);
    if (!handle) { lastError_ = "persistent resource handle space is exhausted"; return {}; }
    persistentResources_.push_back({handle, persistentDescription});
    if (backend_ && initialized_ && !backend_->create_resource(handle, persistentDescription)) {
        lastError_ = backend_->last_error();
        backend_->destroy_resource(handle);
        persistentResources_.pop_back();
        return {};
    }
    return handle;
}

ResourceHandle Renderer::create_sampler(const SamplerDesc& description) {
    const auto handle = allocate_persistent_resource(ResourceKind::Sampler);
    if (!handle) { lastError_ = "persistent resource handle space is exhausted"; return {}; }
    persistentResources_.push_back({handle, description});
    if (backend_ && initialized_ && !backend_->create_resource(handle, description)) {
        lastError_ = backend_->last_error();
        backend_->destroy_resource(handle);
        persistentResources_.pop_back();
        return {};
    }
    return handle;
}

ResourceHandle Renderer::create_depth_stencil(const TextureDesc& description) {
    if (description.width == 0 || description.height == 0 || description.layers == 0 || description.mipLevels == 0) {
        lastError_ = "depth-stencil dimensions, layers, and mipLevels must be non-zero";
        return {};
    }
    const auto handle = allocate_persistent_resource(ResourceKind::DepthStencil);
    if (!handle) { lastError_ = "persistent resource handle space is exhausted"; return {}; }
    persistentResources_.push_back({handle, description});
    if (backend_ && initialized_ && !backend_->create_resource(handle, description)) {
        lastError_ = backend_->last_error();
        backend_->destroy_resource(handle);
        persistentResources_.pop_back();
        return {};
    }
    return handle;
}

ResourceHandle Renderer::create_buffer(const BufferDesc& description) {
    if (description.size == 0 || (description.structuredBuffer && description.stride == 0)) {
        lastError_ = "buffer size must be non-zero and structured buffers require a stride";
        return {};
    }
    const auto handle = allocate_persistent_resource(ResourceKind::Buffer);
    if (!handle) { lastError_ = "persistent resource handle space is exhausted"; return {}; }
    persistentResources_.push_back({handle, description});
    if (backend_ && initialized_ && !backend_->create_resource(handle, description)) {
        lastError_ = backend_->last_error();
        backend_->destroy_resource(handle);
        persistentResources_.pop_back();
        return {};
    }
    return handle;
}

ResourceHandle Renderer::create_shader(const ShaderDesc& description) {
    const auto key = shader_cache_key(description, api_);
    const auto cached = shaderCache_.find(key);
    if (cached != shaderCache_.end()) return cached->second;
    const auto handle = allocate_persistent_resource(ResourceKind::Shader);
    if (!handle) { lastError_ = "persistent resource handle space is exhausted"; return {}; }
    persistentResources_.push_back({handle, description});
    shaderCache_.emplace(key, handle);
    if (backend_ && initialized_ && !backend_->create_resource(handle, description)) {
        lastError_ = backend_->last_error();
        backend_->destroy_resource(handle);
        persistentResources_.pop_back();
        shaderCache_.erase(key);
        return {};
    }
    return handle;
}

bool Renderer::reload_shader(ResourceHandle shader, const ShaderDesc& description) {
    auto* found = find_persistent_resource(shader);
    if (!found || shader.kind != ResourceKind::Shader) {
        lastError_ = "shader handle is invalid";
        return false;
    }
    const auto* currentShader = std::get_if<ShaderDesc>(&found->description);
    if (!currentShader || description.stage != currentShader->stage) {
        lastError_ = "shader stage cannot change during reload";
        return false;
    }
    const ShaderDesc oldShader = *currentShader;
    std::vector<ResourceHandle> dependentPipelines;
    for (const auto& resource : persistentResources_) {
        const auto pipeline = std::get_if<PipelineDesc>(&resource.description);
        if (pipeline && (pipeline->vertexShader == shader.id || pipeline->fragmentShader == shader.id || pipeline->computeShader == shader.id)) {
            dependentPipelines.push_back(resource.handle);
        }
    }
    std::vector<ResourceHandle> dependentMaterials;
    for (const auto& resource : persistentResources_) {
        const auto material = std::get_if<MaterialDesc>(&resource.description);
        if (material && std::find_if(dependentPipelines.begin(), dependentPipelines.end(),
            [material](ResourceHandle pipeline) { return material->pipeline.id == pipeline.id; }) != dependentPipelines.end()) {
            dependentMaterials.push_back(resource.handle);
        }
    }
    std::vector<std::pair<ResourceHandle, ResourceDesc>> pipelineDescriptions;
    std::vector<std::pair<ResourceHandle, ResourceDesc>> materialDescriptions;
    for (const auto pipeline : dependentPipelines) {
        if (const auto* resource = resource_description(pipeline)) pipelineDescriptions.push_back({pipeline, *resource});
    }
    for (const auto material : dependentMaterials) {
        if (const auto* resource = resource_description(material)) materialDescriptions.push_back({material, *resource});
    }
    if (backend_ && initialized_) {
        backend_->wait_idle();
        const auto rollback = [&]() {
            backend_->destroy_resource(shader);
            backend_->create_resource(shader, oldShader);
            for (const auto& [handle, resource] : pipelineDescriptions) {
                backend_->destroy_resource(handle);
                backend_->create_resource(handle, resource);
            }
            for (const auto& [handle, resource] : materialDescriptions) {
                backend_->destroy_resource(handle);
                backend_->create_resource(handle, resource);
            }
        };
        backend_->destroy_resource(shader);
        if (!backend_->create_resource(shader, description)) {
            lastError_ = backend_->last_error();
            rollback();
            return false;
        }
        for (const auto& [handle, resource] : pipelineDescriptions) {
            backend_->destroy_resource(handle);
            if (!backend_->create_resource(handle, resource)) {
                lastError_ = backend_->last_error();
                rollback();
                return false;
            }
        }
    }
    for (const auto& [handle, resource] : materialDescriptions) {
        if (!backend_ || !initialized_) break;
        backend_->destroy_resource(handle);
        if (!backend_->create_resource(handle, resource)) {
            lastError_ = backend_->last_error();
            if (backend_ && initialized_) {
                backend_->destroy_resource(shader);
                backend_->create_resource(shader, oldShader);
                for (const auto& [pipelineHandle, pipelineResource] : pipelineDescriptions) {
                    backend_->destroy_resource(pipelineHandle);
                    backend_->create_resource(pipelineHandle, pipelineResource);
                }
                for (const auto& [materialHandle, materialResource] : materialDescriptions) {
                    backend_->destroy_resource(materialHandle);
                    backend_->create_resource(materialHandle, materialResource);
                }
            }
            return false;
        }
    }
    found->description = description;
    for (auto it = shaderCache_.begin(); it != shaderCache_.end();) {
        if (it->second.id == shader.id) it = shaderCache_.erase(it);
        else ++it;
    }
    shaderCache_[shader_cache_key(description, api_)] = shader;
    lastError_.clear();
    return true;
}

ResourceHandle Renderer::create_pipeline(const PipelineDesc& description) {
    if (description.colorFormats.size() > 8 ||
        !is_supported_color_format(description.colorFormat) ||
        std::any_of(description.colorFormats.begin(), description.colorFormats.end(),
            [](const std::string& format) { return !is_supported_color_format(format); })) {
        lastError_ = "pipeline must declare between 0 and 8 non-empty color attachment formats";
        return {};
    }
    const auto key = pipeline_cache_key(description);
    const auto cached = pipelineCache_.find(key);
    if (cached != pipelineCache_.end()) return cached->second;
    if (description.computeShader == 0 && (description.vertexShader == 0 || description.fragmentShader == 0)) {
        lastError_ = "graphics pipeline requires both vertex and fragment shaders";
        return {};
    }
    if (description.computeShader != 0 && (description.vertexShader != 0 || description.fragmentShader != 0)) {
        lastError_ = "compute pipeline cannot also declare graphics shaders";
        return {};
    }
    const auto findShader = [&](std::uint32_t id, ShaderStage stage) {
        const auto found = std::find_if(persistentResources_.begin(), persistentResources_.end(),
            [id](const PersistentResource& resource) { return resource.handle.id == id; });
        if (found == persistentResources_.end() || found->handle.kind != ResourceKind::Shader) return false;
        const auto* shader = std::get_if<ShaderDesc>(&found->description);
        return shader && shader->stage == stage;
    };
    if (api_ != BackendApi::Null && description.computeShader != 0) {
        if (!findShader(description.computeShader, ShaderStage::Compute)) {
            lastError_ = "compute pipeline references an invalid or non-compute shader";
            return {};
        }
    } else if (api_ != BackendApi::Null &&
               (!findShader(description.vertexShader, ShaderStage::Vertex) ||
                !findShader(description.fragmentShader, ShaderStage::Fragment))) {
        lastError_ = "graphics pipeline references an invalid vertex or fragment shader";
        return {};
    }
    const auto handle = allocate_persistent_resource(ResourceKind::Pipeline);
    if (!handle) { lastError_ = "persistent resource handle space is exhausted"; return {}; }
    persistentResources_.push_back({handle, description});
    pipelineCache_.emplace(key, handle);
    if (backend_ && initialized_ && !backend_->create_resource(handle, description)) {
        lastError_ = backend_->last_error();
        backend_->destroy_resource(handle);
        persistentResources_.pop_back();
        pipelineCache_.erase(key);
        return {};
    }
    return handle;
}

ResourceHandle Renderer::create_material(const MaterialDesc& description) {
    if (!validate_material_bindings(description, persistentResources_, api_, lastError_)) return {};
    const auto handle = allocate_persistent_resource(ResourceKind::Material);
    if (!handle) { lastError_ = "persistent resource handle space is exhausted"; return {}; }
    persistentResources_.push_back({handle, description});
    if (backend_ && initialized_ && !backend_->create_resource(handle, description)) {
        lastError_ = backend_->last_error();
        backend_->destroy_resource(handle);
        persistentResources_.pop_back();
        return {};
    }
    return handle;
}

bool Renderer::update_material(ResourceHandle material, const MaterialDesc& description) {
    if (!validate_material_bindings(description, persistentResources_, api_, lastError_)) return false;
    auto* found = find_persistent_resource(material);
    if (!found || material.kind != ResourceKind::Material) {
        lastError_ = "material handle is invalid";
        return false;
    }
    const auto current = std::get_if<MaterialDesc>(&found->description);
    if (!current || description.pipeline.id != current->pipeline.id) {
        lastError_ = "material pipeline cannot be changed after creation";
        return false;
    }
    for (const auto& oldBinding : current->bindings) {
        const auto replacement = std::find_if(description.bindings.begin(), description.bindings.end(),
            [&oldBinding](const DescriptorBinding& binding) {
                return binding.slot == oldBinding.slot && binding.space == oldBinding.space;
            });
        if (replacement != description.bindings.end() && replacement->type != oldBinding.type) {
            lastError_ = "material descriptor type cannot change for an existing shader slot";
            return false;
        }
    }
    if (backend_ && initialized_ && !backend_->update_material(material, description)) {
        lastError_ = backend_->last_error();
        if (lastError_.empty()) lastError_ = "backend rejected material update";
        return false;
    }
    found->description = description;
    return true;
}

bool Renderer::update_material_binding(ResourceHandle material, const DescriptorBinding& binding) {
    const auto* found = find_persistent_resource(material);
    if (!found || material.kind != ResourceKind::Material) {
        lastError_ = "material handle is invalid";
        return false;
    }
    const auto current = std::get_if<MaterialDesc>(&found->description);
    if (!current) {
        lastError_ = "material description is invalid";
        return false;
    }
    auto updated = *current;
    const auto existing = std::find_if(updated.bindings.begin(), updated.bindings.end(),
        [&binding](const DescriptorBinding& candidate) {
            return candidate.slot == binding.slot && candidate.space == binding.space;
        });
    if (existing == updated.bindings.end()) updated.bindings.push_back(binding);
    else *existing = binding;
    return update_material(material, updated);
}

const ResourceDesc* Renderer::resource_description(ResourceHandle handle) const noexcept {
    const auto resource = find_persistent_resource(handle);
    return resource ? &resource->description : nullptr;
}

void Renderer::destroy_resource(ResourceHandle handle) {
    const auto found = find_persistent_resource(handle);
    if (!found) {
        lastError_ = "destroy_resource received an unknown persistent resource handle";
        return;
    }
    if (initialized_ && backend_) backend_->destroy_resource(handle);
    persistentResources_.erase(std::remove_if(persistentResources_.begin(), persistentResources_.end(),
        [handle](const PersistentResource& resource) {
            return resource.handle.id == handle.id && resource.handle.kind == handle.kind;
        }), persistentResources_.end());
    for (auto& table : persistentBindlessTables_) {
        table.entries.erase(std::remove_if(table.entries.begin(), table.entries.end(),
            [handle](const auto& entry) {
                return entry.second.id == handle.id && entry.second.kind == handle.kind;
            }), table.entries.end());
    }
    for (auto it = pipelineCache_.begin(); it != pipelineCache_.end();) {
        if (it->second.id == handle.id) it = pipelineCache_.erase(it);
        else ++it;
    }
    for (auto it = shaderCache_.begin(); it != shaderCache_.end();) {
        if (it->second.id == handle.id) it = shaderCache_.erase(it);
        else ++it;
    }
}

bool Renderer::update_buffer(const BufferUpdate& update) {
    auto* found = find_persistent_resource(update.buffer);
    if (!found || update.buffer.kind != ResourceKind::Buffer) {
        lastError_ = "buffer update references an unknown persistent buffer";
        return false;
    }
    auto* buffer = std::get_if<BufferDesc>(&found->description);
    if (!buffer || update.offset > buffer->size || update.data.empty() || update.data.size() > buffer->size - update.offset) {
        lastError_ = "buffer update exceeds persistent buffer description";
        return false;
    }
    if (!initialized_ || !backend_) {
        retain_buffer_update(*buffer, update);
        return true;
    }
    const bool success = backend_->update_buffer(update);
    if (!success) lastError_ = backend_->last_error();
    else retain_buffer_update(*buffer, update);
    return success;
}

bool Renderer::update_texture(const TextureUpdate& update) {
    auto* found = find_persistent_resource(update.texture);
    if (!found || update.texture.kind != ResourceKind::Texture2D) {
        lastError_ = "texture update references an unknown persistent texture";
        return false;
    }
    auto* texture = std::get_if<TextureDesc>(&found->description);
    const auto mipWidth = texture ? std::max(1u, texture->width >> update.mipLevel) : 0u;
    const auto mipHeight = texture ? std::max(1u, texture->height >> update.mipLevel) : 0u;
    const auto bytesPerPixel = texture ? texture_bytes_per_pixel(texture->format) : 0u;
    const auto sourcePitch = update.rowPitch == 0 ? static_cast<std::size_t>(update.width) * bytesPerPixel : update.rowPitch;
    if (!texture || update.mipLevel >= texture->mipLevels || update.layer >= texture->layers || update.data.empty() ||
        update.width == 0 || update.height == 0 || update.x > mipWidth || update.y > mipHeight ||
        update.width > mipWidth - update.x || update.height > mipHeight - update.y ||
        sourcePitch < static_cast<std::size_t>(update.width) * bytesPerPixel ||
        sourcePitch > std::numeric_limits<std::size_t>::max() / update.height ||
        sourcePitch * update.height > update.data.size()) {
        lastError_ = "texture update exceeds persistent texture description";
        return false;
    }
    if (!initialized_ || !backend_) {
        retain_texture_update(*texture, update);
        return true;
    }
    const bool success = backend_->update_texture(update);
    if (!success) lastError_ = backend_->last_error();
    else retain_texture_update(*texture, update);
    return success;
}

bool Renderer::generate_mips(ResourceHandle texture) {
    if (!initialized_ || !backend_) return false;
    const bool success = backend_->generate_mips(texture);
    if (!success) {
        lastError_ = backend_->last_error();
        if (lastError_.empty()) lastError_ = "texture mip generation is not supported by the active backend or resource";
    } else {
        auto* found = find_persistent_resource(texture);
        if (found) {
            if (auto* description = std::get_if<TextureDesc>(&found->description)) {
                description->generateMips = true;
                retain_cpu_generated_mips(*description);
            }
        }
    }
    return success;
}

BindlessTableHandle Renderer::create_bindless_table(const BindlessTableDesc& description) {
    if (description.capacity == 0 || description.type != DescriptorType::Texture) {
        lastError_ = "portable bindless tables currently require a non-zero texture capacity";
        return {};
    }
    if (!initialized_ || !backend_) return {};
    const auto backendTable = backend_->create_bindless_table(description);
    if (!backendTable) {
        lastError_ = backend_->last_error();
        if (lastError_.empty()) lastError_ = "bindless descriptor tables are not supported by the active backend";
    }
    if (!backendTable) return {};
    const auto first = nextPersistentBindlessTable_;
    do {
        if (nextPersistentBindlessTable_ == 0) nextPersistentBindlessTable_ = 0x40000000u;
        const auto id = nextPersistentBindlessTable_++;
        const auto used = std::any_of(persistentBindlessTables_.begin(), persistentBindlessTables_.end(),
            [id](const PersistentBindlessTable& table) { return table.handle.id == id; });
        if (!used) {
            const BindlessTableHandle handle{id, 0, description.capacity};
            persistentBindlessTables_.push_back({description, handle, backendTable, {}});
            return handle;
        }
    } while (nextPersistentBindlessTable_ != first);
    backend_->destroy_bindless_table(backendTable);
    lastError_ = "persistent bindless table handle space is exhausted";
    return {};
}

bool Renderer::update_bindless(BindlessTableHandle table, std::uint32_t slot, ResourceHandle resource, DescriptorType type) {
    const auto tableIt = std::find_if(persistentBindlessTables_.begin(), persistentBindlessTables_.end(),
        [table](const PersistentBindlessTable& candidate) { return candidate.handle.id == table.id; });
    if (tableIt == persistentBindlessTables_.end() || slot >= tableIt->description.capacity) {
        lastError_ = "bindless update references an invalid table or slot";
        return false;
    }
    if (type != tableIt->description.type) {
        lastError_ = "bindless update descriptor type does not match the table type";
        return false;
    }
    const auto* resourceDescription = find_persistent_resource(resource);
    if (!resourceDescription) {
        lastError_ = "bindless update references an unknown persistent resource";
        return false;
    }
    const auto* buffer = std::get_if<BufferDesc>(&resourceDescription->description);
    const auto* texture = std::get_if<TextureDesc>(&resourceDescription->description);
    const bool compatible =
        (type == DescriptorType::Sampler && resource.kind == ResourceKind::Sampler) ||
        (type == DescriptorType::Texture && resource.kind == ResourceKind::Texture2D) ||
        (type == DescriptorType::StorageTexture && resource.kind == ResourceKind::Texture2D && texture && texture->storage) ||
        (type == DescriptorType::UniformBuffer && resource.kind == ResourceKind::Buffer) ||
        (type == DescriptorType::StorageBuffer && resource.kind == ResourceKind::Buffer && buffer && buffer->storageBuffer) ||
        (type == DescriptorType::StructuredBuffer && resource.kind == ResourceKind::Buffer && buffer && buffer->structuredBuffer && buffer->stride != 0);
    if (!compatible) {
        lastError_ = "bindless resource type is incompatible with the table descriptor type";
        return false;
    }
    if (!initialized_ || !backend_) return false;
    const bool success = backend_->update_bindless(tableIt->backendHandle, slot, resource, type);
    if (!success) {
        lastError_ = backend_->last_error();
        if (lastError_.empty()) lastError_ = "bindless descriptors are not supported by the active backend";
    }
    if (success) {
        for (auto& persistent : persistentBindlessTables_) {
            if (persistent.handle.id != table.id) continue;
            const auto existing = std::find_if(persistent.entries.begin(), persistent.entries.end(),
                [slot](const auto& entry) { return entry.first == slot; });
            if (existing == persistent.entries.end()) persistent.entries.push_back({slot, resource});
            else existing->second = resource;
            break;
        }
    }
    return success;
}

void Renderer::destroy_bindless_table(BindlessTableHandle table) {
    const auto found = std::find_if(persistentBindlessTables_.begin(), persistentBindlessTables_.end(),
        [table](const auto& persistent) { return persistent.handle.id == table.id; });
    if (found == persistentBindlessTables_.end()) return;
    if (backend_ && found->backendHandle) backend_->destroy_bindless_table(found->backendHandle);
    persistentBindlessTables_.erase(found);
}

void Renderer::bind_bindless_table(BindlessTableHandle table) {
    const auto found = std::find_if(persistentBindlessTables_.begin(), persistentBindlessTables_.end(),
        [table](const auto& persistent) { return persistent.handle.id == table.id; });
    if (backend_ && found != persistentBindlessTables_.end()) backend_->bind_bindless_table(found->backendHandle);
}

const RenderCapabilities Renderer::capabilities() const noexcept {
    return backend_ ? backend_->capabilities() : RenderCapabilities{};
}

const RenderStats Renderer::stats() const noexcept {
    return backend_ ? backend_->stats() : RenderStats{};
}

void Renderer::submit() {
    lastError_.clear();
    if (!backend_) return;
    const auto device = backend_->capabilities();
    if (!initialized_ || !device.deviceReady || device.deviceState == RenderDeviceState::Lost) {
        lastError_ = backend_->last_error();
        if (lastError_.empty()) lastError_ = "render submission skipped because the device is lost or unavailable";
        return;
    }
    if (device.deviceState == RenderDeviceState::NeedsResize) {
        lastError_ = backend_->last_error();
        if (lastError_.empty()) lastError_ = "render submission skipped until the swapchain is resized";
        return;
    }
    if (!backend_->flush_upload_batch()) {
        lastError_ = backend_->last_error();
        if (lastError_.empty()) lastError_ = "render upload batch submission failed";
        return;
    }
    auto* drawData = imguiDrawData_;
#if defined(SHINKOU_WITH_UIKIT)
    auto* uiRenderList = uiRenderList_;
#endif
#if defined(SHINKOU_WITH_UIKIT)
    graph_.execute(*backend_, &lastError_, [drawData, uiRenderList](IRenderBackend& backend) {
        if (drawData) backend.render_imgui(drawData);
        if (uiRenderList) backend.render_ui(*uiRenderList);
    });
#else
    graph_.execute(*backend_, &lastError_, [drawData](IRenderBackend& backend) {
        if (drawData) backend.render_imgui(drawData);
    });
#endif
    imguiDrawData_ = nullptr;
#if defined(SHINKOU_WITH_UIKIT)
    uiRenderList_ = nullptr;
#endif
    if (lastError_.empty() && backend_) lastError_ = backend_->last_error();
    if (lastError_.empty() && backend_ && backend_->capabilities().deviceState == RenderDeviceState::Lost) {
        lastError_ = backend_->last_error();
        if (lastError_.empty()) lastError_ = "render backend lost the device during submission";
    }
}
}

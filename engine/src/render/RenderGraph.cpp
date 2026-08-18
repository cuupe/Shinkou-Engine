#include "shinkou/render/RenderGraph.h"
#include <algorithm>
#include <chrono>
#include <queue>
#include <unordered_set>

namespace shinkou::render {
namespace {
bool is_write_usage(ResourceUsage usage) {
    return usage == ResourceUsage::ShaderWrite ||
        usage == ResourceUsage::StorageWrite ||
        (usage >= ResourceUsage::ColorAttachment0 && usage <= ResourceUsage::ColorAttachment7) ||
        usage == ResourceUsage::DepthStencil ||
        usage == ResourceUsage::CopyDestination;
}

    std::uint32_t color_attachment_index(ResourceUsage usage) {
    if (usage < ResourceUsage::ColorAttachment0 || usage > ResourceUsage::ColorAttachment7) return UINT32_MAX;
    return static_cast<std::uint32_t>(usage) - static_cast<std::uint32_t>(ResourceUsage::ColorAttachment0);
}

bool usage_matches_kind(ResourceKind kind, ResourceUsage usage) {
    switch (kind) {
    case ResourceKind::Texture2D:
        return usage == ResourceUsage::ShaderRead || usage == ResourceUsage::ShaderWrite ||
            usage == ResourceUsage::StorageRead || usage == ResourceUsage::StorageWrite ||
            (usage >= ResourceUsage::ColorAttachment0 && usage <= ResourceUsage::ColorAttachment7) || usage == ResourceUsage::CopySource ||
            usage == ResourceUsage::CopyDestination || usage == ResourceUsage::Present;
    case ResourceKind::DepthStencil:
        return usage == ResourceUsage::ShaderRead || usage == ResourceUsage::ShaderWrite ||
            usage == ResourceUsage::StorageRead || usage == ResourceUsage::StorageWrite ||
            usage == ResourceUsage::DepthStencil || usage == ResourceUsage::CopySource ||
            usage == ResourceUsage::CopyDestination;
    case ResourceKind::Buffer:
        return usage == ResourceUsage::ShaderRead || usage == ResourceUsage::ShaderWrite ||
            usage == ResourceUsage::StorageRead || usage == ResourceUsage::StorageWrite ||
            usage == ResourceUsage::UniformBuffer || usage == ResourceUsage::VertexBuffer ||
            usage == ResourceUsage::IndexBuffer || usage == ResourceUsage::IndirectArguments ||
            usage == ResourceUsage::CopySource || usage == ResourceUsage::CopyDestination;
    case ResourceKind::Sampler:
    case ResourceKind::Shader:
    case ResourceKind::Pipeline:
    case ResourceKind::Material:
        return usage == ResourceUsage::ShaderRead;
    }
    return false;
}

bool requires_storage(ResourceUsage usage) {
    return usage == ResourceUsage::ShaderWrite || usage == ResourceUsage::StorageRead ||
        usage == ResourceUsage::StorageWrite;
}

bool requires_physical_state(ResourceKind kind) {
    return kind == ResourceKind::Texture2D || kind == ResourceKind::DepthStencil || kind == ResourceKind::Buffer;
}

bool description_matches_kind(ResourceKind kind, const ResourceDesc& description) {
    switch (kind) {
    case ResourceKind::Texture2D:
    case ResourceKind::DepthStencil:
        return std::holds_alternative<TextureDesc>(description);
    case ResourceKind::Buffer:
        return std::holds_alternative<BufferDesc>(description);
    case ResourceKind::Shader:
        return std::holds_alternative<ShaderDesc>(description);
    case ResourceKind::Pipeline:
        return std::holds_alternative<PipelineDesc>(description);
    case ResourceKind::Material:
        return std::holds_alternative<MaterialDesc>(description);
    case ResourceKind::Sampler:
        return std::holds_alternative<SamplerDesc>(description);
    }
    return false;
}

bool same_resource(ResourceHandle left, ResourceHandle right) {
    return left.id == right.id && left.kind == right.kind;
}

bool has_present_access(const RenderGraph::Pass& pass) {
    return std::any_of(pass.accesses.begin(), pass.accesses.end(), [](const ResourceAccess& access) {
        return access.usage == ResourceUsage::Present;
    });
}
}

ResourceHandle RenderGraph::allocate_resource(ResourceKind kind) noexcept {
    while (nextResource_ != 0u && std::any_of(resources_.begin(), resources_.end(),
        [this](const ResourceNode& resource) { return resource.handle.id == nextResource_; })) {
        ++nextResource_;
    }
    if (nextResource_ == 0u) return {};
    return {nextResource_++, kind};
}

ResourceHandle RenderGraph::create_texture(const TextureDesc& description) {
    if (description.width == 0 || description.height == 0 || description.layers == 0 || description.mipLevels == 0) return {};
    const auto handle = allocate_resource(ResourceKind::Texture2D);
    if (!handle) return {};
    resources_.push_back({handle, handle, description, false});
    return handle;
}

ResourceHandle RenderGraph::create_sampler(const SamplerDesc& description) {
    const auto handle = allocate_resource(ResourceKind::Sampler);
    if (!handle) return {};
    resources_.push_back({handle, handle, description, false});
    return handle;
}

void RenderGraph::import_resource(ResourceHandle handle, const ResourceDesc& description) {
    if (!handle) return;
    if (!description_matches_kind(handle.kind, description)) return;
    const auto existing = std::find_if(resources_.begin(), resources_.end(),
        [handle](const ResourceNode& resource) { return resource.handle.id == handle.id; });
    if (existing != resources_.end()) {
        if (existing->handle.kind != handle.kind) return;
        if (!existing->external) return;
        existing->physicalHandle = existing->handle;
        existing->description = description;
        existing->external = true;
        compiled_ = false;
        return;
    }
    resources_.push_back({handle, handle, description, true});
    compiled_ = false;
}

void RenderGraph::import_texture(ResourceHandle handle, const TextureDesc& description) {
    if (handle.kind != ResourceKind::Texture2D) return;
    import_resource(handle, description);
}

ResourceHandle RenderGraph::create_depth_stencil(const TextureDesc& description) {
    if (description.width == 0 || description.height == 0 || description.layers == 0 || description.mipLevels == 0) return {};
    const auto handle = allocate_resource(ResourceKind::DepthStencil);
    if (!handle) return {};
    resources_.push_back({handle, handle, description, false});
    return handle;
}

ResourceHandle RenderGraph::create_buffer(const BufferDesc& description) {
    if (description.size == 0 || (description.structuredBuffer && description.stride == 0)) return {};
    const auto handle = allocate_resource(ResourceKind::Buffer);
    if (!handle) return {};
    resources_.push_back({handle, handle, description, false});
    return handle;
}

ResourceHandle RenderGraph::create_shader(const ShaderDesc& description) {
    const auto handle = allocate_resource(ResourceKind::Shader);
    if (!handle) return {};
    resources_.push_back({handle, handle, description, false});
    return handle;
}

ResourceHandle RenderGraph::create_pipeline(const PipelineDesc& description) {
    const auto handle = allocate_resource(ResourceKind::Pipeline);
    if (!handle) return {};
    resources_.push_back({handle, handle, description, false});
    return handle;
}

ResourceHandle RenderGraph::create_material(const MaterialDesc& description) {
    const auto handle = allocate_resource(ResourceKind::Material);
    if (!handle) return {};
    resources_.push_back({handle, handle, description, false});
    return handle;
}

bool RenderGraph::contains_resource(ResourceHandle handle) const noexcept {
    return std::any_of(resources_.begin(), resources_.end(),
        [handle](const ResourceNode& resource) {
            return resource.handle.id == handle.id && resource.handle.kind == handle.kind;
        });
}

bool RenderGraph::is_external_resource(ResourceHandle handle) const noexcept {
    const auto found = std::find_if(resources_.begin(), resources_.end(),
        [handle](const ResourceNode& resource) {
            return resource.handle.id == handle.id && resource.handle.kind == handle.kind;
        });
    return found != resources_.end() && found->external;
}

ResourceHandle RenderGraph::physical_resource(ResourceHandle logical) const noexcept {
    const auto found = std::find_if(resources_.begin(), resources_.end(),
        [logical](const ResourceNode& resource) {
            return resource.handle.id == logical.id && resource.handle.kind == logical.kind;
        });
    return found == resources_.end() ? ResourceHandle{} : found->physicalHandle;
}

std::size_t RenderGraph::add_pass(std::string name, std::vector<ResourceHandle> reads,
                                  std::vector<ResourceHandle> writes, PassCallback callback,
                                  RenderQueue queue, bool sideEffect, bool clearAttachments) {
    std::vector<ResourceAccess> accesses;
    accesses.reserve(reads.size() + writes.size());
    for (const auto resource : reads) accesses.push_back({resource, ResourceUsage::ShaderRead});
    for (const auto resource : writes) accesses.push_back({resource, ResourceUsage::ColorAttachment});
    passes_.push_back({std::move(name), std::move(reads), std::move(writes), std::move(accesses), std::move(callback), queue, sideEffect, clearAttachments});
    compiled_ = false;
    return passes_.size() - 1;
}

std::size_t RenderGraph::add_pass(std::string name, std::vector<ResourceAccess> accesses,
                                  PassCallback callback, RenderQueue queue, bool sideEffect, bool clearAttachments) {
    std::vector<ResourceHandle> reads;
    std::vector<ResourceHandle> writes;
    for (const auto access : accesses) {
        const bool write = is_write_usage(access.usage);
        (write ? writes : reads).push_back(access.resource);
    }
    passes_.push_back({std::move(name), std::move(reads), std::move(writes), std::move(accesses), std::move(callback), queue, sideEffect, clearAttachments});
    compiled_ = false;
    return passes_.size() - 1;
}

bool RenderGraph::compile(std::string* error) {
    const auto compileStart = std::chrono::steady_clock::now();
    const std::size_t count = passes_.size();
    if (error) error->clear();
    compiled_ = false;
    executionOrder_.clear();
    transitions_.clear();
    queueDependencies_.clear();
    queueBatches_.clear();
    passTimings_.clear();
    aliasPlan_ = {};
    diagnostics_ = {};
    diagnostics_.passCount = count;
    diagnostics_.resourceCount = resources_.size();
    const auto fail_compile = [&](std::string message, bool validation = true) {
        if (error) *error = message;
        diagnostics_.lastError = std::move(message);
        if (validation) ++diagnostics_.validationErrorCount;
        diagnostics_.compileNanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - compileStart).count());
        return false;
    };
    std::vector<std::vector<std::size_t>> edges(count);
    std::vector<std::size_t> indegree(count, 0);
    std::unordered_map<std::uint32_t, std::size_t> lastWriter;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> readersSinceWrite;
    std::size_t presentPassIndex = static_cast<std::size_t>(-1);
    const auto addEdge = [&](std::size_t from, std::size_t to) {
        if (from == to) return;
        if (std::find(edges[from].begin(), edges[from].end(), to) == edges[from].end()) {
            edges[from].push_back(to);
            ++indegree[to];
        }
    };

    for (std::size_t i = 0; i < count; ++i) {
        std::unordered_map<std::uint32_t, ResourceUsage> passUsages;
        const auto resource_exists = [this](ResourceHandle handle) {
            return std::any_of(resources_.begin(), resources_.end(), [handle](const ResourceNode& resource) {
                return same_resource(resource.handle, handle);
            });
        };
        const auto resource_id_exists = [this](ResourceHandle handle) {
            return std::any_of(resources_.begin(), resources_.end(), [handle](const ResourceNode& resource) {
                return resource.handle.id == handle.id;
            });
        };
        const auto declared_access = [this](const Pass& pass, ResourceHandle handle, bool write) {
            return std::any_of(pass.accesses.begin(), pass.accesses.end(), [handle, write](const ResourceAccess& access) {
                return same_resource(access.resource, handle) && is_write_usage(access.usage) == write;
            });
        };
        for (const auto resource : passes_[i].reads) {
            if (!resource_exists(resource)) {
                if (resource_id_exists(resource)) {
                    return fail_compile("pass '" + passes_[i].name + "' reads a resource with the wrong resource kind");
                }
                return fail_compile("pass '" + passes_[i].name + "' reads an unknown resource");
            }
            if (!declared_access(passes_[i], resource, false)) {
                return fail_compile("pass '" + passes_[i].name + "' read declaration is inconsistent with its accesses");
            }
        }
        for (const auto resource : passes_[i].writes) {
            if (!resource_exists(resource)) {
                if (resource_id_exists(resource)) {
                    return fail_compile("pass '" + passes_[i].name + "' writes a resource with the wrong resource kind");
                }
                return fail_compile("pass '" + passes_[i].name + "' writes an unknown resource");
            }
            if (!declared_access(passes_[i], resource, true)) {
                return fail_compile("pass '" + passes_[i].name + "' write declaration is inconsistent with its accesses");
            }
        }
        for (const auto access : passes_[i].accesses) {
            const auto resource = std::find_if(resources_.begin(), resources_.end(),
                [access](const ResourceNode& node) { return node.handle.id == access.resource.id; });
            if (resource == resources_.end()) {
                return fail_compile("pass '" + passes_[i].name + "' accesses unknown resource id=" +
                    std::to_string(access.resource.id) + " kind=" + std::to_string(static_cast<std::uint32_t>(access.resource.kind)));
            }
            if (resource->handle.kind != access.resource.kind) {
                return fail_compile("pass '" + passes_[i].name + "' accesses resource " +
                    std::to_string(access.resource.id) + " with the wrong resource kind");
            }
            if (const auto* texture = std::get_if<TextureDesc>(&resource->description)) {
                const auto colorAttachment = color_attachment_index(access.usage) != UINT32_MAX;
                if (colorAttachment && !texture->renderTarget) {
                    return fail_compile("pass '" + passes_[i].name + "' uses a texture without renderTarget capability");
                }
                if (requires_storage(access.usage) && !texture->storage) {
                    return fail_compile("pass '" + passes_[i].name + "' uses a texture without storage capability");
                }
            } else if (const auto* buffer = std::get_if<BufferDesc>(&resource->description)) {
                if (requires_storage(access.usage) &&
                    (access.usage == ResourceUsage::ShaderWrite || access.usage == ResourceUsage::StorageWrite) &&
                    !buffer->storageBuffer) {
                    return fail_compile("pass '" + passes_[i].name + "' writes a buffer without storage capability");
                }
            }
            if (access.usage == ResourceUsage::Unknown || !usage_matches_kind(resource->handle.kind, access.usage)) {
                return fail_compile("pass '" + passes_[i].name + "' uses resource " +
                    std::to_string(access.resource.id) + " with an incompatible usage");
            }
            if (passes_[i].queue == RenderQueue::Compute &&
                (color_attachment_index(access.usage) != UINT32_MAX || access.usage == ResourceUsage::DepthStencil ||
                 access.usage == ResourceUsage::VertexBuffer || access.usage == ResourceUsage::IndexBuffer ||
                 access.usage == ResourceUsage::Present)) {
                return fail_compile("pass '" + passes_[i].name + "' uses a graphics-only resource usage on the compute queue");
            }
            if (passes_[i].queue == RenderQueue::Copy &&
                access.usage != ResourceUsage::CopySource && access.usage != ResourceUsage::CopyDestination) {
                return fail_compile("pass '" + passes_[i].name + "' uses a non-copy resource usage on the copy queue");
            }
            const bool write = is_write_usage(access.usage);
            const auto& declared = write ? passes_[i].writes : passes_[i].reads;
            if (std::none_of(declared.begin(), declared.end(), [access](ResourceHandle resource) {
                    return same_resource(resource, access.resource);
                })) {
                return fail_compile("pass '" + passes_[i].name + "' access declaration is inconsistent with its reads/writes");
            }
            if (passUsages.find(access.resource.id) != passUsages.end()) {
                return fail_compile("pass '" + passes_[i].name + "' accesses resource " +
                    std::to_string(access.resource.id) + " more than once");
            }
            passUsages.emplace(access.resource.id, access.usage);
        }
        if (has_present_access(passes_[i])) {
            if (passes_[i].queue != RenderQueue::Graphics) {
                return fail_compile("pass '" + passes_[i].name + "' presents on a non-graphics queue");
            }
            if (!passes_[i].writes.empty()) {
                return fail_compile("pass '" + passes_[i].name + "' must not write resources while presenting");
            }
            if (presentPassIndex != static_cast<std::size_t>(-1)) {
                return fail_compile("render graph contains more than one present pass");
            }
            presentPassIndex = i;
        }
        bool colorGap = false;
        for (std::uint32_t colorIndex = 0; colorIndex < 8; ++colorIndex) {
            const auto usage = static_cast<ResourceUsage>(static_cast<std::uint32_t>(ResourceUsage::ColorAttachment0) + colorIndex);
            const bool present = std::any_of(passes_[i].accesses.begin(), passes_[i].accesses.end(),
                [usage](const ResourceAccess& access) { return access.usage == usage; });
            if (!present) {
                colorGap = true;
            } else if (colorGap) {
                return fail_compile("pass '" + passes_[i].name + "' has a sparse color attachment list");
            }
        }
        for (const auto resource : passes_[i].reads) {
            const auto it = lastWriter.find(resource.id);
            if (it != lastWriter.end() && it->second != i) {
                addEdge(it->second, i);
            }
            readersSinceWrite[resource.id].push_back(i);
        }
        for (const auto resource : passes_[i].writes) {
            const auto it = lastWriter.find(resource.id);
            if (it != lastWriter.end() && it->second != i) {
                addEdge(it->second, i);
            }
            for (const auto reader : readersSinceWrite[resource.id]) addEdge(reader, i);
            readersSinceWrite[resource.id].clear();
            lastWriter[resource.id] = i;
        }
    }

    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
    for (std::size_t i = 0; i < count; ++i) if (indegree[i] == 0) ready.push(i);
    executionOrder_.clear();
    while (!ready.empty()) {
        const auto node = ready.top();
        ready.pop();
        executionOrder_.push_back(node);
        for (const auto next : edges[node]) if (--indegree[next] == 0) ready.push(next);
    }

    if (executionOrder_.size() != count) {
        return fail_compile("render graph contains a dependency cycle", false);
    }
    std::vector<bool> livePasses(count, false);
    for (std::size_t i = 0; i < count; ++i) {
        livePasses[i] = passes_[i].sideEffect;
        if (!livePasses[i] && has_present_access(passes_[i])) livePasses[i] = true;
        if (!livePasses[i]) {
            for (const auto write : passes_[i].writes) {
                const auto resource = std::find_if(resources_.begin(), resources_.end(),
                    [write](const ResourceNode& node) { return node.handle.id == write.id; });
                if (resource != resources_.end() && resource->external) {
                    livePasses[i] = true;
                    break;
                }
            }
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t consumer = 0; consumer < count; ++consumer) {
            if (!livePasses[consumer]) continue;
            for (std::size_t producer = 0; producer < count; ++producer) {
                if (livePasses[producer]) continue;
                if (std::find(edges[producer].begin(), edges[producer].end(), consumer) == edges[producer].end()) continue;
                livePasses[producer] = true;
                changed = true;
            }
        }
    }
    std::vector<std::size_t> scheduledOrder;
    scheduledOrder.reserve(executionOrder_.size());
    for (const auto passIndex : executionOrder_) if (livePasses[passIndex]) scheduledOrder.push_back(passIndex);
    diagnostics_.culledPassCount = count - scheduledOrder.size();
    executionOrder_ = std::move(scheduledOrder);
    if (presentPassIndex != static_cast<std::size_t>(-1)) {
        if (std::find(executionOrder_.begin(), executionOrder_.end(), presentPassIndex) == executionOrder_.end() ||
            executionOrder_.back() != presentPassIndex) {
            return fail_compile("present pass must be the terminal render graph pass");
        }
    }
    diagnostics_.crossQueueDependencyCount = 0;
    diagnostics_.graphicsPassCount = 0;
    diagnostics_.computePassCount = 0;
    diagnostics_.copyPassCount = 0;
    queueDependencies_.clear();
    for (const auto passIndex : executionOrder_) {
        if (passes_[passIndex].queue == RenderQueue::Graphics) ++diagnostics_.graphicsPassCount;
        else if (passes_[passIndex].queue == RenderQueue::Compute) ++diagnostics_.computePassCount;
        else ++diagnostics_.copyPassCount;
    }
    for (std::size_t producer = 0; producer < count; ++producer) {
        if (!livePasses[producer]) continue;
        for (const auto consumer : edges[producer]) {
            if (!livePasses[consumer]) continue;
            if (passes_[producer].queue == passes_[consumer].queue) continue;
            ResourceHandle dependencyResource{};
            for (const auto producerAccess : passes_[producer].accesses) {
                for (const auto consumerAccess : passes_[consumer].accesses) {
                    if (producerAccess.resource.id != consumerAccess.resource.id ||
                        (!is_write_usage(producerAccess.usage) && !is_write_usage(consumerAccess.usage))) continue;
                    dependencyResource = producerAccess.resource;
                    break;
                }
                if (dependencyResource) break;
            }
            queueDependencies_.push_back({producer, consumer, passes_[producer].queue,
                passes_[consumer].queue, dependencyResource});
        }
    }
    diagnostics_.crossQueueDependencyCount = queueDependencies_.size();
    std::unordered_map<std::size_t, std::uint32_t> passToBatch;
    for (const auto passIndex : executionOrder_) {
        if (queueBatches_.empty() || queueBatches_.back().queue != passes_[passIndex].queue) {
            queueBatches_.push_back({static_cast<std::uint32_t>(queueBatches_.size()), passes_[passIndex].queue, {}, {}});
        }
        auto& batch = queueBatches_.back();
        batch.passIndices.push_back(passIndex);
        passToBatch[passIndex] = batch.index;
    }
    for (const auto& dependency : queueDependencies_) {
        const auto producer = passToBatch.find(dependency.producerPass);
        const auto consumer = passToBatch.find(dependency.consumerPass);
        if (producer == passToBatch.end() || consumer == passToBatch.end() || producer->second == consumer->second) continue;
        auto& waits = queueBatches_[consumer->second].waitBatches;
        if (std::find(waits.begin(), waits.end(), producer->second) == waits.end()) waits.push_back(producer->second);
    }
    for (auto& resource : resources_) resource.physicalHandle = resource.handle;
    const auto ordered_before = [&](std::size_t producer, std::size_t consumer) {
        if (producer == consumer || passes_[producer].queue == passes_[consumer].queue) return true;
        std::vector<std::size_t> pending{producer};
        std::unordered_set<std::size_t> visited{producer};
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            for (const auto next : edges[current]) {
                if (next == consumer) return true;
                if (visited.insert(next).second) pending.push_back(next);
            }
        }
        return false;
    };
    const auto hash_combine = [](std::uint64_t seed, std::uint64_t value) {
        return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u));
    };
    const auto compatibility_key = [&hash_combine](const ResourceDesc& description) {
        std::uint64_t key = 0xcbf29ce484222325ull;
        if (const auto* texture = std::get_if<TextureDesc>(&description)) {
            key = hash_combine(key, texture->width);
            key = hash_combine(key, texture->height);
            key = hash_combine(key, texture->layers);
            key = hash_combine(key, texture->mipLevels);
            key = hash_combine(key, std::hash<std::string>{}(texture->format));
            key = hash_combine(key, texture->renderTarget);
            key = hash_combine(key, texture->generateMips);
            key = hash_combine(key, texture->hdr);
            key = hash_combine(key, std::hash<std::string>{}(texture->colorSpace));
            key = hash_combine(key, texture->storage);
        } else if (const auto* buffer = std::get_if<BufferDesc>(&description)) {
            key = hash_combine(key, buffer->size);
            key = hash_combine(key, buffer->stride);
            key = hash_combine(key, buffer->vertexBuffer);
            key = hash_combine(key, buffer->indexBuffer);
            key = hash_combine(key, buffer->indirectBuffer);
            key = hash_combine(key, buffer->structuredBuffer);
            key = hash_combine(key, buffer->storageBuffer);
        }
        return key;
    };
    const auto resource_memory_type = [&compatibility_key](const ResourceNode& node) {
        const auto descriptionKey = compatibility_key(node.description);
        const auto kindKey = static_cast<std::uint64_t>(node.handle.kind) << 24u;
        return static_cast<GpuMemoryType>((descriptionKey ^ kindKey) & 0xffffffffu);
    };
    std::vector<TransientResourceRequest> aliasRequests;
    std::unordered_map<std::uint32_t, std::size_t> aliasRequestIndices;
    for (std::size_t order = 0; order < executionOrder_.size(); ++order) {
        const auto& pass = passes_[executionOrder_[order]];
        for (const auto access : pass.accesses) {
            const auto node = std::find_if(resources_.begin(), resources_.end(),
                [access](const ResourceNode& resource) { return resource.handle.id == access.resource.id; });
            if (node == resources_.end() || node->external ||
                (node->handle.kind != ResourceKind::Texture2D &&
                 node->handle.kind != ResourceKind::DepthStencil &&
                 node->handle.kind != ResourceKind::Buffer)) continue;
            const auto found = aliasRequestIndices.find(node->handle.id);
            if (found == aliasRequestIndices.end()) {
                TransientResourceRequest request;
                request.resourceId = node->handle.id;
                if (const auto* texture = std::get_if<TextureDesc>(&node->description)) {
                    request.size = static_cast<std::size_t>(texture->width) * texture->height *
                        std::max<std::uint32_t>(texture->layers, 1u) * 4u;
                } else if (const auto* buffer = std::get_if<BufferDesc>(&node->description)) {
                    request.size = buffer->size;
                }
                request.alignment = 256;
                request.memoryType = resource_memory_type(*node);
                request.firstUse = order;
                request.lastUse = order;
                request.firstPass = executionOrder_[order];
                request.lastPass = executionOrder_[order];
                aliasRequestIndices.emplace(node->handle.id, aliasRequests.size());
                aliasRequests.push_back(request);
            } else {
                auto& request = aliasRequests[found->second];
                request.lastUse = order;
                request.lastPass = executionOrder_[order];
            }
        }
    }
    TransientAliasPlanOptions aliasOptions;
    aliasOptions.canAlias = [&ordered_before](const TransientResourceRequest& previous,
                                               const TransientResourceRequest& next) {
        return ordered_before(previous.lastPass, next.firstPass);
    };
    GpuMemoryAllocator aliasPlanner;
    aliasPlan_ = aliasPlanner.plan_transient_aliases(aliasRequests, aliasOptions);
    for (auto& resource : resources_) {
        if (resource.external) continue;
        const auto physicalId = aliasPlan_.physical_resource(resource.handle.id);
        if (physicalId) resource.physicalHandle = ResourceHandle{static_cast<std::uint32_t>(physicalId), resource.handle.kind};
    }
    diagnostics_.plannedTransitionCount = 0;
    std::unordered_map<std::uint32_t, ResourceUsage> previousUsage;
    for (const auto passIndex : executionOrder_) {
        for (const auto access : passes_[passIndex].accesses) {
            const auto node = std::find_if(resources_.begin(), resources_.end(),
                [access](const ResourceNode& resource) { return resource.handle.id == access.resource.id; });
            if (node == resources_.end() || !requires_physical_state(node->handle.kind)) continue;
            const auto physicalId = node == resources_.end() ? access.resource.id : node->physicalHandle.id;
            const auto previous = previousUsage.find(physicalId);
            if (previous == previousUsage.end() || previous->second != access.usage) {
                ++diagnostics_.plannedTransitionCount;
                transitions_.push_back({access.resource,
                    previous == previousUsage.end() ? ResourceUsage::Unknown : previous->second,
                    access.usage, passIndex});
            }
            previousUsage[physicalId] = access.usage;
        }
    }
    diagnostics_.compileNanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - compileStart).count());
    diagnostics_.passCount = passes_.size();
    diagnostics_.resourceCount = resources_.size();
    diagnostics_.physicalTextureCount = 0;
    diagnostics_.aliasedTextureCount = 0;
    diagnostics_.physicalBufferCount = 0;
    diagnostics_.aliasedBufferCount = 0;
    diagnostics_.hdrPassCount = 0;
    for (const auto& resource : resources_) {
        if (resource.handle.kind != ResourceKind::Texture2D || resource.external) continue;
        if (resource.physicalHandle.id == resource.handle.id) ++diagnostics_.physicalTextureCount;
        else ++diagnostics_.aliasedTextureCount;
    }
    for (const auto& resource : resources_) {
        if (resource.handle.kind != ResourceKind::Buffer || resource.external) continue;
        if (resource.physicalHandle.id == resource.handle.id) ++diagnostics_.physicalBufferCount;
        else ++diagnostics_.aliasedBufferCount;
    }
    for (const auto passIndex : executionOrder_) {
        for (const auto access : passes_[passIndex].accesses) {
            const auto resource = std::find_if(resources_.begin(), resources_.end(),
                [access](const ResourceNode& node) { return node.handle.id == access.resource.id; });
            if (resource == resources_.end()) continue;
            const auto texture = std::get_if<TextureDesc>(&resource->description);
            if (texture && texture->hdr) {
                ++diagnostics_.hdrPassCount;
                break;
            }
        }
    }
    compiled_ = true;
    return true;
}

void RenderGraph::execute(IRenderBackend& backend, std::string* error,
                          const std::function<void(IRenderBackend&)>& beforePresent) {
    const auto executeStart = std::chrono::steady_clock::now();
    const auto finish_execution_diagnostics = [this, &executeStart]() {
        diagnostics_.executeNanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - executeStart).count());
        diagnostics_.passCount = passes_.size();
        diagnostics_.resourceCount = resources_.size();
    };
    if (error) error->clear();
    backend.clear_error();
    diagnostics_.executionFailed = false;
    diagnostics_.executedPassCount = 0;
    diagnostics_.failedPassIndex = static_cast<std::size_t>(-1);
    diagnostics_.failedQueueBatch = static_cast<std::uint32_t>(-1);
    diagnostics_.lastError.clear();
    bool frameDiscarded = false;
    const auto discard_frame = [&]() {
        if (!frameDiscarded) {
            backend.discard_frame();
            frameDiscarded = true;
        }
    };
    if (!compiled_ && !compile(error)) {
        diagnostics_.executionFailed = true;
        diagnostics_.lastError = error && !error->empty() ? *error : diagnostics_.lastError;
        finish_execution_diagnostics();
        return;
    }
    passTimings_.clear();
    for (const auto& resource : resources_) {
        if (!resource.external && resource.physicalHandle.id == resource.handle.id) {
            if (!backend.create_resource(resource.handle, resource.description)) {
                if (error) *error = backend.last_error().empty() ?
                    "render graph transient resource creation failed" : backend.last_error();
                diagnostics_.lastError = error ? *error : "render graph transient resource creation failed";
                diagnostics_.executionFailed = true;
                for (const auto& cleanup : resources_) {
                    if (!cleanup.external) backend.retire_resource(cleanup.handle);
                }
                finish_execution_diagnostics();
                return;
            }
            std::string name = "rg_" + std::to_string(resource.handle.id);
            backend.set_debug_name(resource.handle, name);
        }
    }
    for (const auto& resource : resources_) {
        if (!resource.external && resource.physicalHandle.id != resource.handle.id &&
            !backend.alias_resource(resource.handle, resource.physicalHandle)) {
            if (error) *error = "backend could not alias transient resource";
            diagnostics_.lastError = error ? *error : "backend could not alias transient resource";
            diagnostics_.executionFailed = true;
            for (const auto& cleanup : resources_) {
                if (!cleanup.external) backend.retire_resource(cleanup.handle);
            }
            finish_execution_diagnostics();
            return;
        }
    }
    const auto retire_transients = [&backend, this]() {
        // Retire aliases first so their backend views/references are released
        // before the physical allocation they point at.
        for (const auto& resource : resources_) {
            if (!resource.external && resource.physicalHandle.id != resource.handle.id) {
                backend.retire_resource(resource.handle);
            }
        }
        for (const auto& resource : resources_) {
            if (!resource.external && resource.physicalHandle.id == resource.handle.id) {
                backend.retire_resource(resource.handle);
            }
        }
    };
    backend.begin_frame();
    const auto abort_if_device_unavailable = [&]() {
        const auto capabilities = backend.capabilities();
        if (capabilities.deviceReady && capabilities.deviceState != RenderDeviceState::Lost &&
            capabilities.deviceState != RenderDeviceState::NeedsResize) return false;
        if (error) {
            *error = backend.last_error();
            if (error->empty()) {
                *error = capabilities.deviceState == RenderDeviceState::NeedsResize
                    ? "render graph execution requires a valid resized swapchain"
                    : "render graph execution aborted because the device is unavailable";
            }
        }
        return true;
    };
    if (abort_if_device_unavailable()) {
        diagnostics_.executionFailed = true;
        diagnostics_.lastError = error ? *error : backend.last_error();
        discard_frame();
        retire_transients();
        backend.collect_garbage();
        finish_execution_diagnostics();
        return;
    }
    std::uint32_t lastGraphicsBatch = UINT32_MAX;
    std::uint32_t presentBatch = UINT32_MAX;
    std::size_t presentPassIndex = static_cast<std::size_t>(-1);
    for (const auto& batch : queueBatches_) {
        if (batch.queue == RenderQueue::Graphics) lastGraphicsBatch = batch.index;
        for (const auto passIndex : batch.passIndices) {
            if (has_present_access(passes_[passIndex])) {
                presentBatch = batch.index;
                presentPassIndex = passIndex;
                break;
            }
        }
    }
    const auto presentationBatch = presentBatch != UINT32_MAX ? presentBatch : lastGraphicsBatch;
    for (const auto& batch : queueBatches_) {
        if (!backend.begin_queue(batch.queue, batch.index, batch.waitBatches)) {
            if (error) *error = backend.last_error().empty() ?
                "render backend failed to begin queue batch" : backend.last_error();
            diagnostics_.lastError = error ? *error : "render backend failed to begin queue batch";
            diagnostics_.executionFailed = true;
            diagnostics_.failedQueueBatch = batch.index;
            if (!batch.passIndices.empty()) diagnostics_.failedPassIndex = batch.passIndices.front();
            break;
        }
        for (const auto index : batch.passIndices) {
        auto& pass = passes_[index];
        const auto start = std::chrono::steady_clock::now();
        bool transitionFailed = false;
        for (const auto access : pass.accesses) {
            const auto node = std::find_if(resources_.begin(), resources_.end(),
                [access](const ResourceNode& resource) {
                    return resource.handle.id == access.resource.id && resource.handle.kind == access.resource.kind;
                });
            if (node == resources_.end() || !requires_physical_state(node->handle.kind)) continue;
            const auto physical = physical_resource(access.resource);
            if (!backend.transition_resource(physical ? physical : access.resource, access.usage)) {
                transitionFailed = true;
                if (error) *error = "render pass '" + pass.name + "' resource transition failed for resource " +
                    std::to_string((physical ? physical : access.resource).id) + ": " + backend.last_error();
                diagnostics_.lastError = error ? *error : backend.last_error();
                diagnostics_.executionFailed = true;
                diagnostics_.failedPassIndex = index;
                diagnostics_.failedQueueBatch = batch.index;
                break;
            }
        }
        if (transitionFailed) {
            break;
        }
        std::vector<ResourceHandle> colorTargets(8);
        ResourceHandle depthTarget{};
        for (const auto access : pass.accesses) {
            const auto colorIndex = color_attachment_index(access.usage);
            if (colorIndex != UINT32_MAX) {
                colorTargets[colorIndex] = access.resource;
            } else if (access.usage == ResourceUsage::DepthStencil) depthTarget = access.resource;
        }
        if (pass.queue == RenderQueue::Graphics) {
            while (!colorTargets.empty() && !colorTargets.back()) colorTargets.pop_back();
            if (!colorTargets.empty() || depthTarget) {
                backend.set_render_targets(colorTargets, depthTarget, pass.clearAttachments);
            }
        }
        backend.begin_debug_label(pass.name);
        backend.begin_pass();
        const auto beforeStats = backend.stats();
        RenderPassContext context{pass.name, pass.reads, pass.writes, pass.accesses, pass.queue};
        backend.execute(context);
        if (pass.callback) pass.callback(backend, context);
        backend.end_pass();
        backend.end_debug_label();
        if (!backend.last_error().empty()) {
            if (error) *error = "render pass '" + pass.name + "' failed: " + backend.last_error();
            diagnostics_.lastError = error ? *error : backend.last_error();
            diagnostics_.executionFailed = true;
            diagnostics_.failedPassIndex = index;
            diagnostics_.failedQueueBatch = batch.index;
            break;
        }
        ++diagnostics_.executedPassCount;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        const auto afterStats = backend.stats();
        passTimings_.push_back({pass.name, static_cast<std::uint64_t>(elapsed), 0,
            afterStats.drawCalls - beforeStats.drawCalls,
            afterStats.barriers - beforeStats.barriers,
            static_cast<std::uint64_t>(pass.reads.size()),
            static_cast<std::uint64_t>(pass.writes.size())});
        }
        if (diagnostics_.executionFailed) {
            discard_frame();
            break;
        }
        if (beforePresent && batch.queue == RenderQueue::Graphics && batch.index == presentationBatch) {
            beforePresent(backend);
            if (!backend.last_error().empty()) {
                if (error) *error = "ImGui overlay failed: " + backend.last_error();
                diagnostics_.lastError = error ? *error : backend.last_error();
                diagnostics_.executionFailed = true;
                diagnostics_.failedQueueBatch = batch.index;
                if (presentPassIndex != static_cast<std::size_t>(-1)) diagnostics_.failedPassIndex = presentPassIndex;
                break;
            }
        }
        if (!backend.end_queue(batch.queue, batch.index, batch.index + 1 == queueBatches_.size(),
            batch.queue == RenderQueue::Graphics && batch.index == presentationBatch)) {
            if (error) *error = backend.last_error().empty() ?
                "render backend failed to submit queue batch" : backend.last_error();
            diagnostics_.lastError = error ? *error : "render backend failed to submit queue batch";
            diagnostics_.executionFailed = true;
            diagnostics_.failedQueueBatch = batch.index;
            if (!batch.passIndices.empty()) diagnostics_.failedPassIndex = batch.passIndices.back();
            break;
        }
        if (abort_if_device_unavailable()) {
            diagnostics_.executionFailed = true;
            diagnostics_.failedQueueBatch = batch.index;
            diagnostics_.lastError = error ? *error : backend.last_error();
            if (!batch.passIndices.empty()) diagnostics_.failedPassIndex = batch.passIndices.back();
            break;
        }
    }
    if (diagnostics_.executionFailed) discard_frame();
    else backend.end_frame();
    if (!backend.last_error().empty() && !diagnostics_.executionFailed) {
        if (error) *error = backend.last_error();
        diagnostics_.lastError = error ? *error : backend.last_error();
        diagnostics_.executionFailed = true;
        if (!executionOrder_.empty()) diagnostics_.failedPassIndex = executionOrder_.back();
    }
    retire_transients();
    const auto afterFrameStats = backend.stats();
    if (afterFrameStats.gpuPassNanoseconds.size() == passTimings_.size()) {
        diagnostics_.gpuTimingDelayed = true;
        for (std::size_t index = 0; index < passTimings_.size(); ++index) {
            passTimings_[index].gpuNanoseconds = afterFrameStats.gpuPassNanoseconds[index];
        }
    } else {
        diagnostics_.gpuTimingDelayed = false;
    }
    finish_execution_diagnostics();
    backend.collect_garbage();
}

void RenderGraph::reset() {
    passes_.clear();
    resources_.clear();
    executionOrder_.clear();
    transitions_.clear();
    queueDependencies_.clear();
    queueBatches_.clear();
    passTimings_.clear();
    aliasPlan_ = {};
    diagnostics_ = {};
    nextResource_ = 0x80000000u;
    compiled_ = false;
}
}

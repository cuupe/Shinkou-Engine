#include "shinkou/render/RenderCommandList.h"
#include <algorithm>
#include <cmath>
#include <type_traits>

namespace shinkou::render {
bool RenderCommandList::validate(const RenderPassContext &pass, std::string &error) const {
    error.clear();
    const auto resource = [&](ResourceHandle handle, ResourceUsage usage, bool optional = false) {
        if (!handle && optional)
            return true;
        const bool found =
            handle &&
            std::any_of(pass.accesses.begin(), pass.accesses.end(), [&](const auto &access) {
                return access.resource.id == handle.id && access.resource.kind == handle.kind &&
                       access.usage == usage;
            });
        if (!found)
            error = "command references an undeclared resource/usage: " + std::to_string(handle.id);
        return found;
    };
    for (const auto &command : commands_) {
        const bool valid = std::visit(
            [&](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, BindPipeline> || std::is_same_v<T, BindMaterial>)
                    return resource(value.resource, ResourceUsage::ShaderRead);
                else if constexpr (std::is_same_v<T, BindUniform>)
                    return resource(value.resource, ResourceUsage::UniformBuffer);
                else if constexpr (std::is_same_v<T, Targets>) {
                    if (pass.queue != RenderQueue::Graphics || value.colors.size() > 8)
                        return false;
                    for (std::size_t i = 0; i < value.colors.size(); ++i)
                        if (!resource(
                                value.colors[i],
                                static_cast<ResourceUsage>(
                                    static_cast<unsigned>(ResourceUsage::ColorAttachment0) + i),
                                true))
                            return false;
                    return resource(value.depth, ResourceUsage::DepthStencil, true);
                } else if constexpr (std::is_same_v<T, Viewport>)
                    return pass.queue == RenderQueue::Graphics && std::isfinite(value.x) &&
                           std::isfinite(value.y) && std::isfinite(value.width) &&
                           std::isfinite(value.height) && value.width > 0 && value.height > 0 &&
                           value.minDepth >= 0 && value.maxDepth <= 1 &&
                           value.minDepth <= value.maxDepth;
                else if constexpr (std::is_same_v<T, SpriteDraw>)
                    return pass.queue == RenderQueue::Graphics &&
                           resource(value.texture, ResourceUsage::ShaderRead, true);
                else if constexpr (std::is_same_v<T, MeshDraw> ||
                                   std::is_same_v<T, IndirectMeshDraw>) {
                    if (pass.queue != RenderQueue::Graphics ||
                        !resource(value.indexBuffer, ResourceUsage::IndexBuffer))
                        return false;
                    if (!resource(value.vertexBuffer, ResourceUsage::VertexBuffer,
                                  value.vertexBindings.size() > 0))
                        return false;
                    for (const auto &stream : value.vertexBindings)
                        if (!resource(stream.buffer, ResourceUsage::VertexBuffer))
                            return false;
                    if constexpr (std::is_same_v<T, IndirectMeshDraw>)
                        return resource(value.argumentBuffer, ResourceUsage::IndirectArguments);
                    return true;
                } else
                    return pass.queue != RenderQueue::Copy && value.groupCountX &&
                           value.groupCountY && value.groupCountZ;
            },
            command);
        if (!valid) {
            if (error.empty())
                error = "command is invalid for its render pass";
            return false;
        }
    }
    return true;
}

bool RenderCommandList::replay(IRenderBackend &backend, std::string &error) const {
    for (const auto &command : commands_) {
        bool submitted = true;
        std::visit(
            [&](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, BindPipeline>)
                    backend.bind_pipeline(value.resource);
                else if constexpr (std::is_same_v<T, BindMaterial>)
                    backend.bind_material(value.resource);
                else if constexpr (std::is_same_v<T, BindUniform>)
                    backend.bind_uniform_buffer(value.resource, value.slot, value.space);
                else if constexpr (std::is_same_v<T, Viewport>)
                    backend.set_viewport(value.x, value.y, value.width, value.height,
                                         value.minDepth, value.maxDepth);
                else if constexpr (std::is_same_v<T, Targets>)
                    backend.set_render_targets(value.colors, value.depth, value.clear);
                else if constexpr (std::is_same_v<T, SpriteDraw>)
                    backend.draw_sprite(value);
                else if constexpr (std::is_same_v<T, MeshDraw>)
                    backend.draw_mesh(value);
                else if constexpr (std::is_same_v<T, IndirectMeshDraw>)
                    backend.draw_mesh_indirect(value);
                else
                    submitted = backend.dispatch(value);
            },
            command);
        if (!submitted || !backend.last_error().empty()) {
            error = backend.last_error().empty() ? "recorded command submission failed"
                                                 : backend.last_error();
            return false;
        }
    }
    return true;
}
} // namespace shinkou::render

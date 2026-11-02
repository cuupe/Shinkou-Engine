#pragma once

#include "shinkou/render/RenderBackend.h"
#include <variant>

namespace shinkou::render {
// Worker recording can only append value commands. It cannot create/update
// resources, access a native device, or submit GPU work.
class RenderCommandList {
    struct BindPipeline {
        ResourceHandle resource;
    };
    struct BindMaterial {
        ResourceHandle resource;
    };
    struct BindUniform {
        ResourceHandle resource;
        std::uint32_t slot, space;
    };
    struct Viewport {
        float x, y, width, height, minDepth, maxDepth;
    };
    struct Targets {
        std::vector<ResourceHandle> colors;
        ResourceHandle depth;
        bool clear;
    };
    using Command = std::variant<BindPipeline, BindMaterial, BindUniform, Viewport, Targets,
                                 SpriteDraw, MeshDraw, IndirectMeshDraw, DispatchDesc>;
    std::vector<Command> commands_;

  public:
    void bind_pipeline(ResourceHandle resource) {
        commands_.push_back(BindPipeline{resource});
    }
    void bind_material(ResourceHandle resource) {
        commands_.push_back(BindMaterial{resource});
    }
    void bind_uniform_buffer(ResourceHandle resource, std::uint32_t slot, std::uint32_t space = 0) {
        commands_.push_back(BindUniform{resource, slot, space});
    }
    void set_viewport(float x, float y, float width, float height, float minDepth = 0,
                      float maxDepth = 1) {
        commands_.push_back(Viewport{x, y, width, height, minDepth, maxDepth});
    }
    void set_render_targets(std::vector<ResourceHandle> colors, ResourceHandle depth = {},
                            bool clear = false) {
        commands_.push_back(Targets{std::move(colors), depth, clear});
    }
    void draw_sprite(const SpriteDraw &draw) {
        commands_.push_back(draw);
    }
    void draw_mesh(const MeshDraw &draw) {
        commands_.push_back(draw);
    }
    void draw_mesh_indirect(const IndirectMeshDraw &draw) {
        commands_.push_back(draw);
    }
    void dispatch(const DispatchDesc &groups) {
        commands_.push_back(groups);
    }
    std::size_t size() const noexcept {
        return commands_.size();
    }
    bool validate(const RenderPassContext &pass, std::string &error) const;
    bool replay(IRenderBackend &backend, std::string &error) const;
};
} // namespace shinkou::render

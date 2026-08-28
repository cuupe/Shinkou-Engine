#include "shinkou/renderlib/CAPI.h"
#include "shinkou/renderlib/SoftwareDisplay.h"

#include <memory>

struct ShinkouRenderContext {
    shinkou::renderlib::SoftwareDisplayBackend backend;
    shinkou::renderlib::DisplayList list;
    shinkou::renderlib::Size size{};
    bool frameOpen = false;
};

using namespace shinkou::renderlib;

ShinkouRenderContext* shinkou_render_create(uint32_t width, uint32_t height) {
    auto* context = new ShinkouRenderContext();
    if (!context->backend.initialize({{width, height}, 1.0f, false})) { delete context; return nullptr; }
    context->size = {width, height};
    return context;
}
void shinkou_render_destroy(ShinkouRenderContext* context) { if (context) { context->backend.shutdown(); delete context; } }
int shinkou_render_begin(ShinkouRenderContext* context, uint32_t width, uint32_t height, uint32_t clearRgba8) {
    if (!context) return 0;
    if (context->size.width != width || context->size.height != height) { if (!context->backend.resize({width, height})) return 0; context->size = {width, height}; }
    context->list.clear(); context->backend.begin_frame(Color::from_rgba8(clearRgba8)); context->frameOpen = true; return 1;
}
void shinkou_render_rect(ShinkouRenderContext* context, float x, float y, float width, float height, uint32_t rgba8, float radius) { if (context && context->frameOpen) context->list.rect({x, y, width, height}, Color::from_rgba8(rgba8), radius); }
void shinkou_render_border(ShinkouRenderContext* context, float x, float y, float width, float height, uint32_t rgba8, float thickness, float radius) { if (context && context->frameOpen) context->list.border({x, y, width, height}, Color::from_rgba8(rgba8), thickness, radius); }
void shinkou_render_line(ShinkouRenderContext* context, float x1, float y1, float x2, float y2, uint32_t rgba8, float thickness) { if (context && context->frameOpen) context->list.line({x1, y1}, {x2, y2}, Color::from_rgba8(rgba8), thickness); }
void shinkou_render_gradient(ShinkouRenderContext* context, float x, float y, float width, float height, uint32_t startRgba8, uint32_t endRgba8) { if (context && context->frameOpen) context->list.gradient({x, y, width, height}, Color::from_rgba8(startRgba8), Color::from_rgba8(endRgba8)); }
int shinkou_render_end(ShinkouRenderContext* context) { if (!context || !context->frameOpen) return 0; context->backend.submit(context->list); context->frameOpen = false; return context->backend.end_frame() ? 1 : 0; }
const uint8_t* shinkou_render_pixels(const ShinkouRenderContext* context) { return context && !context->backend.pixels().empty() ? context->backend.pixels().data() : nullptr; }
size_t shinkou_render_pixel_bytes(const ShinkouRenderContext* context) { return context ? context->backend.pixels().size() : 0; }
uint32_t shinkou_render_width(const ShinkouRenderContext* context) { return context ? context->size.width : 0; }
uint32_t shinkou_render_height(const ShinkouRenderContext* context) { return context ? context->size.height : 0; }
uint64_t shinkou_render_frames(const ShinkouRenderContext* context) { return context ? context->backend.stats().frames : 0; }

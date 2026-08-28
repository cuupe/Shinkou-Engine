#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(SHINKOU_RENDER_BUILD_SHARED)
#define SHINKOU_RENDER_API __declspec(dllexport)
#elif defined(_WIN32)
#define SHINKOU_RENDER_API __declspec(dllimport)
#else
#define SHINKOU_RENDER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ShinkouRenderContext ShinkouRenderContext;

SHINKOU_RENDER_API ShinkouRenderContext* shinkou_render_create(uint32_t width, uint32_t height);
SHINKOU_RENDER_API void shinkou_render_destroy(ShinkouRenderContext* context);
SHINKOU_RENDER_API int shinkou_render_begin(ShinkouRenderContext* context, uint32_t width, uint32_t height, uint32_t clearRgba8);
SHINKOU_RENDER_API void shinkou_render_rect(ShinkouRenderContext* context, float x, float y, float width, float height, uint32_t rgba8, float radius);
SHINKOU_RENDER_API void shinkou_render_border(ShinkouRenderContext* context, float x, float y, float width, float height, uint32_t rgba8, float thickness, float radius);
SHINKOU_RENDER_API void shinkou_render_line(ShinkouRenderContext* context, float x1, float y1, float x2, float y2, uint32_t rgba8, float thickness);
SHINKOU_RENDER_API void shinkou_render_gradient(ShinkouRenderContext* context, float x, float y, float width, float height, uint32_t startRgba8, uint32_t endRgba8);
SHINKOU_RENDER_API int shinkou_render_end(ShinkouRenderContext* context);
SHINKOU_RENDER_API const uint8_t* shinkou_render_pixels(const ShinkouRenderContext* context);
SHINKOU_RENDER_API size_t shinkou_render_pixel_bytes(const ShinkouRenderContext* context);
SHINKOU_RENDER_API uint32_t shinkou_render_width(const ShinkouRenderContext* context);
SHINKOU_RENDER_API uint32_t shinkou_render_height(const ShinkouRenderContext* context);
SHINKOU_RENDER_API uint64_t shinkou_render_frames(const ShinkouRenderContext* context);

#ifdef __cplusplus
}
#endif

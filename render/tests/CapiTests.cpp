#include "shinkou/renderlib/CAPI.h"
#include <cassert>

int main() {
    ShinkouRenderContext* context = shinkou_render_create(32, 16);
    assert(context != nullptr);
    assert(shinkou_render_begin(context, 32, 16, 0x101820FFu) != 0);
    shinkou_render_rect(context, 4, 4, 10, 6, 0xFF0000FFu, 3.0f);
    shinkou_render_line(context, 0, 0, 31, 15, 0x00FF00FFu, 1.0f);
    assert(shinkou_render_end(context) != 0);
    assert(shinkou_render_width(context) == 32 && shinkou_render_height(context) == 16);
    assert(shinkou_render_pixel_bytes(context) == 32u * 16u * 4u);
    assert(shinkou_render_pixels(context) != nullptr);
    assert(shinkou_render_frames(context) == 1);
    shinkou_render_destroy(context);
    return 0;
}


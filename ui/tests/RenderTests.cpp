#include "shinkou/uikit/Render.h"
#include "shinkou/uikit/Ui.h"
#include <cassert>

using namespace shinkou::uikit;

int main() {
    RenderList list;
    list.reserve(8);
    list.rect({0, 0, 100, 100}, {1, 1, 1, 1});
    list.text({0, 0, 100, 20}, "微软雅黑", {0, 0, 0, 1});
    list.image({0, 20, 100, 80}, "asset://image");
    list.begin_clip({0, 0, 100, 100}); list.end_clip();
    const RenderStats stats = list.stats();
    assert(stats.commandCount == 5 && stats.textCount == 1 && stats.imageCount == 1 && stats.clipCount == 2);
    const VisibleRange range = visible_range(10000, 32.0f, 3200.0f, 640.0f, 2);
    assert(range.first == 98 && range.last == 122);
    return 0;
}

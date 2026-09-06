#include "shinkou/ui/Render.h"

#include <cassert>
#include <iostream>

int main() {
    shinkou::ui::UiRenderList list(2);
    list.rect({0, 0, 100, 40}, {1, 0, 0, 1}, 0);
    list.text({4, 4}, "toolbar", {1, 1, 1, 1});
    assert(list.size() == 2);
    const auto firstHash = list.content_hash();
    assert(firstHash != 0 && list.content_hash() == firstHash);
    list.text({4, 22}, "status", {1, 1, 1, 1});
    assert(list.content_hash() != firstHash);
    list.set_dirty_rect({2.0f, 3.0f, 16.0f, 12.0f});
    assert(list.has_dirty_rect());
    assert(!list.dirty_full());
    assert(list.dirty_rect().x == 2.0f && list.dirty_rect().bottom() == 15.0f);
    list.set_dirty_rect({0.0f, 0.0f, 100.0f, 40.0f}, true);
    assert(list.has_dirty_rect() && list.dirty_full());
    list.clear();
    assert(list.empty());
    assert(!list.has_dirty_rect() && list.dirty_full());
    assert(list.content_hash() != firstHash);
    const auto range = shinkou::ui::visible_range(10000, 20.0f, 1000.0f, 200.0f, 2);
    assert(range.first < 50 && range.last > range.first && range.last < 10000);
    assert(range.contentExtent == 200000.0f);
    std::cout << "UI render list and virtual range passed\n";
}

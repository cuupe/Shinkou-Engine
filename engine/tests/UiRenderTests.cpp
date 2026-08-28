#include "shinkou/ui/Render.h"

#include <cassert>
#include <iostream>

int main() {
    shinkou::ui::UiRenderList list(2);
    list.rect({0, 0, 100, 40}, {1, 0, 0, 1}, 0);
    list.text({4, 4}, "toolbar", {1, 1, 1, 1});
    assert(list.size() == 2);
    const auto range = shinkou::ui::visible_range(10000, 20.0f, 1000.0f, 200.0f, 2);
    assert(range.first < 50 && range.last > range.first && range.last < 10000);
    assert(range.contentExtent == 200000.0f);
    std::cout << "UI render list and virtual range passed\n";
}

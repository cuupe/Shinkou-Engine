#include "shinkou/renderlib/Render.h"
#include <cassert>

using namespace shinkou::renderlib;

int main() {
    auto software = create_software_backend();
    SoftwareDisplayBackend* raw = software.get();
    DisplayRenderer renderer(std::move(software));
    assert(renderer.initialize({{64, 32}, 1.0f, false}));

    DisplayList list;
    list.reserve(4);
    list.rect({4, 4, 20, 10}, {1, 0, 0, 1});
    list.border({3, 3, 22, 12}, {0, 1, 0, 1}, 1.0f);
    list.line({0, 0}, {63, 31}, {0, 0, 1, 1}, 1.0f);
    assert(renderer.render(list, {0.1f, 0.1f, 0.1f, 1.0f}));
    assert(renderer.stats().frames == 1 && renderer.stats().commands == 3);
    assert(raw->pixel(10, 8).r > 0.8f);
    assert(raw->pixel(0, 0).b > 0.8f);
    assert(raw->pixel(50, 20).r > 0.05f);
    assert(renderer.resize({128, 64}));
    assert(raw->size().width == 128 && raw->size().height == 64);
    assert(renderer.capabilities().software && renderer.capabilities().present);
    renderer.shutdown();
    return 0;
}

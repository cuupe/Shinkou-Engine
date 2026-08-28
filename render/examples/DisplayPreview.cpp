#include "shinkou/renderlib/Render.h"
#include <iostream>

using namespace shinkou::renderlib;

int main() {
    auto backend = create_software_backend();
    SoftwareDisplayBackend* output = backend.get();
    DisplayRenderer renderer(std::move(backend));
    if (!renderer.initialize({{640, 360}, 1.0f, false})) return 1;
    DisplayList scene;
    scene.gradient({0, 0, 640, 360}, {0.08f, 0.10f, 0.14f, 1}, {0.16f, 0.20f, 0.28f, 1});
    scene.rect({24, 24, 592, 56}, {0.12f, 0.15f, 0.20f, 1});
    scene.rect({24, 104, 260, 220}, {0.98f, 0.98f, 1.0f, 1});
    scene.rect({308, 104, 308, 104}, {0.14f, 0.18f, 0.24f, 1});
    scene.border({308, 104, 308, 104}, {0.26f, 0.62f, 0.96f, 1}, 2);
    scene.line({332, 248}, {588, 248}, {0.26f, 0.62f, 0.96f, 1}, 4);
    if (!renderer.render(scene, {0.05f, 0.06f, 0.08f, 1})) return 1;
    const bool saved = output->save_ppm("shinkou_render_preview.ppm");
    std::cout << "ShinkouRender display preview\nframes=" << renderer.stats().frames << " commands=" << renderer.stats().commands << " output=" << (saved ? "shinkou_render_preview.ppm" : "failed") << '\n';
    return saved ? 0 : 1;
}


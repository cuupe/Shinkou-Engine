#include "shinkou/render/PbrRenderPipeline.h"

#include <cassert>

using namespace shinkou::render;

int main() {
    PbrMaterialDesc material;
    material.metallic = 0.5f;
    material.roughness = 0.4f;
    assert(validate_pbr_material(material).valid());

    LightClusterDesc grid;
    grid.viewportWidth = 1920;
    grid.viewportHeight = 1080;
    const auto clusterGrid = make_light_cluster_grid(grid);
    assert(clusterGrid.tilesX == 120);
    assert(clusterGrid.tilesY == 68);
    assert(clusterGrid.slices == 24);
    assert(clusterGrid.clusterCount == 120ull * 68ull * 24ull);
    LightComponent invalidSpot;
    invalidSpot.type = LightType::Spot;
    invalidSpot.innerCone = 0.9f;
    invalidSpot.outerCone = 0.2f;
    assert(!validate_light_component(invalidSpot).valid());
    return 0;
}

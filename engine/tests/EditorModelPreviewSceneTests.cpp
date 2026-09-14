#include "shinkou/editor/EditorModelPreviewScene.h"

#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

int main() {
    auto snapshot = std::make_shared<shinkou::editor::EditorModelPreviewSnapshot>();
    snapshot->revision = 42;
    snapshot->vertexCount = 3;
    snapshot->triangleCount = 1;
    snapshot->minX = -1.0f;
    snapshot->minY = -1.0f;
    snapshot->minZ = 0.0f;
    snapshot->maxX = 1.0f;
    snapshot->maxY = 1.0f;
    snapshot->maxZ = 0.0f;
    snapshot->vertices = std::make_shared<const std::vector<shinkou::math::Vec3>>(
        std::initializer_list<shinkou::math::Vec3>{{-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}});
    snapshot->indices = std::make_shared<const std::vector<std::uint32_t>>(
        std::initializer_list<std::uint32_t>{0, 1, 2});
    snapshot->wireSegments = std::make_shared<const std::vector<shinkou::ui::Vec2>>(
        std::initializer_list<shinkou::ui::Vec2>{{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {0.5f, 0.0f},
                                                  {0.5f, 0.0f}, {0.0f, 1.0f}});
    assert(snapshot->valid());

    shinkou::editor::EditorModelPreviewScene scene;
    scene.set_snapshot(snapshot);
    const auto initial = scene.state();
    assert(initial && initial->valid() && initial->geometryRevision == 42);
    assert(scene.snapshot() == snapshot);
    const auto initialProjection = initial->projectionRevision;
    const auto initialYaw = initial->camera.yaw;

    scene.orbit({30.0f, -12.0f});
    const auto orbited = scene.state();
    assert(orbited && orbited->valid());
    assert(orbited->projectionRevision != initialProjection);
    assert(orbited->camera.yaw != initialYaw);
    assert(scene.snapshot() == snapshot);

    scene.zoom(1000.0f);
    assert(scene.camera().distance >= 0.75f && scene.camera().valid());
    scene.zoom(-1000.0f);
    assert(scene.camera().distance <= 8.0f && scene.camera().valid());
    scene.reset_camera();
    const auto reset = scene.state();
    assert(reset && reset->valid());
    assert(reset->camera.yaw == initial->camera.yaw && reset->camera.pitch == initial->camera.pitch &&
           reset->camera.distance == initial->camera.distance);
    scene.clear();
    assert(!scene.state() && !scene.snapshot());
    std::cout << "Editor model preview scene passed\n";
    return 0;
}

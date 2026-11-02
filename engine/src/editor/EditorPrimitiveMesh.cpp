#include "shinkou/editor/EditorPrimitiveMesh.h"
#include "shinkou/World.h"
#include <cmath>

namespace shinkou::editor {
namespace {
std::shared_ptr<const EditorModelPreviewSnapshot> build(std::string_view shape) {
    auto snapshot = std::make_shared<EditorModelPreviewSnapshot>();
    auto vertices = std::make_shared<std::vector<math::Vec3>>();
    auto indices = std::make_shared<std::vector<std::uint32_t>>();
    if (shape == "Cube") {
        *vertices = {{-.5f, -.5f, -.5f}, {.5f, -.5f, -.5f}, {.5f, .5f, -.5f}, {-.5f, .5f, -.5f},
                     {-.5f, -.5f, .5f},  {.5f, -.5f, .5f},  {.5f, .5f, .5f},  {-.5f, .5f, .5f}};
        *indices = {0, 3, 2, 0, 2, 1, 5, 6, 7, 5, 7, 4, 4, 7, 3, 4, 3, 0,
                    1, 2, 6, 1, 6, 5, 3, 7, 6, 3, 6, 2, 4, 0, 1, 4, 1, 5};
    } else if (shape == "Plane") {
        *vertices = {{-.5f, 0, -.5f}, {-.5f, 0, .5f}, {.5f, 0, .5f}, {.5f, 0, -.5f}};
        *indices = {0, 1, 2, 0, 2, 3};
    } else if (shape == "Sphere") {
        constexpr unsigned rings = 16, slices = 24;
        for (unsigned y = 0; y <= rings; ++y)
            for (unsigned x = 0; x <= slices; ++x) {
                const float latitude = math::Pi * y / rings, longitude = 2 * math::Pi * x / slices;
                vertices->push_back({.5f * std::sin(latitude) * std::cos(longitude),
                                     .5f * std::cos(latitude),
                                     .5f * std::sin(latitude) * std::sin(longitude)});
            }
        for (unsigned y = 0; y < rings; ++y)
            for (unsigned x = 0; x < slices; ++x) {
                const unsigned a = y * (slices + 1) + x, b = a + slices + 1;
                if (y)
                    indices->insert(indices->end(), {a, a + 1, b});
                if (y + 1 < rings)
                    indices->insert(indices->end(), {a + 1, b + 1, b});
            }
    } else
        return {};
    snapshot->revision = 1;
    snapshot->sourceFormat = "primitive";
    snapshot->vertexCount = vertices->size();
    snapshot->triangleCount = indices->size() / 3;
    snapshot->objectCount = snapshot->meshCount = snapshot->primitiveCount = 1;
    snapshot->minX = snapshot->minZ = -.5f;
    snapshot->maxX = snapshot->maxZ = .5f;
    snapshot->minY = shape == "Plane" ? 0 : -.5f;
    snapshot->maxY = shape == "Plane" ? 0 : .5f;
    snapshot->vertices = vertices;
    snapshot->indices = indices;
    auto wire = std::make_shared<std::vector<ui::Vec2>>();
    for (std::size_t i = 0; i < indices->size(); i += 3)
        for (unsigned edge = 0; edge < 3; ++edge) {
            const auto a = (*vertices)[(*indices)[i + edge]],
                       b = (*vertices)[(*indices)[i + (edge + 1) % 3]];
            wire->push_back({a.x + .5f, .5f - a.y});
            wire->push_back({b.x + .5f, .5f - b.y});
        }
    snapshot->wireSegments = wire;
    return snapshot;
}
} // namespace
std::shared_ptr<const EditorModelPreviewSnapshot> editor_primitive_mesh(std::string_view shape) {
    static const auto cube = build("Cube"), sphere = build("Sphere"), plane = build("Plane");
    if (shape == "Cube")
        return cube;
    if (shape == "Sphere")
        return sphere;
    if (shape == "Plane")
        return plane;
    return {};
}
void append_editor_primitives(const World &world,
                              std::vector<EditorModelSceneInstance> &instances) {
    world.each_game_object([&](const GameObject &object) {
        const auto *primitive = object.get_component<components::PrimitiveMeshComponent>();
        const auto *transform = object.get_component<components::TransformComponent>();
        if (!primitive || !primitive->enabled() || !object.active_in_hierarchy() || !transform)
            return;
        auto snapshot = editor_primitive_mesh(primitive->shape());
        if (!snapshot)
            return;
        // Immutable built-ins share geometry by shape. Per-object color lives
        // in the object uniform rather than in the shared snapshot.
        const assets::AssetId id = ~assets::AssetId{0} - (primitive->shape() == "Cube"     ? 0
                                                          : primitive->shape() == "Sphere" ? 1
                                                                                           : 2);
        EditorModelSceneInstance instance{id, object.id(), std::move(snapshot),
                                          transform->world_matrix()};
        instance.colorOverride = primitive->color();
        instances.push_back(std::move(instance));
    });
}
} // namespace shinkou::editor

#include "shinkou/editor/EditorModelPreview.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <ufbx.h>

namespace shinkou::editor {
EditorModelPreviewResult load_editor_fbx_preview(const FileSystemService &files,
                                                 std::string_view path, std::uint64_t generation,
                                                 std::uint64_t stamp,
                                                 const std::atomic_bool *cancel) {
    EditorModelPreviewResult result;
    result.path = path;
    result.generation = generation;
    result.sourceStamp = stamp;
    std::string bytes;
    bool truncated = false;
    if (!files.read_text_limited(std::filesystem::u8path(path), 128u * 1024u * 1024u, bytes,
                                 &truncated, &result.error))
        return result;
    if (truncated) {
        result.error = "FBX exceeds 128 MiB preview limit";
        return result;
    }
    if (cancel && cancel->load()) {
        result.error = "model preview cancelled";
        return result;
    }
    ufbx_load_opts opts{};
    opts.file_format = UFBX_FILE_FORMAT_FBX;
    opts.load_external_files = false;
    opts.ignore_embedded = true;
    opts.temp_allocator.memory_limit = 256u * 1024u * 1024u;
    opts.result_allocator.memory_limit = 256u * 1024u * 1024u;
    opts.target_axes = ufbx_axes_left_handed_y_up;
    opts.target_unit_meters = 1;
    opts.progress_cb.fn = [](void *user, const ufbx_progress *) {
        const auto *flag = static_cast<const std::atomic_bool *>(user);
        return flag && flag->load(std::memory_order_relaxed) ? UFBX_PROGRESS_CANCEL
                                                             : UFBX_PROGRESS_CONTINUE;
    };
    opts.progress_cb.user = const_cast<std::atomic_bool *>(cancel);
    ufbx_error error{};
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
        ufbx_load_memory(bytes.data(), bytes.size(), &opts, &error), ufbx_free_scene);
    if (!scene) {
        char message[512]{};
        ufbx_format_error(message, sizeof(message), &error);
        result.error = message;
        return result;
    }
    auto next = std::make_shared<EditorModelPreviewSnapshot>();
    auto vertices = std::make_shared<std::vector<math::Vec3>>();
    auto normals = std::make_shared<std::vector<math::Vec3>>();
    auto uv = std::make_shared<std::vector<math::Vec2>>();
    auto indices = std::make_shared<std::vector<std::uint32_t>>();
    auto materials = std::make_shared<std::vector<EditorModelMaterialPreview>>();
    auto nodes = std::make_shared<std::vector<EditorModelNodePreview>>();
    next->minX = next->minY = next->minZ = std::numeric_limits<float>::max();
    next->maxX = next->maxY = next->maxZ = std::numeric_limits<float>::lowest();
    for (auto *node : scene->nodes) {
        if (cancel && cancel->load()) {
            result.error = "model preview cancelled";
            return result;
        }
        nodes->push_back({std::string(node->name.data, node->name.length), -1, node->mesh ? 0 : -1,
                          animation::InvalidBone});
        const auto *mesh = node->mesh;
        if (!mesh)
            continue;
        ++next->objectCount;
        if (mesh->max_face_triangles > 1000000 || mesh->num_triangles > 500000 ||
            vertices->size() + mesh->num_triangles * 3 > 1500000) {
            result.error = "FBX geometry exceeds preview budget";
            return result;
        }
        std::vector<std::uint32_t> tri(mesh->max_face_triangles * 3);
        const auto normalMatrix = ufbx_matrix_for_normals(&node->geometry_to_world);
        for (const auto face : mesh->faces) {
            if (cancel && cancel->load()) {
                result.error = "model preview cancelled";
                return result;
            }
            const auto count = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
            for (std::size_t i = 0; i < count * 3u; ++i) {
                const auto p = ufbx_transform_position(
                    &node->geometry_to_world, ufbx_get_vertex_vec3(&mesh->vertex_position, tri[i]));
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                    std::abs(p.x) > 1e20 || std::abs(p.y) > 1e20 || std::abs(p.z) > 1e20) {
                    result.error = "FBX contains invalid positions";
                    return result;
                }
                const math::Vec3 v{static_cast<float>(p.x), static_cast<float>(p.y),
                                   static_cast<float>(p.z)};
                indices->push_back(static_cast<std::uint32_t>(vertices->size()));
                vertices->push_back(v);
                next->minX = std::min(next->minX, v.x);
                next->maxX = std::max(next->maxX, v.x);
                next->minY = std::min(next->minY, v.y);
                next->maxY = std::max(next->maxY, v.y);
                next->minZ = std::min(next->minZ, v.z);
                next->maxZ = std::max(next->maxZ, v.z);
                if (mesh->vertex_normal.exists) {
                    const auto n = ufbx_transform_direction(
                        &normalMatrix, ufbx_get_vertex_vec3(&mesh->vertex_normal, tri[i]));
                    normals->push_back({static_cast<float>(n.x), static_cast<float>(n.y),
                                        static_cast<float>(n.z)});
                } else
                    normals->push_back({0, 1, 0});
                if (mesh->vertex_uv.exists) {
                    const auto t = ufbx_get_vertex_vec2(&mesh->vertex_uv, tri[i]);
                    uv->push_back({static_cast<float>(t.x), static_cast<float>(t.y)});
                } else
                    uv->push_back({0, 0});
                if (!mesh->vertex_normal.exists && i % 3 == 2) {
                    const auto end = vertices->size();
                    const auto &a = (*vertices)[end - 3];
                    const auto &b = (*vertices)[end - 2];
                    const auto &c = (*vertices)[end - 1];
                    const auto n = math::Normalize(math::Cross(b - a, c - a));
                    (*normals)[end - 3] = (*normals)[end - 2] = (*normals)[end - 1] = n;
                }
            }
        }
    }
    if (indices->empty()) {
        result.error = "FBX contains no previewable triangles";
        return result;
    }
    for (const auto *m : scene->materials) {
        EditorModelMaterialPreview material;
        material.name.assign(m->name.data, m->name.length);
        const auto color = m->fbx.diffuse_color.value_vec3;
        if (m->fbx.diffuse_color.has_value)
            material.baseColorFactor = {static_cast<float>(color.x), static_cast<float>(color.y),
                                        static_cast<float>(color.z), 1};
        material.metallicFactor = static_cast<float>(m->pbr.metalness.value_real);
        material.roughnessFactor =
            m->pbr.roughness.has_value ? static_cast<float>(m->pbr.roughness.value_real) : 0.7f;
        materials->push_back(std::move(material));
    }
    auto wire = std::make_shared<std::vector<ui::Vec2>>();
    const float dx = std::max(1e-6f, next->maxX - next->minX),
                dy = std::max(1e-6f, next->maxY - next->minY);
    const auto project = [&](std::uint32_t i) {
        const auto &p = (*vertices)[i];
        return ui::Vec2{(p.x - next->minX) / dx, 1 - (p.y - next->minY) / dy};
    };
    for (std::size_t i = 0; i + 2 < indices->size() && wire->size() < 24000; i += 3)
        for (int j = 0; j < 3; ++j) {
            wire->push_back(project((*indices)[i + j]));
            wire->push_back(project((*indices)[i + (j + 1) % 3]));
        }
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : std::string(path)) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    hash ^= stamp;
    hash *= 1099511628211ull;
    next->revision = hash ? hash : 1;
    next->sourceFormat = "fbx";
    next->vertexCount = vertices->size();
    next->triangleCount = indices->size() / 3;
    next->meshCount = scene->meshes.count;
    next->primitiveCount = next->objectCount;
    next->materialCount = materials->size();
    next->vertices = vertices;
    next->indices = indices;
    next->normals = normals;
    next->textureCoordinates = uv;
    next->wireSegments = wire;
    next->materials = materials;
    next->nodes = nodes;
    // FBX animation is reported as metadata until converted to engine clips.
    auto animations = std::make_shared<std::vector<EditorModelAnimationPreview>>();
    for (const auto *stack : scene->anim_stacks) {
        EditorModelAnimationPreview a;
        a.name.assign(stack->name.data, stack->name.length);
        a.duration = static_cast<float>(stack->time_end - stack->time_begin);
        animations->push_back(std::move(a));
    }
    next->animations = animations;
    next->animationCount = animations->size();
    result.snapshot = next;
    return result;
}
} // namespace shinkou::editor

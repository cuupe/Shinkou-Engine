#include "shinkou/editor/EditorModelPreview.h"
#include "shinkou/editor/EditorGltfPreview.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace shinkou::editor {
namespace {

constexpr std::uintmax_t kMaxSourceBytes = 128u * 1024u * 1024u;
constexpr std::size_t kMaxVertices = 500000;
constexpr std::size_t kMaxTriangles = 1000000;
constexpr std::size_t kMaxWireSegments = 8192;
constexpr std::size_t kMaxLineBytes = 4096;

struct Point3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct FaceIndex {
    std::size_t a{0};
    std::size_t b{0};
    std::size_t c{0};
};

bool cancelled(const std::atomic_bool* value) noexcept {
    return value && value->load(std::memory_order_relaxed);
}

EditorModelPreviewResult failed(std::string path, std::uint64_t generation,
                                std::uint64_t sourceStamp, std::string error) {
    EditorModelPreviewResult result;
    result.generation = generation;
    result.sourceStamp = sourceStamp;
    result.path = std::move(path);
    result.error = std::move(error);
    return result;
}

bool parse_vertex(std::string_view line, Point3& point) {
    std::istringstream input{std::string(line)};
    char marker = 0;
    if (!(input >> marker >> point.x >> point.y >> point.z) || marker != 'v') return false;
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

bool parse_index(std::string_view token, std::ptrdiff_t& index) {
    const auto slash = token.find('/');
    const auto vertexToken = token.substr(0, slash);
    if (vertexToken.empty()) return false;
    try {
        std::size_t consumed = 0;
        const auto value = std::stoll(std::string(vertexToken), &consumed, 10);
        if (consumed != vertexToken.size() || value == 0 ||
            value < std::numeric_limits<std::ptrdiff_t>::min() ||
            value > std::numeric_limits<std::ptrdiff_t>::max()) return false;
        index = static_cast<std::ptrdiff_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

std::uint64_t revision(std::string_view path, std::uint64_t stamp,
                       std::size_t vertices, std::size_t triangles) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    };
    mix(path.data(), path.size());
    mix(&stamp, sizeof(stamp));
    mix(&vertices, sizeof(vertices));
    mix(&triangles, sizeof(triangles));
    return hash == 0 ? 1 : hash;
}

EditorModelPreviewResult load_obj_preview_stream(std::string path, std::istream& file,
                                                 std::uint64_t generation, std::uint64_t sourceStamp,
                                                 const std::atomic_bool* cancel) {
    std::vector<Point3> vertices;
    vertices.reserve(std::min<std::size_t>(kMaxVertices, 4096));
    std::vector<FaceIndex> faces;
    faces.reserve(std::min<std::size_t>(kMaxTriangles, 4096));
    std::string line;
    std::size_t objectCount = 0;
    while (std::getline(file, line)) {
        if (cancelled(cancel)) return failed(path, generation, sourceStamp, "model preview cancelled");
        if (line.size() > kMaxLineBytes) return failed(path, generation, sourceStamp,
            "model line exceeds the preview limit");
        std::string_view view(line);
        while (!view.empty() && (view.front() == ' ' || view.front() == '\t')) view.remove_prefix(1);
        if (view.empty() || view.front() == '#') continue;
        if (view.rfind("v ", 0) == 0 || view.rfind("v\t", 0) == 0) {
            if (vertices.size() >= kMaxVertices) return failed(path, generation, sourceStamp,
                "model vertex count exceeds the preview limit");
            Point3 point;
            if (!parse_vertex(view, point)) return failed(path, generation, sourceStamp,
                "model contains an invalid vertex");
            vertices.push_back(point);
            continue;
        }
        if (view.rfind("o ", 0) == 0 || view.rfind("g ", 0) == 0) {
            ++objectCount;
            continue;
        }
        if (view.rfind("f ", 0) != 0 && view.rfind("f\t", 0) != 0) continue;
        std::istringstream input{std::string(view.substr(1))};
        std::vector<std::size_t> polygon;
        std::string token;
        while (input >> token) {
            if (polygon.size() >= 1024) return failed(path, generation, sourceStamp,
                "model face has too many vertices");
            std::ptrdiff_t parsed = 0;
            if (!parse_index(token, parsed)) return failed(path, generation, sourceStamp,
                "model contains an invalid face index");
            const auto negativeMagnitude = parsed < 0 &&
                parsed != std::numeric_limits<std::ptrdiff_t>::min()
                    ? static_cast<std::size_t>(-parsed) : std::numeric_limits<std::size_t>::max();
            const auto resolved = parsed > 0 ? static_cast<std::size_t>(parsed - 1) :
                parsed < 0 && negativeMagnitude <= vertices.size()
                    ? vertices.size() - negativeMagnitude : vertices.size();
            if (resolved >= vertices.size()) return failed(path, generation, sourceStamp,
                "model face index is outside the vertex buffer");
            polygon.push_back(resolved);
        }
        if (polygon.size() < 3) return failed(path, generation, sourceStamp,
            "model face has fewer than three vertices");
        if (faces.size() + polygon.size() - 2 > kMaxTriangles)
            return failed(path, generation, sourceStamp, "model triangle count exceeds the preview limit");
        for (std::size_t index = 1; index + 1 < polygon.size(); ++index)
            faces.push_back({polygon[0], polygon[index], polygon[index + 1]});
    }
    if (vertices.empty() || faces.empty()) return failed(path, generation, sourceStamp,
        "model contains no previewable geometry");

    Point3 min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Point3 max{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (const auto& point : vertices) {
        min.x = std::min(min.x, point.x); min.y = std::min(min.y, point.y); min.z = std::min(min.z, point.z);
        max.x = std::max(max.x, point.x); max.y = std::max(max.y, point.y); max.z = std::max(max.z, point.z);
    }
    const Point3 extent{max.x - min.x, max.y - min.y, max.z - min.z};
    int horizontal = 0;
    int vertical = 1;
    if (extent.z > extent.x && extent.z >= extent.y) horizontal = 2;
    if ((horizontal == 0 && extent.z > extent.y) || (horizontal == 2 && extent.x > extent.y)) vertical = 1;
    else if (horizontal == 0) vertical = 2;
    else vertical = 0;
    const auto coordinate = [](const Point3& point, int axis) {
        return axis == 0 ? point.x : axis == 1 ? point.y : point.z;
    };
    const float minHorizontal = coordinate(min, horizontal);
    const float minVertical = coordinate(min, vertical);
    const float horizontalExtent = std::max(1.0e-6f, coordinate(max, horizontal) - minHorizontal);
    const float verticalExtent = std::max(1.0e-6f, coordinate(max, vertical) - minVertical);
    auto wire = std::make_shared<std::vector<ui::Vec2>>();
    wire->reserve(std::min<std::size_t>(faces.size() * 6, kMaxWireSegments * 2));
    const auto add_segment = [&](std::size_t first, std::size_t second) {
        if (wire->size() >= kMaxWireSegments * 2) return;
        const auto project = [&](std::size_t index) {
            const auto& point = vertices[index];
            return ui::Vec2{(coordinate(point, horizontal) - minHorizontal) / horizontalExtent,
                            1.0f - (coordinate(point, vertical) - minVertical) / verticalExtent};
        };
        wire->push_back(project(first));
        wire->push_back(project(second));
    };
    for (const auto& face : faces) {
        if (cancelled(cancel)) return failed(path, generation, sourceStamp, "model preview cancelled");
        add_segment(face.a, face.b);
        add_segment(face.b, face.c);
        add_segment(face.c, face.a);
        if (wire->size() >= kMaxWireSegments * 2) break;
    }
    auto snapshot = std::make_shared<EditorModelPreviewSnapshot>();
    snapshot->revision = revision(path, sourceStamp, vertices.size(), faces.size());
    snapshot->sourceFormat = "obj";
    snapshot->vertexCount = vertices.size();
    snapshot->triangleCount = faces.size();
    snapshot->objectCount = objectCount;
    snapshot->meshCount = objectCount;
    snapshot->primitiveCount = faces.empty() ? 0 : 1;
    snapshot->minX = min.x; snapshot->minY = min.y; snapshot->minZ = min.z;
    snapshot->maxX = max.x; snapshot->maxY = max.y; snapshot->maxZ = max.z;
    auto structuredVertices = std::make_shared<std::vector<math::Vec3>>();
    structuredVertices->reserve(vertices.size());
    for (const auto& point : vertices) structuredVertices->push_back({point.x, point.y, point.z});
    auto structuredIndices = std::make_shared<std::vector<std::uint32_t>>();
    structuredIndices->reserve(faces.size() * 3u);
    for (const auto& face : faces) {
        structuredIndices->push_back(static_cast<std::uint32_t>(face.a));
        structuredIndices->push_back(static_cast<std::uint32_t>(face.b));
        structuredIndices->push_back(static_cast<std::uint32_t>(face.c));
    }
    snapshot->vertices = std::move(structuredVertices);
    snapshot->indices = std::move(structuredIndices);
    snapshot->wireSegments = std::move(wire);
    EditorModelPreviewResult result;
    result.generation = generation;
    result.sourceStamp = sourceStamp;
    result.path = std::move(path);
    result.snapshot = std::move(snapshot);
    return result;
}

} // namespace

EditorModelPreviewResult load_editor_model_preview(
    const FileSystemService& files, std::string_view relativePath,
    std::uint64_t generation, std::uint64_t sourceStamp,
    const std::atomic_bool* cancel) {
    const std::string path(relativePath);
    auto extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension == ".gltf" || extension == ".glb")
        return load_editor_gltf_preview(files, relativePath, generation, sourceStamp, cancel);
    const auto absolute = files.resolve_existing(path);
    if (absolute.empty()) return failed(path, generation, sourceStamp,
        "model path is outside the project or no longer exists");
    std::error_code sizeError;
    const auto sourceBytes = std::filesystem::file_size(absolute, sizeError);
    if (sizeError) return failed(path, generation, sourceStamp, "model file size is unavailable");
    if (sourceBytes > kMaxSourceBytes) return failed(path, generation, sourceStamp,
        "model file exceeds the 128 MiB preview limit");
    if (cancelled(cancel)) return failed(path, generation, sourceStamp, "model preview cancelled");

    std::ifstream file(absolute, std::ios::binary);
    if (!file) return failed(path, generation, sourceStamp, "model file cannot be opened");
    return load_obj_preview_stream(path, file, generation, sourceStamp, cancel);
}

EditorModelPreviewResult load_editor_obj_preview_bytes(
    std::string_view relativePath, const std::vector<std::uint8_t>& sourceBytes,
    std::uint64_t generation, std::uint64_t sourceStamp,
    const std::atomic_bool* cancel) {
    const std::string path(relativePath);
    if (sourceBytes.size() > kMaxSourceBytes) return failed(path, generation, sourceStamp,
        "model bytes exceed the 128 MiB preview limit");
    if (cancelled(cancel)) return failed(path, generation, sourceStamp, "model preview cancelled");
    const std::string source(reinterpret_cast<const char*>(sourceBytes.data()), sourceBytes.size());
    std::istringstream file{source};
    return load_obj_preview_stream(path, file, generation, sourceStamp, cancel);
}

} // namespace shinkou::editor

#pragma once

#include "shinkou/Math.h"
#include "shinkou/animation/Animation.h"
#include "shinkou/editor/FileSystem.h"
#include "shinkou/ui/Ui.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::editor {

struct EditorModelImagePreview {
    std::string name;
    std::string uri;
    std::string mimeType;
    std::int32_t bufferView{-1};
};

// Immutable, renderer-neutral image bytes extracted from a glTF image table.
// The editor may hand these bytes to the regular bounded image provider; this
// is deliberately not a GPU texture or a decoded pixel buffer.
struct EditorModelTextureArtifact {
    std::int32_t imageIndex{-1};
    std::string uri;
    std::string mimeType;
    std::shared_ptr<const std::vector<std::uint8_t>> encodedBytes{};

    bool valid() const noexcept {
        return imageIndex >= 0 && encodedBytes && !encodedBytes->empty();
    }
};

struct EditorModelTexturePreview {
    std::string name;
    std::int32_t source{-1};
    std::int32_t sampler{-1};
};

struct EditorModelMaterialPreview {
    std::string name;
    std::array<float, 4> baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallicFactor{1.0f};
    float roughnessFactor{1.0f};
    std::int32_t baseColorTexture{-1};
    std::int32_t normalTexture{-1};
    std::int32_t metallicRoughnessTexture{-1};
    std::string alphaMode{"OPAQUE"};
    bool doubleSided{false};
};

struct EditorModelNodePreview {
    std::string name;
    std::int32_t parent{-1};
    std::int32_t mesh{-1};
    animation::BoneIndex bone{animation::InvalidBone};
};

struct EditorModelAnimationPreview {
    std::string name;
    std::size_t channelCount{0};
    std::size_t samplerCount{0};
    std::size_t playableChannelCount{0};
    float duration{0.0f};
    bool cpuPlayable{false};
    std::shared_ptr<const animation::AnimationClip> cpuClip{};
};

struct EditorModelPreviewSnapshot {
    std::uint64_t revision{0};
    std::string sourceFormat{"obj"};
    std::size_t vertexCount{0};
    std::size_t triangleCount{0};
    std::size_t objectCount{0};
    std::size_t meshCount{0};
    std::size_t primitiveCount{0};
    std::size_t materialCount{0};
    std::size_t textureCount{0};
    std::size_t imageCount{0};
    std::size_t animationCount{0};
    float minX{0.0f};
    float minY{0.0f};
    float minZ{0.0f};
    float maxX{0.0f};
    float maxY{0.0f};
    float maxZ{0.0f};
    // Structured geometry is immutable and renderer-neutral. The first
    // provider uses it for an isolated retained preview; GPU mesh upload is
    // deliberately a later lifecycle owned by the renderer backend.
    std::shared_ptr<const std::vector<math::Vec3>> vertices{};
    std::shared_ptr<const std::vector<std::uint32_t>> indices{};
    // Optional per-vertex TEXCOORD_0 data. Providers without texture
    // coordinates leave this empty; renderer fallbacks derive bounded UVs
    // from position instead of mutating the immutable geometry.
    std::shared_ptr<const std::vector<math::Vec2>> textureCoordinates{};
    // Optional per-vertex glTF NORMAL data. Providers without normals leave
    // this empty; the renderer uses a stable preview-light fallback.
    std::shared_ptr<const std::vector<math::Vec3>> normals{};
    // glTF material/image metadata and bounded encoded image artifacts are
    // immutable preview information. Artifacts are not decoded here; the
    // regular image provider owns those bounded snapshots and GPU upload
    // remains a later renderer-owned lifecycle.
    std::shared_ptr<const std::vector<EditorModelMaterialPreview>> materials{};
    std::shared_ptr<const std::vector<EditorModelTexturePreview>> textures{};
    std::shared_ptr<const std::vector<EditorModelImagePreview>> images{};
    std::shared_ptr<const std::vector<EditorModelTextureArtifact>> imageArtifacts{};
    std::shared_ptr<const std::vector<EditorModelNodePreview>> nodes{};
    std::shared_ptr<const animation::Skeleton> animationSkeleton{};
    // One entry per flattened vertex. InvalidBone means that the source
    // geometry is not attached to a previewable glTF node.
    std::shared_ptr<const std::vector<animation::BoneIndex>> vertexBones{};
    std::shared_ptr<const std::vector<EditorModelAnimationPreview>> animations{};
    // Each adjacent pair is a normalized wireframe segment in [0, 1].
    std::shared_ptr<const std::vector<ui::Vec2>> wireSegments{};

    bool valid() const noexcept {
        return revision != 0 && vertexCount > 0 && triangleCount > 0 &&
            vertices && vertices->size() == vertexCount && indices &&
            indices->size() >= 3u && indices->size() / 3u == triangleCount &&
            indices->size() % 3u == 0u && wireSegments &&
            wireSegments->size() >= 2 && (wireSegments->size() % 2) == 0;
    }
};

struct EditorModelPreviewResult {
    std::uint64_t generation{0};
    std::uint64_t sourceStamp{0};
    std::string path;
    std::shared_ptr<const EditorModelPreviewSnapshot> snapshot{};
    std::string error;
};

EditorModelPreviewResult load_editor_model_preview(
    const FileSystemService& files,
    std::string_view relativePath,
    std::uint64_t generation,
    std::uint64_t sourceStamp,
    const std::atomic_bool* cancel = nullptr);

// AssetSystem can supply the immutable typed source payload directly. The
// GLTF/GLB overload still resolves project-relative external buffers/images
// through FileSystemService, while the model container itself is not reopened.
EditorModelPreviewResult load_editor_obj_preview_bytes(
    std::string_view relativePath,
    const std::vector<std::uint8_t>& sourceBytes,
    std::uint64_t generation,
    std::uint64_t sourceStamp,
    const std::atomic_bool* cancel = nullptr);

EditorModelPreviewResult load_editor_gltf_preview_bytes(
    const FileSystemService& files,
    std::string_view relativePath,
    const std::vector<std::uint8_t>& sourceBytes,
    std::uint64_t generation,
    std::uint64_t sourceStamp,
    const std::atomic_bool* cancel = nullptr);

} // namespace shinkou::editor

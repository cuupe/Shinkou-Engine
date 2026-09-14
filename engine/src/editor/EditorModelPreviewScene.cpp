#include "shinkou/editor/EditorModelPreviewScene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace shinkou::editor {
namespace {

constexpr std::size_t kMaxProjectedSegments = 8192;
constexpr float kMinDistance = 0.75f;
constexpr float kMaxDistance = 8.0f;
constexpr float kMinPitch = -1.45f;
constexpr float kMaxPitch = 1.45f;

std::uint64_t mix(std::uint64_t value, std::uint64_t input) noexcept {
    value ^= input + 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
    return value;
}

std::uint64_t projection_revision(std::uint64_t geometryRevision,
                                  const EditorModelPreviewCameraState& camera) noexcept {
    std::uint32_t yawBits = 0;
    std::uint32_t pitchBits = 0;
    std::uint32_t distanceBits = 0;
    static_assert(sizeof(yawBits) == sizeof(camera.yaw));
    std::memcpy(&yawBits, &camera.yaw, sizeof(yawBits));
    std::memcpy(&pitchBits, &camera.pitch, sizeof(pitchBits));
    std::memcpy(&distanceBits, &camera.distance, sizeof(distanceBits));
    auto result = mix(0xcbf29ce484222325ull, geometryRevision);
    result = mix(result, yawBits);
    result = mix(result, pitchBits);
    result = mix(result, distanceBits);
    return result == 0 ? 1 : result;
}

} // namespace

bool EditorModelPreviewCameraState::valid() const noexcept {
    return std::isfinite(yaw) && std::isfinite(pitch) && std::isfinite(distance) &&
        pitch >= kMinPitch && pitch <= kMaxPitch &&
        distance >= kMinDistance && distance <= kMaxDistance;
}

void EditorModelPreviewScene::clear() noexcept {
    snapshot_.reset();
    wireSegments_.reset();
    camera_ = {};
    projectionRevision_ = 0;
}

void EditorModelPreviewScene::set_snapshot(
    std::shared_ptr<const EditorModelPreviewSnapshot> snapshot) noexcept {
    if (snapshot_ && snapshot && snapshot_->revision == snapshot->revision) return;
    snapshot_ = std::move(snapshot);
    camera_ = {};
    rebuild_projection();
}

void EditorModelPreviewScene::reset_camera() noexcept {
    camera_ = {};
    rebuild_projection();
}

void EditorModelPreviewScene::orbit(ui::Vec2 delta) noexcept {
    if (!snapshot_ || !snapshot_->valid() || !std::isfinite(delta.x) || !std::isfinite(delta.y)) return;
    camera_.yaw = math::Wrap(camera_.yaw + delta.x * 0.01f, -math::Pi, math::Pi);
    camera_.pitch = std::clamp(camera_.pitch + delta.y * 0.01f, kMinPitch, kMaxPitch);
    rebuild_projection();
}

void EditorModelPreviewScene::zoom(float wheelDelta) noexcept {
    if (!snapshot_ || !snapshot_->valid() || !std::isfinite(wheelDelta)) return;
    camera_.distance = std::clamp(camera_.distance * std::exp(-wheelDelta * 0.08f),
                                  kMinDistance, kMaxDistance);
    rebuild_projection();
}

void EditorModelPreviewScene::rebuild_projection() noexcept {
    if (!snapshot_ || !snapshot_->valid()) {
        wireSegments_.reset();
        projectionRevision_ = 0;
        return;
    }
    const auto& vertices = *snapshot_->vertices;
    const auto& indices = *snapshot_->indices;
    const math::Vec3 center{
        (snapshot_->minX + snapshot_->maxX) * 0.5f,
        (snapshot_->minY + snapshot_->maxY) * 0.5f,
        (snapshot_->minZ + snapshot_->maxZ) * 0.5f};
    const math::Vec3 extent{snapshot_->maxX - snapshot_->minX,
                            snapshot_->maxY - snapshot_->minY,
                            snapshot_->maxZ - snapshot_->minZ};
    const float radius = std::max(1.0e-6f, 0.5f * math::Length(extent));
    const auto rotation = math::FromAxisAngle({0.0f, 1.0f, 0.0f}, camera_.yaw) *
        math::FromAxisAngle({1.0f, 0.0f, 0.0f}, camera_.pitch);
    const float scale = std::clamp(1.15f / camera_.distance, 0.08f, 2.0f) / radius;
    auto projected = std::make_shared<std::vector<ui::Vec2>>();
    projected->reserve(std::min<std::size_t>(indices.size() * 2u, kMaxProjectedSegments * 2u));
    const auto project = [&](std::uint32_t index) {
        if (index >= vertices.size()) return ui::Vec2{0.5f, 0.5f};
        const auto view = math::Rotate(rotation, vertices[index] - center);
        return ui::Vec2{0.5f + view.x * scale, 0.5f - view.y * scale};
    };
    for (std::size_t index = 0; index + 2 < indices.size() &&
         projected->size() < kMaxProjectedSegments * 2u; index += 3) {
        const auto a = indices[index];
        const auto b = indices[index + 1];
        const auto c = indices[index + 2];
        projected->push_back(project(a)); projected->push_back(project(b));
        projected->push_back(project(b)); projected->push_back(project(c));
        projected->push_back(project(c)); projected->push_back(project(a));
    }
    wireSegments_ = std::move(projected);
    projectionRevision_ = projection_revision(snapshot_->revision, camera_);
}

std::shared_ptr<const EditorModelPreviewSceneState> EditorModelPreviewScene::state() const {
    if (!snapshot_ || !snapshot_->valid() || !wireSegments_ || projectionRevision_ == 0) return {};
    auto result = std::make_shared<EditorModelPreviewSceneState>();
    result->geometryRevision = snapshot_->revision;
    result->projectionRevision = projectionRevision_;
    result->camera = camera_;
    result->wireSegments = wireSegments_;
    return result;
}

} // namespace shinkou::editor

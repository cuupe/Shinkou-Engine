#pragma once

#include "shinkou/editor/EditorModelPreview.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace shinkou::editor {

struct EditorModelPreviewCameraState {
    float yaw{0.55f};
    float pitch{0.25f};
    float distance{3.0f};

    bool valid() const noexcept;
};

// Presentation state for the isolated model preview. It is intentionally
// separate from the asset geometry so orbiting never mutates the imported
// resource or the active World.
struct EditorModelPreviewSceneState {
    std::uint64_t geometryRevision{0};
    std::uint64_t projectionRevision{0};
    EditorModelPreviewCameraState camera{};
    std::shared_ptr<const std::vector<ui::Vec2>> wireSegments{};
    std::int32_t animationIndex{-1};
    float animationTime{0.0f};
    float animationDuration{0.0f};
    bool animationPlaying{false};
    bool animationPlayable{false};

    bool valid() const noexcept {
        return geometryRevision != 0 && projectionRevision != 0 && camera.valid() &&
            wireSegments && wireSegments->size() >= 2 && (wireSegments->size() % 2) == 0;
    }
};

// Renderer-neutral scene seam for model assets. The scene owns no World,
// entity, GPU handle, file path, or decoder. It only projects immutable mesh
// data into bounded retained geometry until a backend render target is added.
class EditorModelPreviewScene final {
    std::shared_ptr<const EditorModelPreviewSnapshot> snapshot_{};
    EditorModelPreviewCameraState camera_{};
    std::shared_ptr<const std::vector<ui::Vec2>> wireSegments_{};
    std::uint64_t projectionRevision_{0};
    animation::Animator animator_{};
    std::int32_t animationIndex_{-1};
    std::uint64_t animationRevision_{0};

    void rebuild_projection() noexcept;
    void clear_animation() noexcept;

public:
    void clear() noexcept;
    void set_snapshot(std::shared_ptr<const EditorModelPreviewSnapshot> snapshot) noexcept;
    void reset_camera() noexcept;
    void orbit(ui::Vec2 delta) noexcept;
    void zoom(float wheelDelta) noexcept;
    bool select_animation(std::int32_t index) noexcept;
    void play_animation() noexcept;
    void pause_animation() noexcept;
    void toggle_animation() noexcept;
    void seek_animation(float seconds) noexcept;
    void advance_animation(float deltaSeconds) noexcept;

    const std::shared_ptr<const EditorModelPreviewSnapshot>& snapshot() const noexcept { return snapshot_; }
    const EditorModelPreviewCameraState& camera() const noexcept { return camera_; }
    std::int32_t animation_index() const noexcept { return animationIndex_; }
    const animation::Animator& animator() const noexcept { return animator_; }
    std::shared_ptr<const EditorModelPreviewSceneState> state() const;
};

} // namespace shinkou::editor

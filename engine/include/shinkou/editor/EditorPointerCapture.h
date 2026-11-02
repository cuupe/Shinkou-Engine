#pragma once
#include "shinkou/Math.h"
#include <cstdint>
#include <memory>

namespace shinkou::editor {
struct EditorPointerBounds {
    long left{}, top{}, right{}, bottom{};
};
class IEditorPointerDevice {
  public:
    virtual ~IEditorPointerDevice() = default;
    virtual bool begin(std::uintptr_t window, EditorPointerBounds, math::Vec2 anchor) = 0;
    virtual bool sample(math::Vec2 &position) = 0;
    virtual bool warp(math::Vec2 position) = 0;
    virtual void release(bool restorePosition) noexcept = 0;
};
// Owns a relative gesture, including failure/focus-loss cleanup. The device is
// injectable so cursor ownership can be tested without moving a user's mouse.
class EditorPointerCapture {
    std::unique_ptr<IEditorPointerDevice> device_;
    math::Vec2 anchor_{};
    bool active_{false}, lostFocus_{false};

  public:
    EditorPointerCapture();
    explicit EditorPointerCapture(std::unique_ptr<IEditorPointerDevice>);
    ~EditorPointerCapture();
    bool begin(std::uintptr_t, EditorPointerBounds, math::Vec2 anchor);
    math::Vec2 delta(math::Vec2 eventPosition, math::Vec2 fallback);
    void end(bool restorePosition = true) noexcept;
    bool active() const noexcept {
        return active_;
    }
    bool lost_focus() const noexcept {
        return lostFocus_;
    }
    math::Vec2 anchor() const noexcept {
        return anchor_;
    }
    static math::Vec2 screen_point(std::uintptr_t, math::Vec2, bool globalCoordinates) noexcept;
    static math::Vec2 input_point(std::uintptr_t, math::Vec2, bool globalCoordinates) noexcept;
};
} // namespace shinkou::editor

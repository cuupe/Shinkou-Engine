#include "shinkou/editor/EditorPointerCapture.h"
#include <cmath>
#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <windows.h>
#endif
namespace shinkou::editor {
namespace {
class NativePointerDevice final : public IEditorPointerDevice {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    HWND window_{};
    RECT previousClip_{};
    POINT original_{};
    HCURSOR cursor_{};
    int hidden_{};
#endif
  public:
    bool begin(std::uintptr_t window, EditorPointerBounds bounds, math::Vec2 anchor) override {
#if defined(SHINKOU_PLATFORM_WINDOWS)
        const auto handle = reinterpret_cast<HWND>(window);
        POINT point{};
        if (!IsWindow(handle) || GetForegroundWindow() != handle || !GetCursorPos(&point) ||
            GetAncestor(WindowFromPoint(point), GA_ROOT) != handle ||
            std::abs(point.x - anchor.x) > 8 || std::abs(point.y - anchor.y) > 8 ||
            point.x < bounds.left || point.x >= bounds.right || point.y < bounds.top ||
            point.y >= bounds.bottom)
            return false;
        if (!GetClipCursor(&previousClip_))
            return false;
        const RECT clip{bounds.left, bounds.top, bounds.right, bounds.bottom};
        if (!ClipCursor(&clip))
            return false;
        window_ = handle;
        original_ = point;
        cursor_ = GetCursor();
        do {
            ++hidden_;
            if (ShowCursor(FALSE) < 0)
                break;
        } while (hidden_ < 32);
        SetCursor(nullptr);
        return true;
#else
        (void)window;
        (void)bounds;
        (void)anchor;
        return false;
#endif
    }
    bool sample(math::Vec2 &position) override {
#if defined(SHINKOU_PLATFORM_WINDOWS)
        POINT point{};
        if (!window_ || !IsWindow(window_) || GetForegroundWindow() != window_ ||
            !GetCursorPos(&point) || GetAncestor(WindowFromPoint(point), GA_ROOT) != window_)
            return false;
        position = {float(point.x), float(point.y)};
        return true;
#else
        (void)position;
        return false;
#endif
    }
    bool warp(math::Vec2 position) override {
#if defined(SHINKOU_PLATFORM_WINDOWS)
        POINT actual{};
        return SetCursorPos(std::lround(position.x), std::lround(position.y)) &&
               GetCursorPos(&actual) && std::abs(actual.x - position.x) < 1 &&
               std::abs(actual.y - position.y) < 1;
#else
        (void)position;
        return false;
#endif
    }
    void release(bool restorePosition) noexcept override {
#if defined(SHINKOU_PLATFORM_WINDOWS)
        if (!window_)
            return;
        ClipCursor(&previousClip_);
        while (hidden_ > 0) {
            ShowCursor(TRUE);
            --hidden_;
        }
        if (restorePosition && GetForegroundWindow() == window_)
            SetCursorPos(original_.x, original_.y);
        SetCursor(cursor_);
        window_ = nullptr;
#else
        (void)restorePosition;
#endif
    }
    ~NativePointerDevice() override {
        release(false);
    }
};
} // namespace
EditorPointerCapture::EditorPointerCapture() : device_(std::make_unique<NativePointerDevice>()) {
}
EditorPointerCapture::EditorPointerCapture(std::unique_ptr<IEditorPointerDevice> device)
    : device_(std::move(device)) {
}
EditorPointerCapture::~EditorPointerCapture() {
    end(false);
}
bool EditorPointerCapture::begin(std::uintptr_t window, EditorPointerBounds bounds,
                                 math::Vec2 anchor) {
    end(false);
    lostFocus_ = false;
    anchor_ = anchor;
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top || !math::IsFinite(anchor.x) ||
        !math::IsFinite(anchor.y))
        return false;
    active_ = device_ && device_->begin(window, bounds, anchor);
    if (active_ && !device_->sample(anchor_)) {
        end(false);
        lostFocus_ = true;
    }
    return active_;
}
math::Vec2 EditorPointerCapture::delta(math::Vec2 eventPosition, math::Vec2 fallback) {
    if (!active_)
        return fallback;
    math::Vec2 actual;
    if (!device_->sample(actual)) {
        end(false);
        lostFocus_ = true;
        return {};
    }
    if (std::abs(actual.x - eventPosition.x) > 8 || std::abs(actual.y - eventPosition.y) > 8) {
        end(false);
        return fallback;
    }
    const auto result = actual - anchor_;
    if (!device_->warp(anchor_))
        end(false);
    return result;
}
void EditorPointerCapture::end(bool restorePosition) noexcept {
    if (active_) {
        device_->release(restorePosition);
        active_ = false;
    }
    lostFocus_ = false;
}
math::Vec2 EditorPointerCapture::screen_point(std::uintptr_t window, math::Vec2 point,
                                              bool globalCoordinates) noexcept {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    if (!globalCoordinates) {
        POINT p{std::lround(point.x), std::lround(point.y)};
        ClientToScreen(reinterpret_cast<HWND>(window), &p);
        return {float(p.x), float(p.y)};
    }
#else
    (void)window;
    (void)globalCoordinates;
#endif
    return point;
}
math::Vec2 EditorPointerCapture::input_point(std::uintptr_t window, math::Vec2 point,
                                             bool globalCoordinates) noexcept {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    if (!globalCoordinates) {
        POINT p{std::lround(point.x), std::lround(point.y)};
        ScreenToClient(reinterpret_cast<HWND>(window), &p);
        return {float(p.x), float(p.y)};
    }
#else
    (void)window;
    (void)globalCoordinates;
#endif
    return point;
}
} // namespace shinkou::editor

#pragma once

#include "Widgets.h"
#include <cstddef>

namespace shinkou::uikit {

struct VisibleRange { std::size_t first = 0; std::size_t last = 0; };
VisibleRange visible_range(std::size_t itemCount, float itemExtent, float offset, float viewportExtent, std::size_t overscan = 2);

class UiContext {
public:
    UiContext();
    Panel& root() { return m_root; }
    const Panel& root() const { return m_root; }
    StyleSheet& style() { return m_style; }
    const StyleSheet& style() const { return m_style; }
    AnimationTimeline& animations() { return m_animations; }

    void set_viewport(Size viewport, float dpiScale = 1.0f);
    Size viewport() const { return m_viewport; }
    float dpi_scale() const { return m_dpiScale; }
    void invalidate_layout() { m_layoutDirty = true; }
    void layout();
    void tick(float deltaSeconds);
    bool dispatch(UiEvent event);
    Widget* hit_test(Vec2 position);
    void paint(RenderList& render);
    std::size_t frame_number() const { return m_frame; }

private:
    Widget* hit_test_recursive(Widget& widget, Vec2 position);
    Panel m_root;
    StyleSheet m_style;
    AnimationTimeline m_animations;
    Size m_viewport{};
    float m_dpiScale = 1.0f;
    bool m_layoutDirty = true;
    std::size_t m_frame = 0;
    WidgetId m_pointerCapture = 0;
};

} // namespace shinkou::uikit


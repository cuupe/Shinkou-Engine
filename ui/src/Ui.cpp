#include "shinkou/uikit/Ui.h"
#include <algorithm>
#include <cmath>

namespace shinkou::uikit {

VisibleRange visible_range(std::size_t itemCount, float itemExtent, float offset, float viewportExtent, std::size_t overscan) {
    if (itemCount == 0 || itemExtent <= 0.0f) return {};
    const float safeOffset = std::max(0.0f, offset);
    const std::size_t first = std::min(itemCount, static_cast<std::size_t>(std::floor(safeOffset / itemExtent)));
    const std::size_t visible = static_cast<std::size_t>(std::ceil(std::max(0.0f, viewportExtent) / itemExtent));
    return {first > overscan ? first - overscan : 0, std::min(itemCount, first + visible + overscan)};
}

UiContext::UiContext() : m_style(StyleSheet::make_windows11_light()) {
    m_root.layout = LayoutMode::Vertical;
    m_root.styleClass = "panel";
}

void UiContext::set_viewport(Size viewportValue, float dpiScaleValue) {
    if (m_viewport.width != viewportValue.width || m_viewport.height != viewportValue.height || m_dpiScale != dpiScaleValue) m_layoutDirty = true;
    m_viewport = viewportValue;
    m_dpiScale = std::max(0.25f, dpiScaleValue);
}

void UiContext::layout() {
    if (!m_layoutDirty) return;
    m_root.arrange({0.0f, 0.0f, m_viewport.width, m_viewport.height});
    m_layoutDirty = false;
}

void UiContext::tick(float deltaSeconds) {
    layout();
    m_animations.update(deltaSeconds, {m_style.reduceMotion});
    for (const auto& child : m_root.children()) {
        if (auto* media = dynamic_cast<Video*>(child.get())) if (media->state == MediaPlaybackState::Playing) media->positionSeconds += std::max(0.0f, deltaSeconds);
        if (auto* audio = dynamic_cast<Audio*>(child.get())) if (audio->state == MediaPlaybackState::Playing) audio->positionSeconds += std::max(0.0f, deltaSeconds);
    }
    ++m_frame;
}

Widget* UiContext::hit_test_recursive(Widget& widget, Vec2 position) {
    if (!widget.visible || !widget.enabled || !widget.hitTestVisible || !widget.bounds().contains(position)) return nullptr;
    for (auto iterator = widget.children().rbegin(); iterator != widget.children().rend(); ++iterator) if (Widget* found = hit_test_recursive(**iterator, position)) return found;
    return &widget;
}
Widget* UiContext::hit_test(Vec2 position) { return hit_test_recursive(m_root, position); }

bool UiContext::dispatch(UiEvent event) {
    Widget* target = m_pointerCapture ? m_root.find(m_pointerCapture) : hit_test(event.position);
    if (!target) return false;
    event.target = target->id();
    if (event.type == UiEventType::PointerDown) m_pointerCapture = target->id();
    if (event.type == UiEventType::PointerUp) m_pointerCapture = 0;
    if (event.type == UiEventType::PointerDown && target->focusable) {
        std::function<void(Widget&)> clearFocus = [&](Widget& widget) { widget.focused = false; for (const auto& child : widget.children()) clearFocus(*child); };
        clearFocus(m_root);
        target->focused = true;
    }

    std::vector<Widget*> route;
    for (Widget* item = target; item; item = item->parent()) route.push_back(item);
    bool handled = false;
    for (std::size_t index = route.size(); index-- > 1;) {
        event.phase = UiEventPhase::Capture;
        event.currentTarget = route[index - 1]->id();
        route[index - 1]->on_event(event);
        handled = handled || event.handled;
        if (event.stopPropagation) return handled;
    }
    event.phase = UiEventPhase::Target;
    event.currentTarget = target->id();
    target->on_event(event);
    handled = handled || event.handled;
    if (event.stopPropagation) return handled;
    for (std::size_t index = 1; index < route.size(); ++index) {
        event.phase = UiEventPhase::Bubble;
        event.currentTarget = route[index]->id();
        route[index]->on_event(event);
        handled = handled || event.handled;
        if (event.stopPropagation) break;
    }
    return handled;
}

void UiContext::paint(RenderList& render) {
    layout();
    render.reserve(128);
    m_root.paint(render, m_style);
}

} // namespace shinkou::uikit

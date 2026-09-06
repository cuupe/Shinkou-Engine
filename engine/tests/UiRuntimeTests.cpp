#include "shinkou/ui/Ui.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

bool near(float left, float right) {
    return std::abs(left - right) < 0.001f;
}

} // namespace

int main() {
    using namespace shinkou::ui;

    UiRuntime ui;
    LayoutStyle row;
    row.layout = Layout::FlexRow;
    row.padding = Insets(10.0f, 5.0f);
    const WidgetId panel = ui.create_widget(InvalidWidgetId, row);

    LayoutStyle fixed;
    fixed.size = {100.0f, 20.0f};
    fixed.margin = Insets(2.0f);
    const WidgetId first = ui.create_widget(panel, fixed);

    LayoutStyle flexible;
    flexible.flex = 1.0f;
    flexible.minSize = {40.0f, 10.0f};
    flexible.margin = Insets(3.0f);
    const WidgetId second = ui.create_widget(panel, flexible);

    LayoutStyle overlay;
    overlay.layout = Layout::Overlay;
    overlay.padding = Insets(4.0f);
    const WidgetId overlayId = ui.create_widget(panel, overlay);
    LayoutStyle overlayChild;
    overlayChild.size = {30.0f, 12.0f};
    const WidgetId over = ui.create_widget(overlayId, overlayChild);

    assert(ui.is_subtree_dirty(ui.root()));
    ui.begin_frame();
    ui.layout({0.0f, 0.0f, 400.0f, 100.0f});
    assert(ui.frame_stats().layoutRebuilt);
    assert(ui.frame_stats().widgetCount == 6);
    assert(near(ui.widget(panel)->rect.width, 400.0f));
    assert(near(ui.widget(first)->rect.x, 12.0f));
    assert(near(ui.widget(first)->rect.y, 7.0f));
    assert(near(ui.widget(first)->rect.width, 100.0f));
    assert(near(ui.widget(second)->rect.x, 117.0f));
    assert(near(ui.widget(second)->rect.width, 232.0f));
    assert(near(ui.widget(over)->rect.width, 30.0f));
    assert(!ui.is_subtree_dirty(ui.root()));

    ui.begin_frame();
    ui.layout({400.0f, 100.0f});
    assert(!ui.frame_stats().layoutRebuilt);
    assert(ui.frame_stats().layoutNodeCount == 0);

    ui.set_padding(panel, Insets(20.0f, 5.0f));
    assert(ui.is_dirty(panel));
    assert(ui.is_subtree_dirty(ui.root()));
    ui.begin_frame();
    ui.layout({400.0f, 100.0f});
    assert(ui.frame_stats().layoutRebuilt);
    assert(near(ui.widget(first)->rect.x, 22.0f));

    std::vector<UiEventType> events;
    LayoutStyle interactive;
    interactive.size = {100.0f, 30.0f};
    interactive.focusable = true;
    const WidgetId button = ui.create_widget(panel, interactive);
    ui.set_event_handler(button, [&](WidgetId id, UiEvent& event) {
        assert(id == button);
        events.push_back(event.type);
        if (event.type == UiEventType::PointerDown) {
            assert(ui.capture_pointer(id));
            return EventResult::Handled;
        }
        return EventResult::Continue;
    });
    ui.set_event_handler(panel, [&](WidgetId id, UiEvent& event) {
        assert(id == panel);
        events.push_back(event.type);
        return EventResult::Continue;
    });

    ui.begin_frame();
    ui.layout({400.0f, 100.0f});
    const Rect buttonRect = ui.widget(button)->rect;
    assert(ui.hit_test({buttonRect.x + 1.0f, buttonRect.y + 1.0f}) == button);

    UiEvent down;
    down.type = UiEventType::PointerDown;
    down.position = {buttonRect.x + 1.0f, buttonRect.y + 1.0f};
    assert(ui.dispatch(down));
    assert(ui.captured_pointer() == button);
    assert(ui.focused() == button);

    UiEvent move;
    move.type = UiEventType::PointerMove;
    move.position = {1.0f, 1.0f};
    assert(!ui.dispatch(move));
    assert(move.target == button);

    UiEvent up;
    up.type = UiEventType::PointerUp;
    up.position = {1.0f, 1.0f};
    ui.dispatch(up);
    assert(ui.captured_pointer() == InvalidWidgetId);
    assert(events.size() >= 3);
    assert(ui.frame_stats().hitTestCount >= 1);
    assert(ui.frame_stats().dispatchedEventCount >= 3);

    UiEvent focusAway;
    focusAway.type = UiEventType::PointerDown;
    focusAway.position = {ui.widget(panel)->rect.x + 1.0f, ui.widget(panel)->rect.y + 1.0f};
    ui.dispatch(focusAway);
    assert(ui.focused() == InvalidWidgetId);

    assert(ui.destroy_widget(overlayId));
    assert(ui.widget(over) == nullptr);
    assert(ui.widget(overlayId) == nullptr);
    assert(!ui.destroy_widget(ui.root()));

    std::cout << "UI runtime layout, hit-test, routing, focus/capture and dirty stats passed\n";
    return 0;
}

#include "shinkou/ui/Components.h"

#include <cassert>
#include <memory>
#include <string>

using namespace shinkou::ui;

namespace {

void test_button_and_state() {
    Button button{"play", "播放"};
    button.set_bounds(Rect{10.0f, 20.0f, 120.0f, 32.0f});
    button.set_toggleable(true);
    bool clicked = false;
    button.set_event_callback([&](Component& sender, ComponentEvent& event) {
        clicked = sender.id() == "play" && event.type == ComponentEventType::Click && event.text == "播放";
        return EventResult::Handled;
    });
    assert(button.click());
    assert(clicked && button.checked() && button.state().checked);
    button.set_enabled(false);
    assert(!button.click() && button.checked());
    button.set_enabled(true);
    ComponentEvent enter{ComponentEventType::PointerEnter};
    button.dispatch(enter);
    assert(button.state().hovered);
}

void test_slider() {
    Slider slider{"volume", 0.25f};
    assert(slider.set_range(-1.0f, 1.0f));
    assert(slider.set_step(0.25f));
    int changes = 0;
    slider.set_event_callback([&](Component&, ComponentEvent& event) {
        assert(event.type == ComponentEventType::ValueChanged);
        ++changes;
        return EventResult::Handled;
    });
    assert(slider.set_value(0.62f));
    assert(slider.value() == 0.5f && slider.normalized_value() == 0.75f && changes == 1);
    assert(slider.set_value(5.0f));
    assert(slider.value() == 1.0f);
    assert(!slider.set_range(1.0f, 1.0f));
}

void test_text_and_rich_text() {
    TextBox text{"name", "abc"};
    text.set_max_length(5);
    int changes = 0;
    text.set_event_callback([&](Component&, ComponentEvent& event) {
        if (event.type == ComponentEventType::TextChanged) ++changes;
        return EventResult::Handled;
    });
    text.set_selection(1, 2);
    assert(text.insert_text("XYZ"));
    assert(text.text() == "aXYZc" && text.selection_start() == 4 && text.selection_end() == 4);
    text.set_selection(1, 4);
    assert(text.erase_selection());
    assert(text.text() == "ac" && changes == 2);
    text.set_read_only(true);
    assert(!text.insert_text("blocked"));

    RichTextBox rich{"description"};
    RichTextStyle emphasis;
    emphasis.bold = true;
    emphasis.color = ThemeColor{0.9f, 0.7f, 0.1f, 1.0f};
    assert(rich.append_run(RichTextRun{"Hello ", {}}));
    assert(rich.append_run(RichTextRun{"world", emphasis}));
    assert(rich.plain_text() == "Hello world");
    assert(rich.set_plain_text("重置", emphasis));
    assert(rich.runs().size() == 1 && rich.plain_text() == "重置");
}

void test_panel_document_and_json() {
    ComponentDocument document;
    auto* panel = document.emplace<Panel>("root", "属性");
    assert(panel);
    panel->set_collapsible(true);
    assert(panel->set_collapsed(true));
    panel->set_padding(Insets{4.0f, 8.0f, 4.0f, 8.0f});
    auto* slider = document.emplace<Slider>("opacity", 0.5f);
    auto* box = document.emplace<TextBox>("label", "Shinkou");
    assert(slider && box && panel->add_child("opacity") && panel->add_child("label"));
    assert(!panel->add_child("opacity"));
    assert(document.valid());

    const std::string json = serialize_json(document, false);
    assert(json.find("shinkou.ui-components") != std::string::npos);
    assert(json.find("richTextBox") == std::string::npos);

    ComponentDocument restored;
    std::string error;
    assert(deserialize_json(json, restored, &error));
    assert(restored.size() == 3);
    const auto* restoredPanel = dynamic_cast<const Panel*>(restored.find("root"));
    assert(restoredPanel && restoredPanel->collapsed() && restoredPanel->child_ids().size() == 2);
    const auto* restoredBox = dynamic_cast<const TextBox*>(restored.find("label"));
    assert(restoredBox && restoredBox->text() == "Shinkou");

    std::unique_ptr<Component> single;
    assert(deserialize_json(serialize_json(*slider, false), single, &error));
    assert(dynamic_cast<Slider*>(single.get()) != nullptr);
    const std::string before = serialize_json(*single, false);
    assert(!deserialize_json("{\"schema\":\"shinkou.ui-components\",\"version\":1,\"component\":{\"type\":\"slider\",\"id\":\"bad\",\"minimum\":2,\"maximum\":1}}", single, &error));
    assert(serialize_json(*single, false) == before);

    const std::size_t sizeBefore = restored.size();
    assert(!deserialize_json("{\"schema\":\"shinkou.ui-components\",\"version\":1,\"components\":[{\"type\":\"button\",\"id\":\"same\"},{\"type\":\"button\",\"id\":\"same\"}]}", restored, &error));
    assert(restored.size() == sizeBefore);
}

} // namespace

int main() {
    test_button_and_state();
    test_slider();
    test_text_and_rich_text();
    test_panel_document_and_json();
    return 0;
}

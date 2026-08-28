#include "shinkou/uikit/Ui.h"
#include <cassert>

using namespace shinkou::uikit;

int main() {
    UiContext ui;
    ui.set_viewport({800, 600});
    ui.root().layout = LayoutMode::Horizontal;
    ui.root().gap = 8.0f;
    Button& button = ui.root().emplace<Button>("确认");
    Slider& slider = ui.root().emplace<Slider>();
    TextBox& input = ui.root().emplace<TextBox>();
    button.flex = 1.0f; slider.flex = 1.0f; input.flex = 1.0f;
    bool clicked = false;
    button.clicked = [&clicked] { clicked = true; };
    ui.tick(0.016f);
    assert(button.bounds().width > 0.0f && slider.bounds().width > 0.0f);
    UiEvent down; down.type = UiEventType::PointerDown; down.position = {button.bounds().x + 2, button.bounds().y + 2}; down.button = PointerButton::Left;
    UiEvent up = down; up.type = UiEventType::PointerUp;
    assert(ui.dispatch(down)); assert(ui.dispatch(up)); assert(clicked);
    UiEvent text; text.type = UiEventType::TextInput; text.text = "A"; input.focused = true;
    assert(input.on_event(text) && input.text == "A");
    return 0;
}


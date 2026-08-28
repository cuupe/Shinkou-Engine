#include "shinkou/uikit/Editor.h"
#include <cassert>
#include <string>

using namespace shinkou::uikit;

int main() {
    EditorDocument document;
    document.ui().set_viewport({640, 360});
    const WidgetId buttonId = document.add("button");
    assert(buttonId != 0);
    assert(document.select(buttonId));
    assert(document.set_property(buttonId, "label", std::string("保存")));
    assert(document.set_property(buttonId, "name", std::string("SaveButton")));
    assert(document.set_property(buttonId, "variant", std::string("primary")));
    assert(document.selected()->styleVariant == "primary");
    const WidgetId copyId = document.duplicate(buttonId);
    assert(copyId != 0 && copyId != buttonId);
    assert(document.hierarchy().size() == 2);
    assert(document.undo() && document.hierarchy().size() == 1);
    assert(document.redo() && document.hierarchy().size() == 2);
    assert(document.set_style_property(".button", "hover", "box-shadow", "0 8px 24px #00000022"));
    const std::string xml = document.serialize_xml();
    assert(xml.find("SaveButton") != std::string::npos);
    assert(xml.find("variant=\"primary\"") != std::string::npos);
    assert(document.load_xml(xml));
    assert(document.hierarchy().size() == 2);
    assert(document.hierarchy().front().styleVariant == "primary" || document.hierarchy().back().styleVariant == "primary");

    ComboBox combo;
    combo.items = {"设计", "预览", "资源"};
    combo.arrange({0, 0, 300, 32});
    UiEvent down; down.type = UiEventType::PointerDown; down.position = {10, 10}; down.button = PointerButton::Left;
    UiEvent up = down; up.type = UiEventType::PointerUp;
    assert(combo.on_event(down));
    assert(combo.on_event(up));
    assert(combo.open == false);
    return 0;
}

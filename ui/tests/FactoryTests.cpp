#include "shinkou/uikit/Factory.h"
#include <cassert>

using namespace shinkou::uikit;

int main() {
    WidgetRegistry registry;
    std::string error;
    const std::string source = "<ui><panel layout=\"horizontal\" padding=\"8,4\"><button label=\"应用\" style=\"primary-button\"/><slider min=\"0\" max=\"10\" step=\"0.5\" value=\"4.5\"/><textbox text=\"hello\"/></panel></ui>";
    std::unique_ptr<Widget> root = registry.create_document(source, &error);
    assert(root && error.empty());
    assert(root->children().size() == 3);
    assert(dynamic_cast<Button*>(root->children()[0].get())->label == "应用");
    assert(dynamic_cast<Slider*>(root->children()[1].get())->value == 4.5f);
    assert(dynamic_cast<TextBox*>(root->children()[2].get())->text == "hello");
    const XmlNode serialized = registry.serialize(*root);
    assert(serialized.name == "panel" && serialized.children.size() == 3);
    XmlDocument xml;
    xml.set_root(serialized);
    const std::string roundTrip = xml.serialize();
    assert(roundTrip.find("primary-button") != std::string::npos);
    registry.register_type("custom", [](const XmlNode&) { return std::make_unique<Panel>(); });
    assert(registry.has_type("custom") && registry.unregister_type("custom") && !registry.has_type("custom"));
    return 0;
}


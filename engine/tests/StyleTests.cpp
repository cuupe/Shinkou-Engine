#include "shinkou/ui/Style.h"

#include <cassert>
#include <iostream>

int main() {
    shinkou::ui::UiStyleConfig source;
    source.activeTheme = "light";
    source.uiScale = 1.25f;
    source.fontPath = "Fonts/Editor.ttf";
    source.background.mode = shinkou::ui::BackgroundMode::Gradient;
    source.background.primary = {0.1f, 0.2f, 0.3f, 1.0f};
    source.background.secondary = {0.4f, 0.5f, 0.6f, 1.0f};
    const auto json = shinkou::ui::serialize_json(source);
    shinkou::ui::UiStyleConfig jsonRestored;
    assert(shinkou::ui::deserialize_json(json, jsonRestored));
    assert(jsonRestored.activeTheme == source.activeTheme && jsonRestored.fontPath == source.fontPath);
    assert(jsonRestored.background.mode == shinkou::ui::BackgroundMode::Gradient);
    const auto xml = shinkou::ui::serialize_xml(source);
    shinkou::ui::UiStyleConfig xmlRestored;
    assert(shinkou::ui::deserialize_xml(xml, xmlRestored));
    assert(xmlRestored.uiScale == source.uiScale && xmlRestored.background.primary == source.background.primary);
    std::cout << "UI style JSON/XML round trip passed\n";
}

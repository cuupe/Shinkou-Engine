#include "shinkou/uikit/Style.h"
#include <cassert>

using namespace shinkou::uikit;

int main() {
    const StyleSheet defaults = StyleSheet::make_windows11_light();
    assert(defaults.valid());
    assert(defaults.font.family == "Microsoft YaHei");
    assert(defaults.control("panel").radii.topLeft == 0.0f);
    const std::string xml = "<ui-styles version=\"1\"><theme id=\"custom\" scale=\"1.25\"><font family=\"Microsoft YaHei\" size=\"16\"/><palette><color name=\"window\" value=\"#102030\"/><color name=\"surface\" value=\"#203040\"/></palette><metrics><metric name=\"space-sm\" value=\"10\"/></metrics><controls><control type=\"button\" background=\"surface\" radius=\"6\" transition=\"200\"/></controls></theme></ui-styles>";
    std::string error;
    StyleSheet custom = StyleSheet::from_xml(xml, &error);
    assert(custom.valid());
    assert(custom.activeTheme == "custom");
    assert(custom.uiScale == 1.25f);
    assert(custom.font.size == 16.0f);
    assert(custom.metric("space-sm") == 10.0f);
    assert(custom.control("button").radii.topLeft == 6.0f);
    custom.set_property(".button", "hover", "box-shadow", "0 8px 24px #00000022");
    assert(custom.property("button", "hover", "box-shadow") == "0 8px 24px #00000022");
    assert(custom.resolved_font_path("C:/Windows/Fonts").find("Microsoft YaHei") != std::string::npos);
    const std::string serialized = custom.to_xml_string();
    assert(serialized.find("#203040ff") != std::string::npos);
    assert(serialized.find("box-shadow") != std::string::npos);
    const StyleSheet roundTrip = StyleSheet::from_xml(serialized, &error);
    assert(roundTrip.valid() && roundTrip.control("button").radii.topLeft == 6.0f);
    assert(roundTrip.property("button", "hover", "box-shadow") == "0 8px 24px #00000022");
    return 0;
}

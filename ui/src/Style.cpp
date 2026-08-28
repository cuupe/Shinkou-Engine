#include "shinkou/uikit/Style.h"
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace shinkou::uikit {
namespace {

float number(const std::string& value, float fallback = 0.0f) {
    if (value.empty()) return fallback;
    char* end = nullptr;
    const float result = std::strtof(value.c_str(), &end);
    return end == value.c_str() ? fallback : result;
}
int integer(const std::string& value, int fallback = 0) {
    if (value.empty()) return fallback;
    char* end = nullptr;
    const long result = std::strtol(value.c_str(), &end, 10);
    return end == value.c_str() ? fallback : static_cast<int>(result);
}
bool boolean(const std::string& value, bool fallback = false) {
    return value.empty() ? fallback : value == "true" || value == "1" || value == "yes";
}
Color resolve(const std::string& value, const std::map<std::string, Color>& palette, Color fallback = {}) {
    if (value.empty()) return fallback;
    const auto found = palette.find(value);
    return found == palette.end() ? Color::from_hex(value) : found->second;
}
std::string hex_color(Color value) {
    std::ostringstream result;
    result << '#' << std::hex << std::setw(8) << std::setfill('0') << value.to_rgba8();
    return result.str();
}
void set_default_metrics(StyleSheet& style) {
    style.metrics["space-xs"] = 4.0f;
    style.metrics["space-sm"] = 8.0f;
    style.metrics["space-md"] = 12.0f;
    style.metrics["space-lg"] = 16.0f;
    style.metrics["control-height"] = 32.0f;
    style.metrics["titlebar-height"] = 40.0f;
}
ControlStyle make_control(Color background, Color hover, Color pressed, Color border, Color text, float radius, float width = 1.0f) {
    ControlStyle result;
    result.background = background; result.hover = hover; result.pressed = pressed;
    result.border = border; result.text = text; result.textSecondary = text;
    result.focus = Color{0.0f, 0.47f, 0.82f, 1.0f}; result.borderWidth = width;
    result.padding = Insets(12.0f, 6.0f);
    result.radii = {radius, radius, radius, radius};
    return result;
}

} // namespace

Color Color::from_rgba8(std::uint32_t value) {
    return {((value >> 24) & 0xff) / 255.0f, ((value >> 16) & 0xff) / 255.0f, ((value >> 8) & 0xff) / 255.0f, (value & 0xff) / 255.0f};
}

Color Color::from_hex(const std::string& value) {
    std::string hex = value;
    if (!hex.empty() && hex.front() == '#') hex.erase(hex.begin());
    if (hex.size() == 3 || hex.size() == 4) {
        std::string expanded;
        for (char item : hex) expanded += std::string(2, item);
        hex = std::move(expanded);
    }
    if (hex.size() != 6 && hex.size() != 8) return {};
    char* end = nullptr;
    const auto parsed = std::strtoul(hex.c_str(), &end, 16);
    if (end == hex.c_str() || *end != '\0') return {};
    if (hex.size() == 6) return from_rgba8((static_cast<std::uint32_t>(parsed) << 8) | 0xffu);
    return from_rgba8(static_cast<std::uint32_t>(parsed));
}

std::uint32_t Color::to_rgba8() const {
    const auto channel = [](float value) { return static_cast<std::uint32_t>(clamp01(value) * 255.0f + 0.5f); };
    return (channel(r) << 24) | (channel(g) << 16) | (channel(b) << 8) | channel(a);
}

bool Color::operator==(const Color& other) const { return to_rgba8() == other.to_rgba8(); }

StyleSheet StyleSheet::make_windows11_light() {
    StyleSheet style;
    style.activeTheme = "windows11";
    style.windowBackground = Color::from_hex("#f3f4f6");
    style.surface = Color::from_hex("#ffffff");
    style.surfaceElevated = Color::from_hex("#ffffff");
    style.palette = {
        {"window", style.windowBackground}, {"surface", style.surface}, {"surface-elevated", style.surfaceElevated},
        {"border", Color::from_hex("#d1d5db")}, {"text", Color::from_hex("#1f2937")},
        {"text-secondary", Color::from_hex("#64748b")}, {"accent", Color::from_hex("#2563eb")},
        {"accent-hover", Color::from_hex("#1d4ed8")}, {"accent-active", Color::from_hex("#1e40af")},
        {"focus", Color::from_hex("#60a5fa")}
    };
    set_default_metrics(style);
    style.controls["button"] = make_control(style.palette["surface"], Color::from_hex("#f8fafc"), Color::from_hex("#e2e8f0"), style.palette["border"], style.palette["text"], 4.0f);
    style.controls["primary-button"] = make_control(style.palette["accent"], style.palette["accent-hover"], style.palette["accent-active"], style.palette["accent"], Color::from_hex("#ffffff"), 4.0f, 0.0f);
    style.controls["input"] = make_control(style.surface, Color::from_hex("#ffffff"), Color::from_hex("#ffffff"), style.palette["border"], style.palette["text"], 3.0f);
    style.controls["panel"] = make_control(style.surface, style.surface, style.surface, style.palette["border"], style.palette["text"], 0.0f, 1.0f);
    style.controls["label"] = make_control({}, {}, {}, {}, style.palette["text"], 0.0f, 0.0f);
    style.controls["label"].textSecondary = style.palette["text-secondary"];
    style.controls["tab"] = make_control(style.windowBackground, Color::from_hex("#e5e7eb"), Color::from_hex("#dbeafe"), style.windowBackground, style.palette["text"], 0.0f, 0.0f);
    style.controlVariants["button"]["primary"] = style.controls["primary-button"];
    style.controlVariants["button"]["destructive"] = make_control(Color::from_hex("#D84A4A"), Color::from_hex("#E15C5C"), Color::from_hex("#B93A3A"), Color::from_hex("#D84A4A"), Color::from_hex("#FFFFFF"), 6.0f, 0.0f);
    style.controlVariants["button"]["subtle"] = make_control(style.windowBackground, Color::from_hex("#e5e7eb"), Color::from_hex("#dbeafe"), style.windowBackground, style.palette["text"], 6.0f, 0.0f);
    style.controlVariants["input"]["compact"] = make_control(style.surface, Color::from_hex("#ffffff"), Color::from_hex("#ffffff"), style.palette["border"], style.palette["text"], 6.0f, 1.0f);
    return style;
}

StyleSheet StyleSheet::make_windows11_dark() {
    StyleSheet style = make_windows11_light();
    style.activeTheme = "windows11-dark";
    style.windowBackground = Color::from_hex("#202020");
    style.surface = Color::from_hex("#2b2b2b");
    style.surfaceElevated = Color::from_hex("#333333");
    style.palette["window"] = style.windowBackground;
    style.palette["surface"] = style.surface;
    style.palette["surface-elevated"] = style.surfaceElevated;
    style.palette["border"] = Color::from_hex("#454545");
    style.palette["text"] = Color::from_hex("#f3f4f6");
    style.palette["text-secondary"] = Color::from_hex("#a1a1aa");
    style.controls["button"] = make_control(style.surface, style.surfaceElevated, Color::from_hex("#404040"), style.palette["border"], style.palette["text"], 4.0f);
    style.controls["input"] = make_control(style.surface, style.surface, style.surface, style.palette["border"], style.palette["text"], 3.0f);
    style.controls["label"].text = style.palette["text"];
    style.controls["label"].textSecondary = style.palette["text-secondary"];
    return style;
}

StyleSheet StyleSheet::make_apple_light() {
    StyleSheet style = make_windows11_light();
    style.activeTheme = "apple-light";
    style.controls["button"].radii = {6, 6, 6, 6};
    style.controls["button"].padding = Insets(14, 7);
    style.controls["panel"].radii = {10, 10, 10, 10};
    style.controls["panel"].borderWidth = 0.0f;
    return style;
}

bool StyleSheet::load_xml(const XmlNode& document, std::string* error) {
    const XmlNode* theme = document.name == "theme" ? &document : document.child("theme");
    if (!theme) { if (error) *error = "style XML has no theme element"; return false; }
    schema = document.attribute("schema", schema);
    version = integer(document.attribute("version"), version);
    activeTheme = theme->attribute("id", activeTheme);
    uiScale = number(theme->attribute("scale"), uiScale);
    reduceMotion = boolean(theme->attribute("reduce-motion"), reduceMotion);
    if (const XmlNode* fontNode = theme->child("font")) {
        font.family = fontNode->attribute("family", font.family);
        font.path = fontNode->attribute("path", font.path);
        font.size = number(fontNode->attribute("size"), font.size);
        font.weight = integer(fontNode->attribute("weight"), font.weight);
        font.italic = boolean(fontNode->attribute("italic"), font.italic);
    }
    if (const XmlNode* paletteNode = theme->child("palette")) {
        for (const XmlNode* colorNode : paletteNode->children_named("color")) {
            const std::string name = colorNode->attribute("name");
            if (!name.empty()) palette[name] = Color::from_hex(colorNode->attribute("value"));
        }
    }
    if (const XmlNode* metricsNode = theme->child("metrics")) {
        for (const XmlNode* metricNode : metricsNode->children_named("metric")) {
            const std::string name = metricNode->attribute("name");
            if (!name.empty()) metrics[name] = number(metricNode->attribute("value"));
        }
    }
    if (const XmlNode* controlsNode = theme->child("controls")) {
        for (const XmlNode* controlNode : controlsNode->children_named("control")) {
            const std::string type = controlNode->attribute("type");
            const std::string variant = controlNode->attribute("variant", "default");
            if (type.empty()) continue;
            ControlStyle value = make_control({}, {}, {}, {}, {}, 0.0f);
            value.background = resolve(controlNode->attribute("background"), palette, value.background);
            value.hover = resolve(controlNode->attribute("hover"), palette, value.hover);
            value.pressed = resolve(controlNode->attribute("pressed"), palette, value.pressed);
            value.border = resolve(controlNode->attribute("border"), palette, value.border);
            value.text = resolve(controlNode->attribute("text"), palette, value.text);
            value.textSecondary = resolve(controlNode->attribute("text-secondary"), palette, value.text);
            value.focus = resolve(controlNode->attribute("focus"), palette, value.focus);
            value.borderWidth = number(controlNode->attribute("border-width"), value.borderWidth);
            value.padding.left = number(controlNode->attribute("padding-left"), value.padding.left);
            value.padding.top = number(controlNode->attribute("padding-top"), value.padding.top);
            value.padding.right = number(controlNode->attribute("padding-right"), value.padding.right);
            value.padding.bottom = number(controlNode->attribute("padding-bottom"), value.padding.bottom);
            const float radius = number(controlNode->attribute("radius"), value.radii.topLeft);
            value.radii.topLeft = number(controlNode->attribute("radius-top-left"), radius);
            value.radii.topRight = number(controlNode->attribute("radius-top-right"), radius);
            value.radii.bottomRight = number(controlNode->attribute("radius-bottom-right"), radius);
            value.radii.bottomLeft = number(controlNode->attribute("radius-bottom-left"), radius);
            value.transition.durationMs = number(controlNode->attribute("transition"), value.transition.durationMs);
            value.transition.easing = controlNode->attribute("easing", value.transition.easing);
            if (variant == "default") controls[type] = value;
            else controlVariants[type][variant] = value;
        }
    }
    if (const XmlNode* rulesNode = theme->child("rules")) {
        for (const XmlNode* ruleNode : rulesNode->children_named("rule")) {
            StyleRule rule;
            rule.selector = ruleNode->attribute("selector");
            rule.state = ruleNode->attribute("state", "normal");
            if (rule.selector.empty()) continue;
            for (const XmlNode* propertyNode : ruleNode->children_named("property")) {
                const std::string name = propertyNode->attribute("name");
                if (!name.empty()) rule.properties[name] = propertyNode->attribute("value", propertyNode->text);
            }
            rules.push_back(std::move(rule));
        }
    }
    windowBackground = color("window", windowBackground);
    surface = color("surface", surface);
    surfaceElevated = color("surface-elevated", surfaceElevated);
    return valid();
}

StyleSheet StyleSheet::from_xml(const std::string& source, std::string* error) {
    StyleSheet style = make_windows11_light();
    XmlDocument document;
    if (!document.parse(source, error) || !document.root() || !style.load_xml(*document.root(), error)) return {};
    return style;
}

XmlNode StyleSheet::to_xml() const {
    XmlNode document; document.name = "ui-styles"; document.attributes["schema"] = schema; document.attributes["version"] = std::to_string(version);
    XmlNode theme; theme.name = "theme"; theme.attributes["id"] = activeTheme; theme.attributes["scale"] = std::to_string(uiScale); theme.attributes["reduce-motion"] = reduceMotion ? "true" : "false";
    XmlNode fontNode; fontNode.name = "font"; fontNode.attributes["family"] = font.family; fontNode.attributes["path"] = font.path; fontNode.attributes["size"] = std::to_string(font.size); fontNode.attributes["weight"] = std::to_string(font.weight); fontNode.attributes["italic"] = font.italic ? "true" : "false"; theme.children.push_back(std::move(fontNode));
    XmlNode paletteNode; paletteNode.name = "palette";
    for (const auto& item : palette) { XmlNode colorNode; colorNode.name = "color"; colorNode.attributes["name"] = item.first; colorNode.attributes["value"] = hex_color(item.second); paletteNode.children.push_back(std::move(colorNode)); }
    theme.children.push_back(std::move(paletteNode));
    XmlNode metricsNode; metricsNode.name = "metrics";
    for (const auto& item : metrics) { XmlNode metricNode; metricNode.name = "metric"; metricNode.attributes["name"] = item.first; metricNode.attributes["value"] = std::to_string(item.second); metricsNode.children.push_back(std::move(metricNode)); }
    theme.children.push_back(std::move(metricsNode));
    XmlNode controlsNode; controlsNode.name = "controls";
    for (const auto& item : controls) {
        const ControlStyle& value = item.second;
        XmlNode controlNode; controlNode.name = "control"; controlNode.attributes["type"] = item.first;
        controlNode.attributes["variant"] = "default";
        controlNode.attributes["background"] = hex_color(value.background); controlNode.attributes["hover"] = hex_color(value.hover);
        controlNode.attributes["pressed"] = hex_color(value.pressed); controlNode.attributes["border"] = hex_color(value.border);
        controlNode.attributes["text"] = hex_color(value.text); controlNode.attributes["text-secondary"] = hex_color(value.textSecondary);
        controlNode.attributes["focus"] = hex_color(value.focus); controlNode.attributes["border-width"] = std::to_string(value.borderWidth);
        controlNode.attributes["padding-left"] = std::to_string(value.padding.left); controlNode.attributes["padding-top"] = std::to_string(value.padding.top);
        controlNode.attributes["padding-right"] = std::to_string(value.padding.right); controlNode.attributes["padding-bottom"] = std::to_string(value.padding.bottom);
        controlNode.attributes["radius-top-left"] = std::to_string(value.radii.topLeft); controlNode.attributes["radius-top-right"] = std::to_string(value.radii.topRight);
        controlNode.attributes["radius-bottom-right"] = std::to_string(value.radii.bottomRight); controlNode.attributes["radius-bottom-left"] = std::to_string(value.radii.bottomLeft);
        controlNode.attributes["transition"] = std::to_string(value.transition.durationMs); controlNode.attributes["easing"] = value.transition.easing;
        controlsNode.children.push_back(std::move(controlNode));
    }
    for (const auto& type : controlVariants) {
        for (const auto& variant : type.second) {
            const ControlStyle& value = variant.second;
            XmlNode controlNode; controlNode.name = "control"; controlNode.attributes["type"] = type.first; controlNode.attributes["variant"] = variant.first;
            controlNode.attributes["background"] = hex_color(value.background); controlNode.attributes["hover"] = hex_color(value.hover);
            controlNode.attributes["pressed"] = hex_color(value.pressed); controlNode.attributes["border"] = hex_color(value.border);
            controlNode.attributes["text"] = hex_color(value.text); controlNode.attributes["text-secondary"] = hex_color(value.textSecondary);
            controlNode.attributes["focus"] = hex_color(value.focus); controlNode.attributes["border-width"] = std::to_string(value.borderWidth);
            controlNode.attributes["padding-left"] = std::to_string(value.padding.left); controlNode.attributes["padding-top"] = std::to_string(value.padding.top);
            controlNode.attributes["padding-right"] = std::to_string(value.padding.right); controlNode.attributes["padding-bottom"] = std::to_string(value.padding.bottom);
            controlNode.attributes["radius-top-left"] = std::to_string(value.radii.topLeft); controlNode.attributes["radius-top-right"] = std::to_string(value.radii.topRight);
            controlNode.attributes["radius-bottom-right"] = std::to_string(value.radii.bottomRight); controlNode.attributes["radius-bottom-left"] = std::to_string(value.radii.bottomLeft);
            controlNode.attributes["transition"] = std::to_string(value.transition.durationMs); controlNode.attributes["easing"] = value.transition.easing;
            controlsNode.children.push_back(std::move(controlNode));
        }
    }
    theme.children.push_back(std::move(controlsNode));
    XmlNode rulesNode; rulesNode.name = "rules";
    for (const StyleRule& rule : rules) {
        XmlNode ruleNode; ruleNode.name = "rule"; ruleNode.attributes["selector"] = rule.selector; ruleNode.attributes["state"] = rule.state;
        for (const auto& property : rule.properties) {
            XmlNode propertyNode; propertyNode.name = "property"; propertyNode.attributes["name"] = property.first; propertyNode.attributes["value"] = property.second;
            ruleNode.children.push_back(std::move(propertyNode));
        }
        rulesNode.children.push_back(std::move(ruleNode));
    }
    theme.children.push_back(std::move(rulesNode)); document.children.push_back(std::move(theme));
    return document;
}

std::string StyleSheet::to_xml_string() const { XmlDocument document; document.set_root(to_xml()); return document.serialize(); }

const ControlStyle& StyleSheet::control(const std::string& name) const {
    static const ControlStyle empty;
    const auto found = controls.find(name);
    return found == controls.end() ? empty : found->second;
}
const ControlStyle& StyleSheet::control(const std::string& name, const std::string& variant) const {
    if (!variant.empty() && variant != "default") {
        const auto type = controlVariants.find(name);
        if (type != controlVariants.end()) {
            const auto found = type->second.find(variant);
            if (found != type->second.end()) return found->second;
        }
    }
    return control(name);
}
std::vector<std::string> StyleSheet::variants_for(const std::string& name) const {
    std::vector<std::string> result{"default"};
    const auto found = controlVariants.find(name);
    if (found != controlVariants.end()) for (const auto& variant : found->second) result.push_back(variant.first);
    return result;
}
const StyleRule* StyleSheet::rule(const std::string& selector, const std::string& state) const {
    for (auto iterator = rules.rbegin(); iterator != rules.rend(); ++iterator) {
        if ((iterator->selector == selector || iterator->selector == "." + selector || iterator->selector == selector + ":" + state) && (iterator->state == state || iterator->state == "*")) return &*iterator;
    }
    return nullptr;
}
std::string StyleSheet::property(const std::string& selector, const std::string& state, const std::string& name, const std::string& fallback) const {
    if (const StyleRule* matched = rule(selector, state)) {
        const auto found = matched->properties.find(name);
        if (found != matched->properties.end()) return found->second;
    }
    return fallback;
}
void StyleSheet::set_property(std::string selector, std::string state, std::string name, std::string value) {
    for (StyleRule& rule : rules) {
        if (rule.selector == selector && rule.state == state) { rule.properties[std::move(name)] = std::move(value); return; }
    }
    StyleRule rule; rule.selector = std::move(selector); rule.state = std::move(state); rule.properties[std::move(name)] = std::move(value); rules.push_back(std::move(rule));
}
Color StyleSheet::color(const std::string& name, Color fallback) const { const auto found = palette.find(name); return found == palette.end() ? fallback : found->second; }
float StyleSheet::metric(const std::string& name, float fallback) const { const auto found = metrics.find(name); return found == metrics.end() ? fallback : found->second; }
std::string StyleSheet::resolved_font_path(const std::string& platformFontDirectory) const { return font.path.empty() ? (platformFontDirectory.empty() ? font.family : platformFontDirectory + "/" + font.family + ".ttf") : font.path; }

} // namespace shinkou::uikit

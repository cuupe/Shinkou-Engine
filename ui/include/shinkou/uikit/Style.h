#pragma once

#include "Types.h"
#include "Xml.h"
#include <map>
#include <string>
#include <vector>

namespace shinkou::uikit {

struct FontSpec {
    std::string family = "Microsoft YaHei";
    std::string path;
    float size = 14.0f;
    int weight = 400;
    bool italic = false;
};

struct CornerRadii {
    float topLeft = 0.0f;
    float topRight = 0.0f;
    float bottomRight = 0.0f;
    float bottomLeft = 0.0f;
};

struct TransitionSpec {
    float durationMs = 120.0f;
    std::string easing = "ease-out";
};

struct ControlStyle {
    Color background{};
    Color hover{};
    Color pressed{};
    Color border{};
    Color text{};
    Color textSecondary{};
    Color focus{};
    float borderWidth = 0.0f;
    Insets padding{};
    CornerRadii radii{};
    TransitionSpec transition{};
};

struct StyleRule {
    std::string selector;
    std::string state = "normal";
    std::map<std::string, std::string> properties;
};

class StyleSheet {
public:
    static StyleSheet make_windows11_light();
    static StyleSheet make_windows11_dark();
    static StyleSheet make_apple_light();

    bool load_xml(const XmlNode& document, std::string* error = nullptr);
    static StyleSheet from_xml(const std::string& source, std::string* error = nullptr);
    XmlNode to_xml() const;
    std::string to_xml_string() const;

    const ControlStyle& control(const std::string& name) const;
    const ControlStyle& control(const std::string& name, const std::string& variant) const;
    std::vector<std::string> variants_for(const std::string& name) const;
    const StyleRule* rule(const std::string& selector, const std::string& state = "normal") const;
    std::string property(const std::string& selector, const std::string& state, const std::string& name, const std::string& fallback = {}) const;
    void set_property(std::string selector, std::string state, std::string name, std::string value);
    Color color(const std::string& name, Color fallback = {}) const;
    float metric(const std::string& name, float fallback = 0.0f) const;
    std::string resolved_font_path(const std::string& platformFontDirectory = {}) const;
    bool valid() const { return !activeTheme.empty() && !font.family.empty(); }

    std::string schema = "shinkou-ui";
    int version = 1;
    std::string activeTheme = "windows11";
    Color windowBackground{};
    Color surface{};
    Color surfaceElevated{};
    FontSpec font{};
    float uiScale = 1.0f;
    float dpiScale = 1.0f;
    bool reduceMotion = false;
    std::map<std::string, Color> palette;
    std::map<std::string, float> metrics;
    std::map<std::string, ControlStyle> controls;
    std::map<std::string, std::map<std::string, ControlStyle>> controlVariants;
    std::vector<StyleRule> rules;
};

} // namespace shinkou::uikit

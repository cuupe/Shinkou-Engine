#include "shinkou/data/Read.h"
#include "shinkou/data/Json.h"
#include "shinkou/ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <utility>

namespace shinkou::ui {
namespace {
using JsonValue = data::Value;
using JsonParser = data::JsonParser;

bool finite(float value) noexcept {
    return std::isfinite(static_cast<double>(value));
}

bool valid_name(std::string_view name) noexcept {
    if (name.empty())
        return false;
    for (const char character : name) {
        if (static_cast<unsigned char>(character) < 0x20u)
            return false;
    }
    return true;
}

void set_error(std::string *error, std::string message) {
    if (error)
        *error = std::move(message);
}

ThemeColor rgb(int red, int green, int blue) {
    return ThemeColor{red / 255.0f, green / 255.0f, blue / 255.0f};
}

TypographyStyle type(std::string family, float size, std::int32_t weight, float lineHeight,
                     bool italic = false) {
    return TypographyStyle{std::move(family), size, weight, lineHeight, italic};
}

// Domain validation consumes the shared value tree.

const JsonValue *member(const JsonValue &object, std::string_view name) {
    if (object.kind() != JsonValue::Kind::Object)
        return nullptr;
    const auto found = object.object_items().find(name);
    return found == object.object_items().end() ? nullptr : &found->second;
}

bool string_value(const JsonValue *value, std::string &output) {
    return value && data::convert(*value,output).ok();
}

bool number_value(const JsonValue *value, float &output) {
    return value && data::convert(*value,output).ok();
}

bool integer_value(const JsonValue *value, std::uint32_t &output) {
    return value && data::convert(*value,output).ok();
}



data::Value theme_value(const Theme& theme, bool metadata = true) {
    auto root = metadata ? data::document({kThemeSchema,kThemeSchemaVersion}) : data::Value::object();
    auto& fields = root.object_items();
    fields["id"] = theme.id;
    fields["displayName"] = theme.displayName;
    auto colors = data::Value::object();
    for (const auto& [name,color] : theme.colors)
        colors.object_items()[name] = data::Value::Array{color.r,color.g,color.b,color.a};
    fields["colors"] = std::move(colors);
    auto metrics = data::Value::object();
    for (const auto& [name,metric] : theme.metrics.tokens) metrics.object_items()[name] = metric;
    fields["metrics"] = std::move(metrics);
    auto typography = data::Value::object();
    for (const auto& [name,style] : theme.typography.tokens) {
        auto item = data::Value::object();
        item.object_items() = {{"family",style.family},{"size",style.size},{"weight",style.weight},
                              {"lineHeight",style.lineHeight},{"italic",style.italic}};
        typography.object_items()[name] = std::move(item);
    }
    fields["typography"] = std::move(typography);
    return root;
}

bool parse_theme_value(const JsonValue &root, Theme &theme, std::string &error) {
    if (root.kind() != JsonValue::Kind::Object) {
        error = "theme JSON root must be an object";
        return false;
    }
    const JsonValue *schema = member(root, "schema");
    const JsonValue *version = member(root, "version");
    std::string schemaName;
    std::uint32_t schemaVersion = 0;
    if (!string_value(schema, schemaName) || schemaName != kThemeSchema ||
        !integer_value(version, schemaVersion) || schemaVersion != kThemeSchemaVersion) {
        error = "unsupported or missing theme schema/version";
        return false;
    }
    Theme parsed;
    if (!string_value(member(root, "id"), parsed.id) ||
        !string_value(member(root, "displayName"), parsed.displayName) || !valid_name(parsed.id) ||
        parsed.displayName.empty()) {
        error = "theme id/displayName is invalid";
        return false;
    }
    const JsonValue *colors = member(root, "colors");
    if (!colors || colors->kind() != JsonValue::Kind::Object) {
        error = "theme colors must be an object";
        return false;
    }
    for (const auto &[name, value] : colors->object_items()) {
        if (!valid_name(name) || value.kind() != JsonValue::Kind::Array || value.array_items().size() != 4) {
            error = "color token must contain four components";
            return false;
        }
        ThemeColor color;
        if (!number_value(&value.array_items()[0], color.r) || !number_value(&value.array_items()[1], color.g) ||
            !number_value(&value.array_items()[2], color.b) || !number_value(&value.array_items()[3], color.a) ||
            !color.valid()) {
            error = "color component is outside [0, 1]";
            return false;
        }
        parsed.colors.emplace(name, color);
    }
    const JsonValue *metrics = member(root, "metrics");
    if (!metrics || metrics->kind() != JsonValue::Kind::Object) {
        error = "theme metrics must be an object";
        return false;
    }
    for (const auto &[name, value] : metrics->object_items()) {
        float metric = 0.0f;
        if (!valid_name(name) || !number_value(&value, metric) || metric < 0.0f ||
            metric > 1000000.0f) {
            error = "metric token is invalid";
            return false;
        }
        parsed.metrics.tokens.emplace(name, metric);
    }
    const JsonValue *typography = member(root, "typography");
    if (!typography || typography->kind() != JsonValue::Kind::Object) {
        error = "theme typography must be an object";
        return false;
    }
    for (const auto &[name, value] : typography->object_items()) {
        if (!valid_name(name) || value.kind() != JsonValue::Kind::Object) {
            error = "typography token is invalid";
            return false;
        }
        TypographyStyle style;
        if (!string_value(member(value, "family"), style.family) ||
            !number_value(member(value, "size"), style.size) ||
            !number_value(member(value, "lineHeight"), style.lineHeight) || !style.valid()) {
            error = "typography token has invalid fields";
            return false;
        }
        float weight = 0.0f;
        if (!number_value(member(value, "weight"), weight) || weight < 1.0f || weight > 1000.0f ||
            std::floor(weight) != weight) {
            error = "typography weight is invalid";
            return false;
        }
        style.weight = static_cast<std::int32_t>(weight);
        const JsonValue *italic = member(value, "italic");
        if (!italic || italic->kind() != JsonValue::Kind::Boolean) {
            error = "typography italic must be boolean";
            return false;
        }
        style.italic = italic->bool_or();
        parsed.typography.tokens.emplace(name, std::move(style));
    }
    if (!parsed.valid(&error))
        return false;
    theme = std::move(parsed);
    return true;
}

} // namespace

bool ThemeColor::valid() const noexcept {
    return finite(r) && finite(g) && finite(b) && finite(a) && r >= 0.0f && r <= 1.0f &&
           g >= 0.0f && g <= 1.0f && b >= 0.0f && b <= 1.0f && a >= 0.0f && a <= 1.0f;
}

void ThemeMetrics::set(std::string name, float value) {
    tokens[std::move(name)] = value;
}

const float *ThemeMetrics::find(std::string_view name) const noexcept {
    const auto found = tokens.find(name);
    return found == tokens.end() ? nullptr : &found->second;
}

float ThemeMetrics::value(std::string_view name, float fallback) const noexcept {
    const float *result = find(name);
    return result ? *result : fallback;
}

bool TypographyStyle::valid() const noexcept {
    return !family.empty() && family.find_first_of("\r\n\t") == std::string::npos && finite(size) &&
           size > 0.0f && weight >= 1 && weight <= 1000 && finite(lineHeight) &&
           lineHeight > 0.0f && lineHeight <= 100.0f;
}

void ThemeTypography::set(std::string name, TypographyStyle style) {
    tokens[std::move(name)] = std::move(style);
}

const TypographyStyle *ThemeTypography::find(std::string_view name) const noexcept {
    const auto found = tokens.find(name);
    return found == tokens.end() ? nullptr : &found->second;
}

const ThemeColor *Theme::color(std::string_view name) const noexcept {
    const auto found = colors.find(name);
    return found == colors.end() ? nullptr : &found->second;
}

const float *Theme::metric(std::string_view name) const noexcept {
    return metrics.find(name);
}

const TypographyStyle *Theme::type(std::string_view name) const noexcept {
    return typography.find(name);
}

bool Theme::valid(std::string *error) const {
    if (!valid_name(id) || displayName.empty()) {
        set_error(error, "theme id/displayName is invalid");
        return false;
    }
    for (const auto &[name, color] : colors)
        if (!valid_name(name) || !color.valid()) {
            set_error(error, "theme contains an invalid color token");
            return false;
        }
    for (const auto &[name, metric] : metrics.tokens)
        if (!valid_name(name) || !finite(metric) || metric < 0.0f || metric > 1000000.0f) {
            set_error(error, "theme contains an invalid metric token");
            return false;
        }
    for (const auto &[name, style] : typography.tokens)
        if (!valid_name(name) || !style.valid()) {
            set_error(error, "theme contains an invalid typography token");
            return false;
        }
    return true;
}

bool ThemeOverride::empty() const noexcept {
    return colors.empty() && metrics.empty() && typography.empty();
}

float relative_luminance(ThemeColor color) noexcept {
    const auto linear = [](float component) noexcept {
        component = std::clamp(component, 0.0f, 1.0f);
        return component <= 0.04045f ? component / 12.92f
                                     : std::pow((component + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * linear(color.r) + 0.7152f * linear(color.g) + 0.0722f * linear(color.b);
}

float contrast_ratio(ThemeColor foreground, ThemeColor background) noexcept {
    const float first = relative_luminance(foreground);
    const float second = relative_luminance(background);
    const float light = std::max(first, second);
    const float dark = std::min(first, second);
    return (light + 0.05f) / (dark + 0.05f);
}

ContrastCheck check_contrast(ThemeColor foreground, ThemeColor background,
                             float minimumRatio) noexcept {
    const float ratio = contrast_ratio(foreground, background);
    return ContrastCheck{ratio,
                         finite(minimumRatio) && minimumRatio >= 1.0f && ratio >= minimumRatio};
}

ThemeContrastReport check_theme_contrast(const Theme &theme, float minimumRatio) {
    ThemeContrastReport report;
    const auto check = [&](std::string_view foreground, std::string_view background) {
        const ThemeColor *fg = theme.color(foreground);
        const ThemeColor *bg = theme.color(background);
        if (!fg || !bg)
            return;
        const ContrastCheck result = check_contrast(*fg, *bg, minimumRatio);
        if (!result.passes) {
            report.passes = false;
            report.issues.push_back(
                ContrastIssue{std::string(foreground), std::string(background), result.ratio});
        }
    };
    check("text", "background");
    check("text-muted", "background");
    check("text", "surface");
    check("text-muted", "surface");
    check("on-accent", "accent");
    return report;
}

Theme make_dark_theme() {
    Theme theme{"dark", "Dark", {}, {}, {}};
    theme.colors = {{"background", rgb(18, 22, 29)},       {"surface", rgb(27, 34, 44)},
                    {"surface-elevated", rgb(37, 47, 60)}, {"border", rgb(73, 87, 105)},
                    {"text", rgb(244, 247, 251)},          {"text-muted", rgb(183, 195, 209)},
                    {"accent", rgb(92, 163, 255)},         {"accent-hover", rgb(137, 195, 255)},
                    {"accent-active", rgb(54, 128, 224)},  {"on-accent", rgb(5, 14, 26)},
                    {"success", rgb(91, 211, 142)},        {"warning", rgb(255, 209, 102)},
                    {"danger", rgb(255, 120, 133)},        {"selection", rgb(42, 88, 145)},
                    {"focus", rgb(137, 195, 255)}};
    theme.metrics.tokens = {
        {"space-xs", 4.0f},        {"space-sm", 8.0f},     {"space-md", 12.0f},
        {"space-lg", 16.0f},       {"space-xl", 24.0f},    {"corner-radius", 6.0f},
        {"control-height", 32.0f}, {"border-width", 1.0f}, {"panel-padding", 16.0f}};
    theme.typography.tokens = {{"body", type("Inter", 14.0f, 400, 1.4f)},
                               {"heading", type("Inter", 20.0f, 700, 1.2f)},
                               {"caption", type("Inter", 12.0f, 400, 1.3f)},
                               {"monospace", type("Consolas", 13.0f, 400, 1.35f)}};
    return theme;
}

Theme make_light_theme() {
    Theme theme{"light", "Light", {}, {}, {}};
    theme.colors = {{"background", rgb(248, 250, 252)},
                    {"surface", rgb(255, 255, 255)},
                    {"surface-elevated", rgb(255, 255, 255)},
                    {"border", rgb(148, 163, 184)},
                    {"text", rgb(15, 23, 42)},
                    {"text-muted", rgb(71, 85, 105)},
                    {"accent", rgb(25, 98, 190)},
                    {"accent-hover", rgb(18, 75, 148)},
                    {"accent-active", rgb(13, 59, 117)},
                    {"on-accent", rgb(255, 255, 255)},
                    {"success", rgb(20, 121, 66)},
                    {"warning", rgb(141, 82, 0)},
                    {"danger", rgb(180, 31, 48)},
                    {"selection", rgb(185, 215, 250)},
                    {"focus", rgb(25, 98, 190)}};
    theme.metrics.tokens = {
        {"space-xs", 4.0f},        {"space-sm", 8.0f},     {"space-md", 12.0f},
        {"space-lg", 16.0f},       {"space-xl", 24.0f},    {"corner-radius", 6.0f},
        {"control-height", 32.0f}, {"border-width", 1.0f}, {"panel-padding", 16.0f}};
    theme.typography.tokens = {{"body", type("Inter", 14.0f, 400, 1.4f)},
                               {"heading", type("Inter", 20.0f, 700, 1.2f)},
                               {"caption", type("Inter", 12.0f, 400, 1.3f)},
                               {"monospace", type("Consolas", 13.0f, 400, 1.35f)}};
    return theme;
}

Theme make_high_contrast_theme() {
    Theme theme{"high-contrast", "High Contrast", {}, {}, {}};
    theme.colors = {{"background", rgb(0, 0, 0)},          {"surface", rgb(0, 0, 0)},
                    {"surface-elevated", rgb(24, 24, 24)}, {"border", rgb(255, 255, 255)},
                    {"text", rgb(255, 255, 255)},          {"text-muted", rgb(238, 238, 238)},
                    {"accent", rgb(255, 230, 0)},          {"accent-hover", rgb(255, 255, 0)},
                    {"accent-active", rgb(255, 190, 0)},   {"on-accent", rgb(0, 0, 0)},
                    {"success", rgb(0, 255, 128)},         {"warning", rgb(255, 230, 0)},
                    {"danger", rgb(255, 96, 96)},          {"selection", rgb(0, 96, 192)},
                    {"focus", rgb(255, 255, 255)}};
    theme.metrics.tokens = {
        {"space-xs", 4.0f},        {"space-sm", 8.0f},     {"space-md", 12.0f},
        {"space-lg", 16.0f},       {"space-xl", 24.0f},    {"corner-radius", 2.0f},
        {"control-height", 34.0f}, {"border-width", 2.0f}, {"panel-padding", 16.0f}};
    theme.typography.tokens = {{"body", type("Inter", 14.0f, 500, 1.4f)},
                               {"heading", type("Inter", 20.0f, 800, 1.2f)},
                               {"caption", type("Inter", 12.0f, 500, 1.3f)},
                               {"monospace", type("Consolas", 13.0f, 500, 1.35f)}};
    return theme;
}

std::string serialize_json(const Theme &theme) {
    if (!theme.valid()) return {};
    auto encoded = data::write_json(theme_value(theme));
    return encoded ? std::move(encoded).value() : std::string{};
}

bool deserialize_json(std::string_view json, Theme &theme, std::string *error) {
    JsonValue root;
    std::string parseError;
    if (!JsonParser(json).parse(root, parseError)) {
        set_error(error, std::move(parseError));
        return false;
    }
    if (!parse_theme_value(root, theme, parseError)) {
        set_error(error, std::move(parseError));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool deserialize_json(std::string_view json, Theme *theme, std::string *error) {
    if (!theme) {
        set_error(error, "theme output pointer is null");
        return false;
    }
    return deserialize_json(json, *theme, error);
}

ThemeRegistry::ThemeRegistry() {
    register_theme(make_dark_theme());
    register_theme(make_light_theme());
    register_theme(make_high_contrast_theme());
    activeId_ = std::string(kDarkThemeId);
}

bool ThemeRegistry::register_theme(Theme theme, std::string *error) {
    if (!theme.valid(error))
        return false;
    if (themes_.find(theme.id) != themes_.end()) {
        set_error(error, "theme id is already registered");
        return false;
    }
    const std::string id = theme.id;
    themes_.emplace(id, Entry{theme, std::move(theme)});
    if (activeId_.empty())
        activeId_ = id;
    if (error)
        error->clear();
    return true;
}

const Theme *ThemeRegistry::find(std::string_view id) const noexcept {
    const auto found = themes_.find(id);
    return found == themes_.end() ? nullptr : &found->second.resolved;
}

Theme *ThemeRegistry::find(std::string_view id) noexcept {
    const auto found = themes_.find(id);
    return found == themes_.end() ? nullptr : &found->second.resolved;
}

const Theme *ThemeRegistry::active() const noexcept {
    return find(activeId_);
}

std::string_view ThemeRegistry::active_id() const noexcept {
    return activeId_;
}

bool ThemeRegistry::switch_theme(std::string_view id, std::string *error) {
    if (!find(id)) {
        set_error(error, "theme id is not registered");
        return false;
    }
    activeId_ = id;
    if (error)
        error->clear();
    return true;
}

bool ThemeRegistry::apply_color(std::string_view themeId, std::string_view token, ThemeColor value,
                                std::string *error) {
    const auto found = themes_.find(themeId);
    if (found == themes_.end()) {
        set_error(error, "theme id is not registered");
        return false;
    }
    if (!valid_name(token) || !value.valid()) {
        set_error(error, "color override is invalid");
        return false;
    }
    found->second.resolved.colors[std::string(token)] = value;
    if (error)
        error->clear();
    return true;
}

bool ThemeRegistry::apply_metric(std::string_view themeId, std::string_view token, float value,
                                 std::string *error) {
    const auto found = themes_.find(themeId);
    if (found == themes_.end()) {
        set_error(error, "theme id is not registered");
        return false;
    }
    if (!valid_name(token) || !finite(value) || value < 0.0f || value > 1000000.0f) {
        set_error(error, "metric override is invalid");
        return false;
    }
    found->second.resolved.metrics.tokens[std::string(token)] = value;
    if (error)
        error->clear();
    return true;
}

bool ThemeRegistry::apply_typography(std::string_view themeId, std::string_view token,
                                     TypographyStyle value, std::string *error) {
    const auto found = themes_.find(themeId);
    if (found == themes_.end()) {
        set_error(error, "theme id is not registered");
        return false;
    }
    if (!valid_name(token) || !value.valid()) {
        set_error(error, "typography override is invalid");
        return false;
    }
    found->second.resolved.typography.tokens[std::string(token)] = std::move(value);
    if (error)
        error->clear();
    return true;
}

bool ThemeRegistry::override_color(std::string_view token, ThemeColor value, std::string *error) {
    return override_color(activeId_, token, value, error);
}

bool ThemeRegistry::override_color(std::string_view themeId, std::string_view token,
                                   ThemeColor value, std::string *error) {
    return apply_color(themeId, token, value, error);
}

bool ThemeRegistry::override_metric(std::string_view token, float value, std::string *error) {
    return override_metric(activeId_, token, value, error);
}

bool ThemeRegistry::override_metric(std::string_view themeId, std::string_view token, float value,
                                    std::string *error) {
    return apply_metric(themeId, token, value, error);
}

bool ThemeRegistry::override_typography(std::string_view token, TypographyStyle value,
                                        std::string *error) {
    return override_typography(activeId_, token, std::move(value), error);
}

bool ThemeRegistry::override_typography(std::string_view themeId, std::string_view token,
                                        TypographyStyle value, std::string *error) {
    return apply_typography(themeId, token, std::move(value), error);
}

bool ThemeRegistry::apply_override(std::string_view themeId, const ThemeOverride &value,
                                   std::string *error) {
    const auto found = themes_.find(themeId);
    if (found == themes_.end()) {
        set_error(error, "theme id is not registered");
        return false;
    }
    Theme candidate = found->second.resolved;
    for (const auto &[token, color] : value.colors) {
        if (!valid_name(token) || !color.valid()) {
            set_error(error, "color override is invalid");
            return false;
        }
        candidate.colors[token] = color;
    }
    for (const auto &[token, metric] : value.metrics) {
        if (!valid_name(token) || !finite(metric) || metric < 0.0f || metric > 1000000.0f) {
            set_error(error, "metric override is invalid");
            return false;
        }
        candidate.metrics.tokens[token] = metric;
    }
    for (const auto &[token, style] : value.typography) {
        if (!valid_name(token) || !style.valid()) {
            set_error(error, "typography override is invalid");
            return false;
        }
        candidate.typography.tokens[token] = style;
    }
    found->second.resolved = std::move(candidate);
    if (error)
        error->clear();
    return true;
}

bool ThemeRegistry::clear_overrides(std::string_view themeId, std::string *error) {
    const auto found = themes_.find(themeId);
    if (found == themes_.end()) {
        set_error(error, "theme id is not registered");
        return false;
    }
    found->second.resolved = found->second.base;
    if (error)
        error->clear();
    return true;
}

void ThemeRegistry::clear_all_overrides() noexcept {
    for (auto &[id, entry] : themes_)
        entry.resolved = entry.base;
}

std::size_t ThemeRegistry::size() const noexcept {
    return themes_.size();
}

std::vector<std::string> ThemeRegistry::ids() const {
    std::vector<std::string> result;
    result.reserve(themes_.size());
    for (const auto &[id, entry] : themes_)
        result.push_back(id);
    return result;
}

std::string ThemeRegistry::serialize_json() const {
    auto root = data::document({kThemeRegistrySchema,kThemeSchemaVersion});
    root.object_items()["activeTheme"] = activeId_;
    auto themes = data::Value::array();
    for (const auto& [id,entry] : themes_) themes.array_items().push_back(theme_value(entry.resolved));
    root.object_items()["themes"] = std::move(themes);
    auto encoded = data::write_json(root);
    return encoded ? std::move(encoded).value() : std::string{};
}

bool ThemeRegistry::deserialize_json(std::string_view json, std::string *error) {
    JsonValue root;
    std::string parseError;
    if (!JsonParser(json).parse(root, parseError)) {
        set_error(error, std::move(parseError));
        return false;
    }
    if (root.kind() != JsonValue::Kind::Object) {
        set_error(error, "registry JSON root must be an object");
        return false;
    }
    std::string schema;
    std::uint32_t version = 0;
    std::string active;
    const JsonValue *themes = member(root, "themes");
    if (!string_value(member(root, "schema"), schema) || schema != kThemeRegistrySchema ||
        !integer_value(member(root, "version"), version) || version != kThemeSchemaVersion ||
        !string_value(member(root, "activeTheme"), active) || !themes ||
        themes->kind() != JsonValue::Kind::Array || themes->array_items().empty()) {
        set_error(error, "unsupported or incomplete theme registry schema");
        return false;
    }
    std::map<std::string, Entry, std::less<>> parsed;
    for (const JsonValue &value : themes->array_items()) {
        Theme theme;
        if (!parse_theme_value(JsonValue{value}, theme, parseError)) {
            set_error(error, std::move(parseError));
            return false;
        }
        const std::string id = theme.id;
        if (!parsed.emplace(id, Entry{theme, std::move(theme)}).second) {
            set_error(error, "duplicate theme id in registry");
            return false;
        }
    }
    if (parsed.find(active) == parsed.end()) {
        set_error(error, "active theme is not registered");
        return false;
    }
    themes_ = std::move(parsed);
    activeId_ = std::move(active);
    if (error)
        error->clear();
    return true;
}

} // namespace shinkou::ui

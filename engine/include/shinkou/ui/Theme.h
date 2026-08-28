#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::ui {

inline constexpr std::uint32_t kThemeSchemaVersion = 1;
inline constexpr std::string_view kThemeSchema = "shinkou.theme";
inline constexpr std::string_view kThemeRegistrySchema = "shinkou.theme-registry";
inline constexpr std::string_view kDarkThemeId = "dark";
inline constexpr std::string_view kLightThemeId = "light";
inline constexpr std::string_view kHighContrastThemeId = "high-contrast";

// A backend-neutral RGBA color. Components are normalized to the [0, 1] range.
struct ThemeColor {
    float r{0.0f};
    float g{0.0f};
    float b{0.0f};
    float a{1.0f};

    constexpr ThemeColor() = default;
    constexpr ThemeColor(float red, float green, float blue, float alpha = 1.0f)
        : r(red), g(green), b(blue), a(alpha) {}

    bool valid() const noexcept;
    friend constexpr bool operator==(const ThemeColor& left, const ThemeColor& right) {
        return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
    }
};

// Metric and typography collections intentionally use semantic token names. This keeps
// the core independent of any UI toolkit while allowing adapters to map tokens by name.
struct ThemeMetrics {
    std::map<std::string, float, std::less<>> tokens;

    void set(std::string name, float value);
    const float* find(std::string_view name) const noexcept;
    float value(std::string_view name, float fallback = 0.0f) const noexcept;
};

struct TypographyStyle {
    std::string family;
    float size{14.0f};
    std::int32_t weight{400};
    float lineHeight{1.2f};
    bool italic{false};

    bool valid() const noexcept;
    friend bool operator==(const TypographyStyle& left, const TypographyStyle& right) {
        return left.family == right.family && left.size == right.size && left.weight == right.weight &&
               left.lineHeight == right.lineHeight && left.italic == right.italic;
    }
};

struct ThemeTypography {
    std::map<std::string, TypographyStyle, std::less<>> tokens;

    void set(std::string name, TypographyStyle style);
    const TypographyStyle* find(std::string_view name) const noexcept;
};

struct Theme {
    std::string id;
    std::string displayName;
    std::map<std::string, ThemeColor, std::less<>> colors;
    ThemeMetrics metrics;
    ThemeTypography typography;

    const ThemeColor* color(std::string_view name) const noexcept;
    const float* metric(std::string_view name) const noexcept;
    const TypographyStyle* type(std::string_view name) const noexcept;
    bool valid(std::string* error = nullptr) const;
};

struct ThemeOverride {
    std::map<std::string, ThemeColor, std::less<>> colors;
    std::map<std::string, float, std::less<>> metrics;
    std::map<std::string, TypographyStyle, std::less<>> typography;

    bool empty() const noexcept;
};

struct ContrastCheck {
    float ratio{1.0f};
    bool passes{false};
};

struct ContrastIssue {
    std::string foreground;
    std::string background;
    float ratio{1.0f};
};

struct ThemeContrastReport {
    bool passes{true};
    std::vector<ContrastIssue> issues;
};

float relative_luminance(ThemeColor color) noexcept;
float contrast_ratio(ThemeColor foreground, ThemeColor background) noexcept;
ContrastCheck check_contrast(ThemeColor foreground, ThemeColor background,
                              float minimumRatio = 4.5f) noexcept;
ThemeContrastReport check_theme_contrast(const Theme& theme, float minimumRatio = 4.5f);

Theme make_dark_theme();
Theme make_light_theme();
Theme make_high_contrast_theme();

// The standalone theme JSON format is deterministic and contains schema/version metadata.
std::string serialize_json(const Theme& theme);
bool deserialize_json(std::string_view json, Theme& theme, std::string* error = nullptr);
bool deserialize_json(std::string_view json, Theme* theme, std::string* error = nullptr);

class ThemeRegistry {
public:
    ThemeRegistry();

    bool register_theme(Theme theme, std::string* error = nullptr);
    const Theme* find(std::string_view id) const noexcept;
    Theme* find(std::string_view id) noexcept;
    const Theme* active() const noexcept;
    std::string_view active_id() const noexcept;
    bool switch_theme(std::string_view id, std::string* error = nullptr);

    bool override_color(std::string_view token, ThemeColor value, std::string* error = nullptr);
    bool override_color(std::string_view themeId, std::string_view token, ThemeColor value,
                        std::string* error = nullptr);
    bool override_metric(std::string_view token, float value, std::string* error = nullptr);
    bool override_metric(std::string_view themeId, std::string_view token, float value,
                         std::string* error = nullptr);
    bool override_typography(std::string_view token, TypographyStyle value,
                             std::string* error = nullptr);
    bool override_typography(std::string_view themeId, std::string_view token,
                             TypographyStyle value, std::string* error = nullptr);
    bool apply_override(std::string_view themeId, const ThemeOverride& value,
                        std::string* error = nullptr);
    bool clear_overrides(std::string_view themeId, std::string* error = nullptr);
    void clear_all_overrides() noexcept;

    std::size_t size() const noexcept;
    std::vector<std::string> ids() const;
    std::string serialize_json() const;
    bool deserialize_json(std::string_view json, std::string* error = nullptr);

private:
    struct Entry {
        Theme base;
        Theme resolved;
    };

    std::map<std::string, Entry, std::less<>> themes_;
    std::string activeId_;

    bool apply_color(std::string_view themeId, std::string_view token, ThemeColor value,
                     std::string* error);
    bool apply_metric(std::string_view themeId, std::string_view token, float value,
                      std::string* error);
    bool apply_typography(std::string_view themeId, std::string_view token,
                          TypographyStyle value, std::string* error);
};

} // namespace shinkou::ui

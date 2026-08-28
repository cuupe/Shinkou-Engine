#pragma once

#include "shinkou/ui/Theme.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace shinkou::ui {

inline constexpr std::uint32_t kStyleSchemaVersion = 1;
inline constexpr std::string_view kStyleSchema = "shinkou.ui-style";

enum class BackgroundMode : std::uint8_t { Solid, Gradient, Image };

struct UiBackground {
    BackgroundMode mode{BackgroundMode::Solid};
    ThemeColor primary{0.055f, 0.067f, 0.09f, 1.0f};
    ThemeColor secondary{0.10f, 0.13f, 0.17f, 1.0f};
    std::string imagePath{};
    float opacity{1.0f};
};

struct UiStyleConfig {
    std::uint32_t version{kStyleSchemaVersion};
    std::string activeTheme{"dark"};
    float uiScale{1.0f};
    float fontSize{14.0f};
    std::string fontPath{};
    std::string fallbackFontPath{};
    bool compactControls{false};
    bool enableAnimations{true};
    bool reduceMotion{false};
    UiBackground background{};
    ThemeOverride themeOverride{};

    bool valid(std::string* error = nullptr) const;
};

std::string serialize_json(const UiStyleConfig& config, bool pretty = true);
bool deserialize_json(std::string_view json, UiStyleConfig& config, std::string* error = nullptr);
std::string serialize_xml(const UiStyleConfig& config, bool pretty = true);
bool deserialize_xml(std::string_view xml, UiStyleConfig& config, std::string* error = nullptr);
bool load_style_file(const std::filesystem::path& path, UiStyleConfig& config, std::string* error = nullptr);
bool save_style_file(const std::filesystem::path& path, const UiStyleConfig& config,
                    std::string* error = nullptr);

} // namespace shinkou::ui

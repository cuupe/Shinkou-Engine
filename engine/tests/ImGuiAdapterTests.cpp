#include "shinkou/ui/ImGuiAdapter.h"

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    using namespace shinkou::ui;

    ThemeRegistry registry;
    UiStyleConfig config;
    config.uiScale = 1.25f;
    config.fontSize = 16.0f;
    config.fontPath = "fonts/editor.ttf";
    config.fallbackFontPath = "fonts/fallback.ttf";
    config.background.mode = BackgroundMode::Gradient;
    config.background.primary = ThemeColor{0.04f, 0.05f, 0.08f, 1.0f};
    config.background.secondary = ThemeColor{0.12f, 0.16f, 0.22f, 1.0f};
    config.background.opacity = 0.85f;

    ImGuiApplyOptions options;
    options.projectRoot = "C:/Shinkou/TestProject";
    options.dpiScale = 1.5f;
    options.maxControlRounding = 4.0f;

    const auto font = ImGuiAdapter::resolve_font_path(config, options);
    assert(font == std::filesystem::path("C:/Shinkou/TestProject/fonts/editor.ttf"));
    const auto fallback = ImGuiAdapter::resolve_fallback_font_path(config, options);
    assert(fallback == std::filesystem::path("C:/Shinkou/TestProject/fonts/fallback.ttf"));
    assert(std::fabs(ImGuiAdapter::effective_scale(config, options) - 1.875f) < 0.001f);
    assert(std::fabs(ImGuiAdapter::effective_font_size(*registry.active(), config, options) - 30.0f) < 0.001f);
    assert(ImGuiAdapter::limited_rounding(*registry.active(), config, options) <= 7.5f);

    const auto result = ImGuiAdapter::apply(registry, config, options);
    assert(result.optionsValid);
    assert(result.scale > 1.8f && result.scale < 1.9f);
    assert(result.fontFamily == "Microsoft YaHei");
    assert(result.background.mode == BackgroundMode::Gradient);
    assert(result.background.primary == config.background.primary);
    assert(result.background.secondary == config.background.secondary);
    assert(result.background.opacity == config.background.opacity);
#if !defined(SHINKOU_WITH_IMGUI)
    assert(!result.backendAvailable);
    assert(!result.styleApplied);
    assert(result.message.find("description") != std::string::npos);
#endif

    ImGuiApplyOptions customFont;
    customFont.projectRoot = "D:/Project";
    customFont.defaultFontPath = "assets/fonts/custom.ttf";
    UiStyleConfig defaultConfig;
    const auto custom = ImGuiAdapter::resolve_font_path(defaultConfig, customFont);
    assert(custom == std::filesystem::path("D:/Project/assets/fonts/custom.ttf"));

    std::cout << "ImGui adapter style, DPI, font path, flat rounding and backend-neutral fallback passed\n";
    return 0;
}

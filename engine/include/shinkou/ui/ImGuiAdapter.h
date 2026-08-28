#pragma once

#include "shinkou/ui/Style.h"

#include <filesystem>
#include <string>

namespace shinkou::ui {

// Backend-neutral controls for applying the editor look.  No ImGui types are
// exposed here, so the style pipeline can be used by tests and future backends.
struct ImGuiApplyOptions {
    std::filesystem::path projectRoot{};
    float dpiScale{1.0f};
    float maxControlRounding{4.0f};
    bool applyStyle{true};
    bool applyFont{true};
    bool preferProjectFont{true};
    bool allowSystemFontFallback{true};
    std::string defaultFontFamily{"Microsoft YaHei"};
    std::filesystem::path defaultFontPath{};
    std::filesystem::path fallbackFontPath{};
};

struct ImGuiBackgroundParameters {
    BackgroundMode mode{BackgroundMode::Solid};
    ThemeColor primary{};
    ThemeColor secondary{};
    std::filesystem::path imagePath{};
    float opacity{1.0f};
};

// A portable description of the result.  It is intentionally useful even
// when SHINKOU_WITH_IMGUI is not defined: callers can still inspect the
// resolved font/background data and decide how another renderer consumes it.
struct ImGuiApplyResult {
    bool backendAvailable{false};
    bool styleApplied{false};
    bool fontLoaded{false};
    bool fallbackFontLoaded{false};
    bool usedSystemFontFallback{false};
    bool optionsValid{true};
    float scale{1.0f};
    float fontSize{14.0f};
    std::string fontFamily{"Microsoft YaHei"};
    std::filesystem::path fontPath{};
    std::filesystem::path fallbackFontPath{};
    ImGuiBackgroundParameters background{};
    std::string message{};
};

class ImGuiAdapter final {
public:
    // Resolve a project-root-relative font path without touching the file
    // system. Absolute paths are preserved; relative paths are rooted at
    // options.projectRoot when one is supplied.
    static std::filesystem::path resolve_font_path(const UiStyleConfig& config,
                                                   const ImGuiApplyOptions& options = {});
    static std::filesystem::path resolve_fallback_font_path(
        const UiStyleConfig& config, const ImGuiApplyOptions& options = {});

    static float effective_scale(const UiStyleConfig& config,
                                 const ImGuiApplyOptions& options = {}) noexcept;
    static float effective_font_size(const Theme& theme, const UiStyleConfig& config,
                                     const ImGuiApplyOptions& options = {}) noexcept;
    static float limited_rounding(const Theme& theme, const UiStyleConfig& config,
                                  const ImGuiApplyOptions& options = {}) noexcept;

    // Produces the backend-neutral result and, when available, applies it to
    // the current ImGui context. It is safe to call without an ImGui build.
    static ImGuiApplyResult apply(const Theme& theme, const UiStyleConfig& config,
                                  const ImGuiApplyOptions& options = {});
    static ImGuiApplyResult apply(const ThemeRegistry& registry, const UiStyleConfig& config,
                                  const ImGuiApplyOptions& options = {});
};

} // namespace shinkou::ui

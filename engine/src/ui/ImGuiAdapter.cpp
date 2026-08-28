#include "shinkou/ui/ImGuiAdapter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string_view>

#if defined(SHINKOU_WITH_IMGUI)
#include <imgui.h>
#endif

namespace shinkou::ui {
namespace {

constexpr float kMinimumScale = 0.5f;
constexpr float kMaximumScale = 3.0f;
constexpr float kMinimumFontSize = 6.0f;
constexpr float kMaximumFontSize = 96.0f;

float finite_or(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

float clamp_scale(float value) noexcept {
    return std::clamp(finite_or(value, 1.0f), kMinimumScale, kMaximumScale);
}

float clamp_non_negative(float value, float fallback = 0.0f) noexcept {
    return std::max(0.0f, finite_or(value, fallback));
}

float metric_or(const Theme& theme, std::string_view name, float fallback) noexcept {
    const auto* value = theme.metric(name);
    return value && std::isfinite(*value) ? *value : fallback;
}

std::filesystem::path root_relative(const std::filesystem::path& path,
                                    const ImGuiApplyOptions& options) {
    if (path.empty() || path.is_absolute() || options.projectRoot.empty()) return path.lexically_normal();
    return (options.projectRoot / path).lexically_normal();
}

std::filesystem::path windows_font_directory() {
#if defined(_WIN32)
    if (const char* windir = std::getenv("WINDIR"); windir && *windir)
        return std::filesystem::path(windir) / "Fonts";
    return std::filesystem::path("C:/Windows/Fonts");
#else
    return {};
#endif
}

std::filesystem::path find_existing_default_font(const ImGuiApplyOptions& options) {
    if (!options.defaultFontPath.empty()) {
        const auto candidate = root_relative(options.defaultFontPath, options);
        if (std::filesystem::exists(candidate)) return candidate;
    }
    if (options.preferProjectFont && !options.projectRoot.empty()) {
        for (const auto& relative : {std::filesystem::path("assets/fonts/msyh.ttc"),
                                     std::filesystem::path("assets/fonts/msyh.ttf"),
                                     std::filesystem::path("fonts/msyh.ttc"),
                                     std::filesystem::path("fonts/msyh.ttf")}) {
            const auto candidate = (options.projectRoot / relative).lexically_normal();
            if (std::filesystem::exists(candidate)) return candidate;
        }
    }
    if (options.allowSystemFontFallback) {
        const auto directory = windows_font_directory();
        for (const auto& name : {"msyh.ttc", "msyh.ttf", "msyhbd.ttc"}) {
            const auto candidate = directory / name;
            if (!candidate.empty() && std::filesystem::exists(candidate)) return candidate;
        }
    }
    return {};
}

#if defined(SHINKOU_WITH_IMGUI)
ThemeColor color_or(const Theme& theme, std::string_view name, ThemeColor fallback) noexcept {
    const auto* value = theme.color(name);
    return value && value->valid() ? *value : fallback;
}

ImVec4 imgui_color(ThemeColor color) noexcept {
    return ImVec4(color.r, color.g, color.b, color.a);
}

void set_color(ImGuiStyle& style, ImGuiCol index, ThemeColor color) noexcept {
    style.Colors[index] = imgui_color(color);
}

void apply_imgui_style(ImGuiStyle& style, const Theme& theme, const UiStyleConfig& config,
                       float scale, float fontSize, float maxRounding) {
    const ThemeColor background = config.background.primary.valid()
        ? config.background.primary
        : color_or(theme, "background", ThemeColor{0.07f, 0.08f, 0.11f});
    const ThemeColor surface = color_or(theme, "surface", ThemeColor{0.12f, 0.14f, 0.18f});
    const ThemeColor elevated = color_or(theme, "surface-elevated", surface);
    const ThemeColor border = color_or(theme, "border", ThemeColor{0.28f, 0.32f, 0.38f});
    const ThemeColor text = color_or(theme, "text", ThemeColor{0.95f, 0.97f, 1.0f});
    const ThemeColor muted = color_or(theme, "text-muted", ThemeColor{0.68f, 0.72f, 0.78f});
    const ThemeColor accent = color_or(theme, "accent", ThemeColor{0.36f, 0.64f, 1.0f});
    const ThemeColor accentHover = color_or(theme, "accent-hover", accent);
    const ThemeColor accentActive = color_or(theme, "accent-active", accent);
    const ThemeColor selection = color_or(theme, "selection", accent);
    const ThemeColor focus = color_or(theme, "focus", accentHover);

    // Flat editor surfaces: panels and dock hosts remain square. Rounding is
    // reserved for interactive controls and is capped by ApplyOptions.
    const float corner = std::clamp(clamp_non_negative(metric_or(theme, "corner-radius", 3.0f)) * scale,
                                    0.0f, std::max(0.0f, maxRounding * scale));
    const float borderWidth = clamp_non_negative(metric_or(theme, "border-width", 1.0f)) * scale;
    const float controlHeight = clamp_non_negative(metric_or(theme, "control-height", 32.0f)) * scale;
    const float panelPadding = clamp_non_negative(metric_or(theme, "panel-padding", 16.0f)) * scale;
    const float spaceSm = clamp_non_negative(metric_or(theme, "space-sm", 8.0f)) * scale;
    const float spaceMd = clamp_non_negative(metric_or(theme, "space-md", 12.0f)) * scale;

    set_color(style, ImGuiCol_Text, text);
    set_color(style, ImGuiCol_TextDisabled, muted);
    set_color(style, ImGuiCol_WindowBg, ThemeColor{background.r, background.g, background.b,
                                                   background.a * config.background.opacity});
    set_color(style, ImGuiCol_ChildBg, surface);
    set_color(style, ImGuiCol_PopupBg, elevated);
    set_color(style, ImGuiCol_Border, border);
    set_color(style, ImGuiCol_BorderShadow, ThemeColor{0, 0, 0, 0});
    set_color(style, ImGuiCol_FrameBg, surface);
    set_color(style, ImGuiCol_FrameBgHovered, elevated);
    set_color(style, ImGuiCol_FrameBgActive, accentActive);
    set_color(style, ImGuiCol_TitleBg, background);
    set_color(style, ImGuiCol_TitleBgActive, surface);
    set_color(style, ImGuiCol_TitleBgCollapsed, background);
    set_color(style, ImGuiCol_MenuBarBg, background);
    set_color(style, ImGuiCol_ScrollbarBg, background);
    set_color(style, ImGuiCol_ScrollbarGrab, border);
    set_color(style, ImGuiCol_ScrollbarGrabHovered, elevated);
    set_color(style, ImGuiCol_ScrollbarGrabActive, accent);
    set_color(style, ImGuiCol_CheckMark, accent);
    set_color(style, ImGuiCol_SliderGrab, accent);
    set_color(style, ImGuiCol_SliderGrabActive, accentHover);
    set_color(style, ImGuiCol_Button, surface);
    set_color(style, ImGuiCol_ButtonHovered, elevated);
    set_color(style, ImGuiCol_ButtonActive, accentActive);
    set_color(style, ImGuiCol_Header, surface);
    set_color(style, ImGuiCol_HeaderHovered, elevated);
    set_color(style, ImGuiCol_HeaderActive, selection);
    set_color(style, ImGuiCol_Separator, border);
    set_color(style, ImGuiCol_SeparatorHovered, accentHover);
    set_color(style, ImGuiCol_SeparatorActive, accent);
    set_color(style, ImGuiCol_ResizeGrip, border);
    set_color(style, ImGuiCol_ResizeGripHovered, accentHover);
    set_color(style, ImGuiCol_ResizeGripActive, accent);
    set_color(style, ImGuiCol_Tab, surface);
    set_color(style, ImGuiCol_TabHovered, elevated);
    set_color(style, ImGuiCol_TabSelected, background);
    set_color(style, ImGuiCol_TabSelectedOverline, accent);
    set_color(style, ImGuiCol_TabDimmed, background);
    set_color(style, ImGuiCol_TabDimmedSelected, surface);
    set_color(style, ImGuiCol_TabDimmedSelectedOverline, muted);
#if defined(IMGUI_HAS_DOCK)
#if defined(IMGUI_HAS_DOCK)
    set_color(style, ImGuiCol_DockingPreview, ThemeColor{selection.r, selection.g, selection.b, 0.55f});
    set_color(style, ImGuiCol_DockingEmptyBg, background);
#endif
#endif
    set_color(style, ImGuiCol_NavHighlight, focus);
    set_color(style, ImGuiCol_ModalWindowDimBg, ThemeColor{0, 0, 0, 0.55f});

    style.WindowPadding = ImVec2(panelPadding, panelPadding);
    style.FramePadding = ImVec2(spaceSm, std::max(2.0f * scale, (controlHeight - fontSize) * 0.5f));
    style.ItemSpacing = ImVec2(spaceMd, spaceSm);
    style.ItemInnerSpacing = ImVec2(spaceSm, spaceSm);
    style.IndentSpacing = 18.0f * scale;
    style.ScrollbarSize = 14.0f * scale;
    style.GrabMinSize = 10.0f * scale;
    style.WindowBorderSize = borderWidth;
    style.ChildBorderSize = borderWidth;
    style.PopupBorderSize = borderWidth;
    style.FrameBorderSize = borderWidth;
    style.TabBorderSize = 0.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.PopupRounding = corner;
    style.FrameRounding = corner;
    style.ScrollbarRounding = std::min(corner, 3.0f * scale);
    style.GrabRounding = std::min(corner, 3.0f * scale);
    style.TabRounding = 0.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;
}
#endif

ImGuiApplyResult make_result(const Theme& theme, const UiStyleConfig& config,
                             const ImGuiApplyOptions& options) {
    ImGuiApplyResult result;
    result.optionsValid = config.valid(nullptr) && std::isfinite(options.dpiScale) &&
        std::isfinite(options.maxControlRounding) && options.maxControlRounding >= 0.0f;
    result.scale = ImGuiAdapter::effective_scale(config, options);
    result.fontSize = ImGuiAdapter::effective_font_size(theme, config, options);
    result.fontFamily = options.defaultFontFamily.empty() ? "Microsoft YaHei" : options.defaultFontFamily;
    result.fontPath = ImGuiAdapter::resolve_font_path(config, options);
    result.fallbackFontPath = ImGuiAdapter::resolve_fallback_font_path(config, options);
    result.background.mode = config.background.mode;
    result.background.primary = config.background.primary;
    result.background.secondary = config.background.secondary;
    result.background.imagePath = root_relative(config.background.imagePath, options);
    result.background.opacity = std::clamp(finite_or(config.background.opacity, 1.0f), 0.0f, 1.0f);
    return result;
}

} // namespace

std::filesystem::path ImGuiAdapter::resolve_font_path(const UiStyleConfig& config,
                                                       const ImGuiApplyOptions& options) {
    if (!config.fontPath.empty()) return root_relative(config.fontPath, options);
    // An explicit adapter default is a user choice, so preserve it even when
    // the file is not present yet. The apply step can then report/load a
    // system fallback without losing the configured project path.
    if (!options.defaultFontPath.empty()) return root_relative(options.defaultFontPath, options);
    if (const auto projectOrSystem = find_existing_default_font(options); !projectOrSystem.empty())
        return projectOrSystem;
    return {};
}

std::filesystem::path ImGuiAdapter::resolve_fallback_font_path(
    const UiStyleConfig& config, const ImGuiApplyOptions& options) {
    if (!config.fallbackFontPath.empty()) return root_relative(config.fallbackFontPath, options);
    return root_relative(options.fallbackFontPath, options);
}

float ImGuiAdapter::effective_scale(const UiStyleConfig& config,
                                    const ImGuiApplyOptions& options) noexcept {
    return clamp_scale(clamp_scale(config.uiScale) * clamp_scale(options.dpiScale));
}

float ImGuiAdapter::effective_font_size(const Theme& theme, const UiStyleConfig& config,
                                        const ImGuiApplyOptions& options) noexcept {
    float size = config.fontSize;
    if (!std::isfinite(size) || size <= 0.0f) size = theme.type("body") ? theme.type("body")->size : 14.0f;
    return std::clamp(finite_or(size, 14.0f), kMinimumFontSize, kMaximumFontSize) *
        effective_scale(config, options);
}

float ImGuiAdapter::limited_rounding(const Theme& theme, const UiStyleConfig& config,
                                     const ImGuiApplyOptions& options) noexcept {
    const float limit = std::max(0.0f, finite_or(options.maxControlRounding, 4.0f));
    return std::clamp(clamp_non_negative(metric_or(theme, "corner-radius", 3.0f)) *
                          effective_scale(config, options),
                      0.0f, limit * effective_scale(config, options));
}

ImGuiApplyResult ImGuiAdapter::apply(const Theme& theme, const UiStyleConfig& config,
                                     const ImGuiApplyOptions& options) {
    auto result = make_result(theme, config, options);
#if defined(SHINKOU_WITH_IMGUI)
    result.backendAvailable = ImGui::GetCurrentContext() != nullptr;
    if (!result.backendAvailable) {
        result.message = "ImGui context is not available";
        return result;
    }
    if (options.applyStyle) {
        apply_imgui_style(ImGui::GetStyle(), theme, config, result.scale, result.fontSize,
                          std::max(0.0f, finite_or(options.maxControlRounding, 4.0f)));
        result.styleApplied = true;
    }
    if (options.applyFont) {
        auto& io = ImGui::GetIO();
        ImFont* font = nullptr;
        if (!result.fontPath.empty() && std::filesystem::exists(result.fontPath))
            font = io.Fonts->AddFontFromFileTTF(result.fontPath.string().c_str(), result.fontSize);
        if (!font && !result.fallbackFontPath.empty() && std::filesystem::exists(result.fallbackFontPath)) {
            font = io.Fonts->AddFontFromFileTTF(result.fallbackFontPath.string().c_str(), result.fontSize);
            result.fallbackFontLoaded = font != nullptr;
        }
        if (!font && options.allowSystemFontFallback) {
            const auto systemPath = find_existing_default_font(options);
            if (!systemPath.empty() && systemPath != result.fontPath && systemPath != result.fallbackFontPath) {
                font = io.Fonts->AddFontFromFileTTF(systemPath.string().c_str(), result.fontSize);
                result.usedSystemFontFallback = font != nullptr;
            }
        }
        if (font) {
            io.FontDefault = font;
            result.fontLoaded = true;
        }
    }
    result.message = result.styleApplied || result.fontLoaded ? "ImGui style applied" : "No ImGui changes requested";
#else
    result.message = "ImGui backend is not enabled; style description was resolved only";
#endif
    return result;
}

ImGuiApplyResult ImGuiAdapter::apply(const ThemeRegistry& registry, const UiStyleConfig& config,
                                     const ImGuiApplyOptions& options) {
    const Theme* theme = registry.find(config.activeTheme);
    if (!theme) theme = registry.active();
    if (!theme) {
        ImGuiApplyResult result;
        result.optionsValid = false;
        result.message = "No active theme is available";
        return result;
    }
    return apply(*theme, config, options);
}

} // namespace shinkou::ui

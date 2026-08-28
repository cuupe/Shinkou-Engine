#include "shinkou/ui/Theme.h"

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    using namespace shinkou::ui;

    ThemeRegistry registry;
    assert(registry.size() == 3);
    assert(registry.find(kDarkThemeId) != nullptr);
    assert(registry.find(kLightThemeId) != nullptr);
    assert(registry.find(kHighContrastThemeId) != nullptr);
    assert(registry.active_id() == kDarkThemeId);

    for (const auto id : {kDarkThemeId, kLightThemeId, kHighContrastThemeId}) {
        const Theme* theme = registry.find(id);
        assert(theme != nullptr);
        assert(check_theme_contrast(*theme).passes);
        assert(theme->color("text") != nullptr);
        assert(theme->metric("control-height") != nullptr);
        assert(theme->type("body") != nullptr);
    }
    assert(std::fabs(contrast_ratio(ThemeColor{1, 1, 1}, ThemeColor{0, 0, 0}) - 21.0f) < 0.01f);
    assert(check_contrast(ThemeColor{1, 1, 1}, ThemeColor{0, 0, 0}).passes);

    std::string error;
    assert(registry.switch_theme(kLightThemeId, &error));
    assert(registry.active_id() == kLightThemeId);
    assert(registry.override_color("text", ThemeColor{0.2f, 0.2f, 0.2f}, &error));
    assert(registry.active()->color("text")->r == 0.2f);
    assert(registry.override_metric(kLightThemeId, "control-height", 40.0f, &error));
    assert(registry.find(kLightThemeId)->metrics.value("control-height") == 40.0f);
    assert(registry.override_typography("body", TypographyStyle{"Test Sans", 16.0f, 500, 1.5f}, &error));
    assert(registry.active()->type("body")->family == "Test Sans");
    assert(registry.clear_overrides(kLightThemeId, &error));
    assert(registry.find(kLightThemeId)->metrics.value("control-height") == 32.0f);
    assert(registry.find(kLightThemeId)->type("body")->family == "Inter");

    Theme custom{"custom", "Custom", {}, {}, {}};
    custom.colors["background"] = ThemeColor{0, 0, 0};
    custom.colors["text"] = ThemeColor{1, 1, 1};
    custom.metrics.set("control-height", 28.0f);
    custom.typography.set("body", TypographyStyle{"Custom", 15.0f, 400, 1.4f});
    assert(registry.register_theme(custom, &error));
    assert(registry.switch_theme("custom", &error));
    assert(registry.active()->color("text")->g == 1.0f);

    const Theme beforeBadTheme = *registry.active();
    assert(!deserialize_json("{\"schema\":\"shinkou.theme\",\"version\":999}", *registry.find("custom"), &error));
    assert(registry.find("custom")->id == beforeBadTheme.id);
    assert(registry.find("custom")->colors == beforeBadTheme.colors);

    const std::string themeJson = serialize_json(*registry.find("custom"));
    Theme restored;
    assert(deserialize_json(themeJson, restored, &error));
    assert(restored.id == "custom");
    assert(restored.colors == registry.find("custom")->colors);
    assert(restored.metrics.value("control-height") == 28.0f);
    assert(restored.type("body")->family == "Custom");
    assert(themeJson.find("\"schema\":\"shinkou.theme\"") != std::string::npos);
    assert(themeJson.find("\"version\":1") != std::string::npos);

    const std::string registryJson = registry.serialize_json();
    ThemeRegistry restoredRegistry;
    assert(restoredRegistry.deserialize_json(registryJson, &error));
    assert(restoredRegistry.size() == 4);
    assert(restoredRegistry.active_id() == "custom");
    assert(restoredRegistry.find("custom")->type("body")->family == "Custom");

    const std::string registryBeforeBad = restoredRegistry.serialize_json();
    assert(!restoredRegistry.deserialize_json("{\"schema\":\"shinkou.theme-registry\",\"version\":1,\"themes\":[]}", &error));
    assert(restoredRegistry.serialize_json() == registryBeforeBad);
    assert(!restoredRegistry.switch_theme("missing", &error));
    assert(!registry.override_color("custom", "bad", ThemeColor{2, 0, 0}, &error));

    std::cout << "theme tests passed\n";
    return 0;
}

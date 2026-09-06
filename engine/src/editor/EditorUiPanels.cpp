#include "shinkou/editor/EditorUi.h"
#include "shinkou/editor/EditorLayer.h"
#include "shinkou/render/Renderer.h"
#include "shinkou/ui/Performance.h"
#include <iomanip>
#include <sstream>

namespace shinkou::editor {
void EditorUi::draw_tools_panel(const DockRect& bounds, std::string_view id,
                               const render::Renderer& renderer, const EditorLayoutState& layout) {
    const std::string panel(id);
    toolRects_[panel] = bounds;
    set_region("tools:"+panel, rect(bounds), true);
    const float contentHeight = id=="settings" ? 430.0f : id=="profiler" ? 300.0f : 180.0f;
    toolScroll_[panel] = std::clamp(toolScroll_[panel], 0.0f, std::max(0.0f, contentHeight-bounds.height));
    float y = bounds.y+12-toolScroll_[panel];
    const float x = bounds.x+12, width = std::max(0.0f,bounds.width-24);
    const auto text = color(layout.theme == "light" ? "#30353C" : "#D7DCE4");
    auto line = [&](std::string value) { renderList_.text({x,y,width,20},value,text,12,{},ui::TextAlign::Start,ui::TextOverflow::Ellipsis); y+=24; };
    auto button = [&](std::string key, std::string label, EditorCommand command, std::string target = {}, bool checked = false) {
        const auto region = "command:"+key;
        commandActions_[region] = {command,std::move(target)};
        draw_button({x,y,width,26},region,label,layout,checked); y+=32;
    };
    if (id == "settings") {
        line("Editor preferences");
        button("theme-dark","Dark",EditorCommand::SetDarkTheme,{},layout.theme=="dark");
        button("theme-light","Light",EditorCommand::SetLightTheme,{},layout.theme=="light");
        button("theme-contrast","High contrast",EditorCommand::SetHighContrastTheme,{},layout.theme=="high-contrast");
        button("docking",layout.allowDocking ? "Docking: On" : "Docking: Off",EditorCommand::ToggleDocking);
        for (const auto* scale : {"0.75","1.0","1.25","1.5"}) button(std::string("scale:")+scale,std::string("UI scale ")+scale,EditorCommand::SetUiScale,scale);
        button("save-layout","Save workspace & preferences",EditorCommand::SaveLayout);
        button("reload-layout","Reload workspace",EditorCommand::ReloadLayout);
        return;
    }
    if (id == "profiler") {
        line("UI CPU timing / idle p50 | p95 | max (ms)");
        const char* names[]{"Input","Model","Asset index","Layout","Paint","D2D raster","Upload","Composite","Present"};
        for (std::size_t i=0;i<9;++i) {
            const auto stats = ui::ui_performance().summary(ui::UiWorkload::Idle,static_cast<ui::UiStage>(i));
            std::ostringstream out; out << names[i] << "  " << std::fixed << std::setprecision(3) << stats.p50 << " | " << stats.p95 << " | " << stats.maximum;
            line(out.str());
        }
        line("Composite measures CPU submission; GPU time unavailable.");
        return;
    }
    if (id == "render-graph") {
        const auto caps = renderer.capabilities();
        line("Renderer capabilities");
        line(caps.deviceReady ? "Device: Ready" : "Device: Unavailable");
        line(caps.supportsNativeUi ? "Retained UI: supported" : "Retained UI: unsupported");
        line(caps.supportsEditorViewportScissor ? "Viewport scissor: supported" : "Viewport scissor: unsupported");
        const auto seam = renderer.editor_viewport();
        std::ostringstream out; out << "Viewport " << seam.viewport.width << " x " << seam.viewport.height << " px"; line(out.str());
        line(renderer.last_error().empty() ? "No renderer error" : renderer.last_error());
        return;
    }
    line("Simulation uses the Scene viewport.");
    button("game-play","Play / Stop",EditorCommand::Play);
    button("game-pause","Pause / Resume",EditorCommand::Pause);
    button("game-step","Advance one frame",EditorCommand::Step);
    line("A separate game render target is not available.");
}
} // namespace shinkou::editor

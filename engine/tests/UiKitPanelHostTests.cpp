#include "shinkou/editor/EditorLayer.h"
#include "shinkou/editor/UiKitPanelHost.h"

#include <cassert>

int main() {
    shinkou::editor::EditorUiModel model;
    shinkou::editor::EditorLayoutState layout;
    shinkou::render::Renderer renderer;
    shinkou::ui::MediaPanel media;
    shinkou::editor::UiKitPanelHost host;

    host.set_viewport(1600.0f, 900.0f, 1.25f);
    host.rebuild(model, {}, renderer, media, layout, {}, "ready", 1.0f / 60.0f);
    const auto fullStats = host.render_list().stats();
    assert(fullStats.commandCount > 0);
    assert(fullStats.textCount > 0);

    layout.showHierarchy = false;
    layout.showInspector = false;
    layout.showAssets = false;
    layout.showConsole = false;
    host.rebuild(model, {}, renderer, media, layout, {}, "ready", 1.0f / 60.0f);
    const auto compactStats = host.render_list().stats();
    assert(compactStats.commandCount > 0);
    assert(compactStats.commandCount < fullStats.commandCount);
    return 0;
}

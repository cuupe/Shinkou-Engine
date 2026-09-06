#include "shinkou/editor/AssetIconLibrary.h"

#include <cassert>
#include <iostream>
#include <filesystem>

int main() {
    shinkou::editor::AssetIconLibrary library;
#if defined(SHINKOU_EDITOR_ICON_SOURCE_DIR)
    const bool initialized = library.initialize(SHINKOU_EDITOR_ICON_SOURCE_DIR);
#else
    const bool initialized = library.initialize();
#endif
    assert(initialized);
    assert(library.ready());
    assert(library.loaded_count() == 11);

    shinkou::ui::UiRenderList renderList;
    const auto before = renderList.content_hash();
    const auto painted = library.paint(renderList, {0.0f, 0.0f, 48.0f, 48.0f},
                                       shinkou::editor::AssetIconKind::Folder,
                                       {0.85f, 0.6f, 0.15f, 1.0f},
                                       {1.0f, 1.0f, 1.0f, 1.0f},
                                       {0.9f, 0.92f, 0.96f, 1.0f});
    assert(painted);
    assert(renderList.content_hash() != before);
    assert(renderList.size() > 0);
    for (const auto& command : renderList.commands()) {
        assert(command.type == shinkou::ui::DrawCommandType::Path);
        assert(command.pathPoints && command.pathPoints->size() >= 2);
    }
    std::cout << "SVG asset icon library passed (" << library.loaded_count() << " icons)\n";
    return 0;
}

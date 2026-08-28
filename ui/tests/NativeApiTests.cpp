#include "shinkou/uikit/CAPI.h"
#include <cassert>
#include <string>
#include <vector>

int main() {
    shinkou_ui_editor_handle editor = shinkou_ui_editor_create(800, 600);
    assert(editor != nullptr);
    const std::uint64_t button = shinkou_ui_editor_add(editor, "button", 0);
    assert(button != 0);
    assert(shinkou_ui_editor_set_string(editor, button, "label", "运行"));
    assert(shinkou_ui_editor_set_bool(editor, button, "enabled", 1));
    assert(shinkou_ui_editor_serialize(editor, nullptr, 0) > 0);
    const std::size_t size = shinkou_ui_editor_serialize(editor, nullptr, 0);
    std::vector<char> buffer(size);
    shinkou_ui_editor_serialize(editor, buffer.data(), buffer.size());
    assert(std::string(buffer.data()).find("button") != std::string::npos);
    assert(shinkou_ui_editor_undo(editor) == 1);
    assert(shinkou_ui_editor_redo(editor) == 1);
    shinkou_ui_editor_destroy(editor);
    return 0;
}

#include "shinkou/uikit/CAPI.h"
#include "shinkou/uikit/Editor.h"
#include <algorithm>
#include <cstring>
#include <memory>

using shinkou::uikit::EditorDocument;

extern "C" {

shinkou_ui_editor_handle shinkou_ui_editor_create(float width, float height) {
    auto* editor = new EditorDocument();
    editor->ui().set_viewport({width, height});
    return editor;
}

void shinkou_ui_editor_destroy(shinkou_ui_editor_handle handle) {
    delete static_cast<EditorDocument*>(handle);
}

std::uint64_t shinkou_ui_editor_add(shinkou_ui_editor_handle handle, const char* type, std::uint64_t parent) {
    if (!handle || !type) return 0;
    return static_cast<EditorDocument*>(handle)->add(type, parent);
}

std::uint64_t shinkou_ui_editor_duplicate(shinkou_ui_editor_handle handle, std::uint64_t id) {
    return handle ? static_cast<EditorDocument*>(handle)->duplicate(id) : 0;
}

int shinkou_ui_editor_remove(shinkou_ui_editor_handle handle, std::uint64_t id) {
    return handle && static_cast<EditorDocument*>(handle)->remove(id) ? 1 : 0;
}

int shinkou_ui_editor_select(shinkou_ui_editor_handle handle, std::uint64_t id) {
    return handle && static_cast<EditorDocument*>(handle)->select(id) ? 1 : 0;
}

int shinkou_ui_editor_set_string(shinkou_ui_editor_handle handle, std::uint64_t id, const char* property, const char* value) {
    return handle && property && value && static_cast<EditorDocument*>(handle)->set_property(id, property, std::string(value)) ? 1 : 0;
}

int shinkou_ui_editor_set_float(shinkou_ui_editor_handle handle, std::uint64_t id, const char* property, float value) {
    return handle && property && static_cast<EditorDocument*>(handle)->set_property(id, property, value) ? 1 : 0;
}

int shinkou_ui_editor_set_bool(shinkou_ui_editor_handle handle, std::uint64_t id, const char* property, int value) {
    return handle && property && static_cast<EditorDocument*>(handle)->set_property(id, property, value != 0) ? 1 : 0;
}

int shinkou_ui_editor_undo(shinkou_ui_editor_handle handle) {
    return handle && static_cast<EditorDocument*>(handle)->undo() ? 1 : 0;
}

int shinkou_ui_editor_redo(shinkou_ui_editor_handle handle) {
    return handle && static_cast<EditorDocument*>(handle)->redo() ? 1 : 0;
}

int shinkou_ui_editor_load(shinkou_ui_editor_handle handle, const char* source) {
    return handle && source && static_cast<EditorDocument*>(handle)->load_xml(source) ? 1 : 0;
}

void shinkou_ui_editor_tick(shinkou_ui_editor_handle handle, float deltaSeconds) {
    if (handle) static_cast<EditorDocument*>(handle)->ui().tick(std::max(0.0f, deltaSeconds));
}

std::size_t shinkou_ui_editor_serialize(shinkou_ui_editor_handle handle, char* buffer, std::size_t capacity) {
    if (!handle) return 0;
    const std::string xml = static_cast<EditorDocument*>(handle)->serialize_xml();
    const std::size_t required = xml.size() + 1;
    if (buffer && capacity > 0) std::memcpy(buffer, xml.c_str(), std::min(required, capacity));
    return required;
}

}

#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#if defined(SHINKOU_UI_BUILD_SHARED)
#define SHINKOU_UI_API __declspec(dllexport)
#else
#define SHINKOU_UI_API __declspec(dllimport)
#endif
#else
#define SHINKOU_UI_API
#endif

extern "C" {

using shinkou_ui_editor_handle = void*;

SHINKOU_UI_API shinkou_ui_editor_handle shinkou_ui_editor_create(float width, float height);
SHINKOU_UI_API void shinkou_ui_editor_destroy(shinkou_ui_editor_handle handle);
SHINKOU_UI_API std::uint64_t shinkou_ui_editor_add(shinkou_ui_editor_handle handle, const char* type, std::uint64_t parent);
SHINKOU_UI_API std::uint64_t shinkou_ui_editor_duplicate(shinkou_ui_editor_handle handle, std::uint64_t id);
SHINKOU_UI_API int shinkou_ui_editor_remove(shinkou_ui_editor_handle handle, std::uint64_t id);
SHINKOU_UI_API int shinkou_ui_editor_select(shinkou_ui_editor_handle handle, std::uint64_t id);
SHINKOU_UI_API int shinkou_ui_editor_set_string(shinkou_ui_editor_handle handle, std::uint64_t id, const char* property, const char* value);
SHINKOU_UI_API int shinkou_ui_editor_set_float(shinkou_ui_editor_handle handle, std::uint64_t id, const char* property, float value);
SHINKOU_UI_API int shinkou_ui_editor_set_bool(shinkou_ui_editor_handle handle, std::uint64_t id, const char* property, int value);
SHINKOU_UI_API int shinkou_ui_editor_undo(shinkou_ui_editor_handle handle);
SHINKOU_UI_API int shinkou_ui_editor_redo(shinkou_ui_editor_handle handle);
SHINKOU_UI_API int shinkou_ui_editor_load(shinkou_ui_editor_handle handle, const char* source);
SHINKOU_UI_API void shinkou_ui_editor_tick(shinkou_ui_editor_handle handle, float deltaSeconds);
SHINKOU_UI_API std::size_t shinkou_ui_editor_serialize(shinkou_ui_editor_handle handle, char* buffer, std::size_t capacity);

}

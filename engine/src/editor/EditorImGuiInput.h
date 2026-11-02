#pragma once
#include "shinkou/editor/EditorActions.h"
namespace shinkou::editor {
EditorInputFrame collect_editor_input();
int editor_imgui_key(EditorKey) noexcept;
} // namespace shinkou::editor

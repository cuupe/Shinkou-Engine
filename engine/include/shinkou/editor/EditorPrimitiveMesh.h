#pragma once
#include "shinkou/components/PrimitiveMeshComponent.h"
#include "shinkou/editor/EditorModelSceneRenderer.h"

namespace shinkou::editor {
// Shared immutable CPU geometry; each viewport renderer owns its GPU cache.
std::shared_ptr<const EditorModelPreviewSnapshot> editor_primitive_mesh(std::string_view shape);
void append_editor_primitives(const World &world, std::vector<EditorModelSceneInstance> &instances);
} // namespace shinkou::editor

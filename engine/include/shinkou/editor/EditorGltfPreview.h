#pragma once

#include "shinkou/editor/EditorModelPreview.h"

namespace shinkou::editor {

// Bounded glTF 2.0 provider seam. The current implementation accepts JSON
// glTF with project-contained buffers and GLB with one embedded BIN chunk. It
// publishes the same immutable mesh snapshot as OBJ and bounded image
// artifacts, so the preview scene and retained UI do not care which container
// supplied the geometry.
EditorModelPreviewResult load_editor_gltf_preview(
    const FileSystemService& files,
    std::string_view relativePath,
    std::uint64_t generation,
    std::uint64_t sourceStamp,
    const std::atomic_bool* cancel = nullptr);

} // namespace shinkou::editor

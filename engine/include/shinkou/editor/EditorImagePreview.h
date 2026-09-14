#pragma once

#include "shinkou/editor/FileSystem.h"
#include "shinkou/ui/Render.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::editor {

struct EditorImagePreviewResult {
    std::uint64_t generation{0};
    std::uint64_t sourceStamp{0};
    std::string path;
    std::uint32_t sourceWidth{0};
    std::uint32_t sourceHeight{0};
    std::shared_ptr<const ui::UiImageSnapshot> snapshot{};
    std::string error;
};

// Decodes only a bounded thumbnail. The provider owns no UI state and may be
// called from a worker thread. Unsupported platforms/providers return an
// explicit error instead of silently presenting a fake image.
EditorImagePreviewResult load_editor_image_preview(const FileSystemService& files,
                                                   std::string_view relativePath,
                                                   std::uint64_t generation,
                                                   std::uint64_t sourceStamp,
                                                   std::uint32_t maxDimension = 512);

// AssetSystem can provide immutable typed image bytes without forcing the
// preview worker to reopen the project file. The WIC decode limits are shared
// with the path-based provider.
EditorImagePreviewResult load_editor_image_preview_bytes(
    std::string_view relativePath,
    const std::vector<std::uint8_t>& sourceBytes,
    std::uint64_t generation,
    std::uint64_t sourceStamp,
    std::uint32_t maxDimension = 512);

} // namespace shinkou::editor

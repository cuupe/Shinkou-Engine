#pragma once

#include "shinkou/editor/FileSystem.h"
#include "shinkou/ui/Render.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace shinkou::editor {

struct EditorVideoPreviewSnapshot {
    std::uint64_t revision{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    double duration{0.0};
    double frameRate{0.0};
    double frameTime{0.0};
    std::uint32_t audioStreamCount{0};
    std::shared_ptr<const ui::UiImageSnapshot> firstFrame{};

    bool valid() const noexcept {
        return revision != 0 && width > 0 && height > 0 && duration >= 0.0 &&
            firstFrame && firstFrame->valid();
    }
};

struct EditorVideoPreviewResult {
    std::uint64_t generation{0};
    std::uint64_t sourceStamp{0};
    std::string path;
    std::shared_ptr<const EditorVideoPreviewSnapshot> snapshot{};
    std::string error;
};

// Extracts bounded video metadata and one decoded first-frame snapshot on
// platforms with a registered provider. The worker never exposes decoder
// handles to retained UI and may be cancelled during resource switches.
EditorVideoPreviewResult load_editor_video_preview(
    const FileSystemService& files,
    std::string_view relativePath,
    std::uint64_t generation,
    std::uint64_t sourceStamp,
    std::uint32_t maxDimension = 512,
    const std::atomic_bool* cancel = nullptr);

// Decodes one bounded frame at a requested presentation time. Metadata is
// republished with the frame so a seek result can replace the retained image
// without exposing Media Foundation handles to the editor UI.
EditorVideoPreviewResult load_editor_video_frame(
    const FileSystemService& files,
    std::string_view relativePath,
    std::uint64_t generation,
    std::uint64_t sourceStamp,
    double seconds,
    std::uint32_t maxDimension = 512,
    const std::atomic_bool* cancel = nullptr);

} // namespace shinkou::editor

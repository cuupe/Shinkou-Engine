#pragma once

#include "shinkou/editor/FileSystem.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::editor {

struct EditorAudioPreviewSnapshot {
    std::uint64_t revision{0};
    std::uint32_t channels{0};
    std::uint32_t sampleRate{0};
    std::uint64_t totalFrames{0};
    double duration{0.0};
    // One normalized absolute peak per bounded time bin.
    std::vector<float> peaks;

    bool valid() const noexcept {
        return revision != 0 && channels > 0 && sampleRate > 0 &&
            totalFrames > 0 && duration > 0.0 && !peaks.empty();
    }
};

struct EditorAudioPreviewResult {
    std::uint64_t generation{0};
    std::uint64_t sourceStamp{0};
    std::string path;
    std::shared_ptr<const EditorAudioPreviewSnapshot> snapshot{};
    std::string error;
};

// Reads decoder metadata and a bounded set of peak samples on a worker
// thread. It never materializes the full PCM stream and reports unsupported
// or malformed files explicitly.
EditorAudioPreviewResult load_editor_audio_preview(const FileSystemService& files,
                                                   std::string_view relativePath,
                                                   std::uint64_t generation,
                                                   std::uint64_t sourceStamp,
                                                   std::uint32_t peakCount = 256,
                                                   const std::atomic_bool* cancel = nullptr);

} // namespace shinkou::editor

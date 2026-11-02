#pragma once
#include "shinkou/editor/FileSystem.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace shinkou::editor {
struct EditorTextPreviewSnapshot {
    std::vector<std::string> lines;
    // 0: paragraph/source, 1..9: Word heading level, 10: table row.
    std::vector<std::uint8_t> styles;
    std::string format;
    std::string encoding;
    bool truncated{false};
};
struct EditorTextPreviewResult {
    std::uint64_t generation{0}, sourceStamp{0};
    std::string path;
    std::shared_ptr<const EditorTextPreviewSnapshot> snapshot;
    std::string error;
};
// Bounded, read-only. Scripts are never executed; Word packages are never
// extracted to disk and external relationships/macros are not followed.
EditorTextPreviewResult load_editor_text_preview(const FileSystemService &, std::string_view path,
                                                 std::uint64_t generation, std::uint64_t stamp);
} // namespace shinkou::editor

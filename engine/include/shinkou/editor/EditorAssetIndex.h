#pragma once

#include "shinkou/editor/AssetPreview.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shinkou::editor {

struct EditorAssetIndexEntry {
    AssetPreviewDescriptor descriptor;
    std::uint64_t writeStamp{0};
};

// Immutable after construction. The retained UI, preview workers and future
// import providers can safely share this snapshot without touching the file
// system or coordinating per-entry mutation.
class EditorAssetIndexSnapshot final {
public:
    std::uint64_t revision() const noexcept { return revision_; }
    std::size_t size() const noexcept { return entries_.size(); }
    const std::vector<EditorAssetIndexEntry>& entries() const noexcept { return entries_; }
    const EditorAssetIndexEntry* find(std::string_view relativePath) const;

private:
    friend class EditorAssetIndex;
    std::uint64_t revision_{0};
    std::vector<EditorAssetIndexEntry> entries_;
    std::unordered_map<std::string, std::size_t> lookup_;
};

class EditorAssetIndex final {
public:
    static std::shared_ptr<const EditorAssetIndexSnapshot> build(
        const std::vector<FileEntry>& entries, std::uint64_t revision);
};

} // namespace shinkou::editor

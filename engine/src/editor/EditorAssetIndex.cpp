#include "shinkou/editor/EditorAssetIndex.h"

namespace shinkou::editor {

const EditorAssetIndexEntry* EditorAssetIndexSnapshot::find(std::string_view relativePath) const {
    const auto found = lookup_.find(std::string(relativePath));
    return found == lookup_.end() ? nullptr : &entries_[found->second];
}

std::shared_ptr<const EditorAssetIndexSnapshot> EditorAssetIndex::build(
    const std::vector<FileEntry>& entries, std::uint64_t revision) {
    auto snapshot = std::make_shared<EditorAssetIndexSnapshot>();
    snapshot->revision_ = revision;
    snapshot->entries_.reserve(entries.size());
    snapshot->lookup_.reserve(entries.size());
    AssetPreviewCatalog catalog;
    for (const auto& entry : entries) {
        EditorAssetIndexEntry indexed;
        indexed.descriptor = catalog.describe(entry);
        indexed.writeStamp = entry.writeStamp;
        const auto key = indexed.descriptor.relativePath.generic_string();
        const auto index = snapshot->entries_.size();
        snapshot->entries_.push_back(std::move(indexed));
        snapshot->lookup_.emplace(key, index);
    }
    return snapshot;
}

} // namespace shinkou::editor

#include "shinkou/editor/EditorAssetBrowser.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>

namespace shinkou::editor {
namespace {
std::string search_key(std::string text) {
    for (auto &character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}
std::string directory_key(const std::filesystem::path &path) {
    const auto key = path.lexically_normal().generic_u8string();
    return key == "." ? std::string{} : key;
}
} // namespace

void EditorAssetBrowser::refresh(const std::vector<FileEntry> &entries, std::uint64_t revision) {
    const auto scope = directory_key(directory);
    if (sourceRevision_ == revision && cachedDirectory_ == scope && cachedFilter_ == filter &&
        cachedView_ == view && cachedExpansion_ == expansionRevision_)
        return;
    sourceRevision_ = revision;
    cachedDirectory_ = scope;
    cachedFilter_ = filter;
    cachedView_ = view;
    cachedExpansion_ = expansionRevision_;
    rows_.clear();
    const auto needle = search_key(filter);
    std::unordered_map<std::string, std::vector<std::size_t>> children;
    std::vector<std::string> sortKeys;
    sortKeys.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto &entry = entries[index];
        sortKeys.push_back(search_key(entry.name));
        const auto path = directory_key(entry.relativePath);
        const bool inScope = scope.empty() || path.compare(0, scope.size() + 1, scope + "/") == 0;
        if (!inScope)
            continue;
        if (!needle.empty()) {
            if (search_key(path).find(needle) != std::string::npos)
                rows_.push_back({index, 0});
        } else
            children[directory_key(entry.relativePath.parent_path())].push_back(index);
    }
    const auto less = [&](std::size_t left, std::size_t right) {
        if (entries[left].directory != entries[right].directory)
            return entries[left].directory;
        if (sortKeys[left] != sortKeys[right])
            return sortKeys[left] < sortKeys[right];
        return entries[left].relativePath.generic_u8string() <
               entries[right].relativePath.generic_u8string();
    };
    if (!needle.empty()) {
        std::stable_sort(rows_.begin(), rows_.end(), [&](const auto &a, const auto &b) {
            return less(a.sourceIndex, b.sourceIndex);
        });
        return;
    }
    for (auto &[parent, indices] : children) {
        (void)parent;
        std::stable_sort(indices.begin(), indices.end(), less);
    }
    // Project paths are bounded by the file scan. A depth cap also protects
    // the view against adversarial snapshots that were not produced by it.
    const std::function<void(const std::string &, std::size_t)> append =
        [&](const std::string &parent, std::size_t depth) {
            const auto found = children.find(parent);
            if (found == children.end() || depth > 64)
                return;
            for (const auto index : found->second) {
                rows_.push_back({index, depth});
                const auto path = directory_key(entries[index].relativePath);
                if (view == EditorAssetView::Tree && entries[index].directory && expanded(path))
                    append(path, depth + 1);
            }
        };
    append(scope, 0);
}
void EditorAssetBrowser::toggle_directory(std::string_view path) {
    const std::string key(path);
    if (!expanded_.erase(key))
        expanded_.insert(key);
    ++expansionRevision_;
}
bool EditorAssetBrowser::expanded(std::string_view path) const {
    return expanded_.count(std::string(path)) != 0;
}
void EditorAssetBrowser::reset() {
    *this = EditorAssetBrowser{};
}
} // namespace shinkou::editor

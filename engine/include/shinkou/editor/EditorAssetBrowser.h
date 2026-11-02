#pragma once

#include "shinkou/editor/EditorPresentation.h"
#include "shinkou/editor/FileSystem.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace shinkou::editor {
struct EditorAssetBrowserRow {
    std::size_t sourceIndex{};
    std::size_t depth{};
};

// One owner for resource navigation and filtering, independent of widgets.
// The view consumes this bounded snapshot; filesystem scanning stays in the
// editor controller and no file I/O occurs while drawing a browser row.
class EditorAssetBrowser final {
    std::vector<EditorAssetBrowserRow> rows_;
    std::unordered_set<std::string> expanded_;
    std::uint64_t sourceRevision_{~std::uint64_t{0}};
    std::uint64_t expansionRevision_{0}, cachedExpansion_{0};
    std::string cachedDirectory_, cachedFilter_;
    EditorAssetView cachedView_{EditorAssetView::Tree};

  public:
    std::filesystem::path directory;
    std::string selection;
    std::string filter;
    EditorAssetView view{EditorAssetView::Tree};

    void refresh(const std::vector<FileEntry> &entries, std::uint64_t revision);
    void toggle_directory(std::string_view path);
    bool expanded(std::string_view path) const;
    void reset();
    const std::vector<EditorAssetBrowserRow> &rows() const noexcept {
        return rows_;
    }
};
} // namespace shinkou::editor

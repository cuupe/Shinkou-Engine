#pragma once
#include "shinkou/editor/EditorUiModel.h"
#include <unordered_set>

namespace shinkou::editor {
struct EditorHierarchyRow {
    const EditorObjectTreeNode *node{};
    std::size_t depth{};
};

// Flatten expanded nodes once per hierarchy/filter/expansion change. Selection,
// hover and scrolling leave this snapshot intact; ImGui clips its visible range.
class EditorHierarchy final {
    std::vector<EditorHierarchyRow> rows_;
    std::unordered_set<ObjectId> collapsed_;
    std::uint64_t revision_{~std::uint64_t{0}}, rebuilds_{0};
    bool dirty_{true}, filtering_{false};

  public:
    void refresh(const std::vector<EditorObjectTreeNode> &roots, std::uint64_t revision,
                 bool filtering);
    void set_expanded(ObjectId id, bool expanded);
    bool expanded(ObjectId id) const noexcept {
        return filtering_ || collapsed_.find(id) == collapsed_.end();
    }
    const auto &rows() const noexcept {
        return rows_;
    }
    std::uint64_t rebuilds() const noexcept {
        return rebuilds_;
    }
};
} // namespace shinkou::editor

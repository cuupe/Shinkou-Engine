#include "shinkou/editor/EditorHierarchy.h"

namespace shinkou::editor {
void EditorHierarchy::refresh(const std::vector<EditorObjectTreeNode> &roots,
                              std::uint64_t revision, bool filtering) {
    if (!dirty_ && revision_ == revision && filtering_ == filtering)
        return;
    revision_ = revision;
    filtering_ = filtering;
    dirty_ = false;
    ++rebuilds_;
    rows_.clear();
    // Iterative preorder avoids a second recursive traversal on every frame.
    std::vector<EditorHierarchyRow> pending;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it)
        pending.push_back({&*it, 0});
    while (!pending.empty()) {
        const auto row = pending.back();
        pending.pop_back();
        rows_.push_back(row);
        if (!expanded(row.node->id))
            continue;
        for (auto it = row.node->children.rbegin(); it != row.node->children.rend(); ++it)
            pending.push_back({&*it, row.depth + 1});
    }
}
void EditorHierarchy::set_expanded(ObjectId id, bool expanded) {
    if (filtering_)
        return; // A search keeps matching ancestors visible.
    if (expanded ? collapsed_.erase(id) != 0 : collapsed_.insert(id).second)
        dirty_ = true;
}
} // namespace shinkou::editor

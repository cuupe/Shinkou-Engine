#include "shinkou/editor/EditorSelection.h"

#include <algorithm>

namespace shinkou::editor {

std::vector<ObjectId> EditorSelection::normalize(std::vector<ObjectId> ids) {
    ids.erase(std::remove(ids.begin(), ids.end(), ObjectId{0}), ids.end());
    std::vector<ObjectId> result;
    result.reserve(ids.size());
    for (const auto id : ids) {
        if (std::find(result.begin(), result.end(), id) == result.end()) result.push_back(id);
    }
    return result;
}

ObjectId EditorSelection::fallback_primary(const std::vector<ObjectId>& ids, ObjectId requested) noexcept {
    if (requested != 0 && std::find(ids.begin(), ids.end(), requested) != ids.end()) return requested;
    return ids.empty() ? 0 : ids.back();
}

bool EditorSelection::commit(EditorSelectionChangeKind kind, std::vector<ObjectId> next, ObjectId primary) {
    next = normalize(std::move(next));
    primary = fallback_primary(next, primary);
    if (selected_ == next && primary_ == primary) return false;

    EditorSelectionChange change;
    change.kind = kind;
    change.before = selected_;
    change.primaryBefore = primary_;
    selected_ = std::move(next);
    primary_ = primary;
    ++revision_;
    change.after = selected_;
    change.primaryAfter = primary_;
    change.revision = revision_;
    if (changeCallback_) changeCallback_(change);
    return true;
}

bool EditorSelection::select(ObjectId id, EditorSelectionMode mode) {
    if (id == 0) return false;
    return select(std::vector<ObjectId>{id}, mode);
}

bool EditorSelection::select(const std::vector<ObjectId>& ids, EditorSelectionMode mode) {
    const auto incoming = normalize(ids);
    if (mode == EditorSelectionMode::Replace) {
        return commit(EditorSelectionChangeKind::Replace, incoming, incoming.empty() ? 0 : incoming.back());
    }

    auto next = selected_;
    ObjectId requestedPrimary = primary_;
    if (mode == EditorSelectionMode::Add) {
        for (const auto id : incoming) {
            if (std::find(next.begin(), next.end(), id) == next.end()) {
                next.push_back(id);
                requestedPrimary = id;
            }
        }
        return commit(EditorSelectionChangeKind::Add, std::move(next), requestedPrimary);
    }

    if (mode == EditorSelectionMode::Remove) {
        next.erase(std::remove_if(next.begin(), next.end(), [&incoming](ObjectId id) {
            return std::find(incoming.begin(), incoming.end(), id) != incoming.end();
        }), next.end());
        return commit(EditorSelectionChangeKind::Remove, std::move(next), primary_);
    }

    for (const auto id : incoming) {
        const auto it = std::find(next.begin(), next.end(), id);
        if (it == next.end()) {
            next.push_back(id);
            requestedPrimary = id;
        } else {
            if (id == requestedPrimary) requestedPrimary = 0;
            next.erase(it);
        }
    }
    return commit(EditorSelectionChangeKind::Toggle, std::move(next), requestedPrimary);
}

bool EditorSelection::set(std::vector<ObjectId> ids, ObjectId primary) {
    return commit(EditorSelectionChangeKind::Replace, std::move(ids), primary);
}

bool EditorSelection::set_primary(ObjectId id) {
    if (id == 0 || !contains(id)) return false;
    return commit(EditorSelectionChangeKind::SetPrimary, selected_, id);
}

bool EditorSelection::remove(ObjectId id) {
    return id != 0 && select(std::vector<ObjectId>{id}, EditorSelectionMode::Remove);
}

bool EditorSelection::clear() {
    return commit(EditorSelectionChangeKind::Clear, {}, 0);
}

bool EditorSelection::contains(ObjectId id) const noexcept {
    return id != 0 && std::find(selected_.begin(), selected_.end(), id) != selected_.end();
}

} // namespace shinkou::editor

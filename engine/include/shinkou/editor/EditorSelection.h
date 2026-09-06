#pragma once

#include "shinkou/Types.h"

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace shinkou::editor {

enum class EditorSelectionMode : std::uint8_t {
    Replace,
    Add,
    Toggle,
    Remove,
};

enum class EditorSelectionChangeKind : std::uint8_t {
    Replace,
    Add,
    Toggle,
    Remove,
    Clear,
    SetPrimary,
};

struct EditorSelectionChange {
    EditorSelectionChangeKind kind{EditorSelectionChangeKind::Replace};
    std::vector<ObjectId> before;
    std::vector<ObjectId> after;
    ObjectId primaryBefore{0};
    ObjectId primaryAfter{0};
    std::uint64_t revision{0};
};

class EditorSelection {
public:
    using ChangeCallback = std::function<void(const EditorSelectionChange&)>;

    bool select(ObjectId id, EditorSelectionMode mode = EditorSelectionMode::Replace);
    bool select(const std::vector<ObjectId>& ids, EditorSelectionMode mode = EditorSelectionMode::Replace);
    bool set(std::vector<ObjectId> ids, ObjectId primary = 0);
    bool set_primary(ObjectId id);
    bool remove(ObjectId id);
    bool clear();

    bool contains(ObjectId id) const noexcept;
    bool empty() const noexcept { return selected_.empty(); }
    std::size_t size() const noexcept { return selected_.size(); }
    ObjectId primary() const noexcept { return primary_; }
    const std::vector<ObjectId>& ids() const noexcept { return selected_; }
    std::uint64_t revision() const noexcept { return revision_; }

    void set_change_callback(ChangeCallback callback) { changeCallback_ = std::move(callback); }

private:
    std::vector<ObjectId> selected_;
    ObjectId primary_{0};
    std::uint64_t revision_{0};
    ChangeCallback changeCallback_;

    static std::vector<ObjectId> normalize(std::vector<ObjectId> ids);
    bool commit(EditorSelectionChangeKind kind, std::vector<ObjectId> next, ObjectId primary);
    static ObjectId fallback_primary(const std::vector<ObjectId>& ids, ObjectId requested) noexcept;
};

} // namespace shinkou::editor

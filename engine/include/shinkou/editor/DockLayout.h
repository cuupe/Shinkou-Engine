#pragma once

#include "shinkou/Math.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shinkou::editor {

struct DockRect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};

    bool contains(math::Vec2 point) const noexcept {
        return point.x >= x && point.y >= y && point.x <= x + width && point.y <= y + height;
    }
};

enum class DockSplitOrientation : std::uint8_t { Horizontal, Vertical };

struct DockPanel {
    std::string id;
    std::string title;
    bool visible{true};
    bool closeable{true};
    math::Vec2 minSize{0.0f, 0.0f};
};

// Horizontal splits divide the X axis (left/right); vertical splits divide the Y axis (top/bottom).
struct DockNode {
    enum class Kind : std::uint8_t { Split, TabStack, Leaf, Floating };

    Kind kind{Kind::Leaf};
    DockSplitOrientation orientation{DockSplitOrientation::Horizontal};
    float ratio{0.5f};
    std::size_t activeTab{0};
    std::vector<DockNode> children;
    std::vector<DockPanel> tabs;

    // Leaf data. A leaf has one panel, while a tab stack has the panels in tabs.
    std::string panelId;
    std::string title;
    bool visible{true};
    bool closeable{true};
    math::Vec2 minSize{0.0f, 0.0f};

    // Floating data. A floating node owns exactly one child and is laid out in bounds.
    DockRect bounds{};

    static DockNode leaf(DockPanel panel);
    static DockNode tab_stack(std::vector<DockPanel> panels = {}, std::size_t active = 0);
    static DockNode split(DockNode first, DockNode second, DockSplitOrientation direction = DockSplitOrientation::Horizontal,
                          float splitRatio = 0.5f);
    static DockNode floating(DockNode child, DockRect rectangle);

    bool is_leaf() const noexcept { return kind == Kind::Leaf; }
    bool is_tab_stack() const noexcept { return kind == Kind::TabStack; }
    bool is_split() const noexcept { return kind == Kind::Split; }
    bool is_floating() const noexcept { return kind == Kind::Floating; }
};

using DockNodeKind = DockNode::Kind;
using DockTab = DockPanel;

struct DockSplitHit {
    const DockNode* node{nullptr};
    DockRect bar{};
    DockSplitOrientation orientation{DockSplitOrientation::Horizontal};
    float ratio{0.5f};
};

enum class DockSerializationError : std::uint8_t {
    None,
    InvalidJson,
    TypeMismatch,
    MissingField,
    RangeError,
    InvalidTree,
    UnsupportedVersion,
};

struct DockSerializationResult {
    DockSerializationError error{DockSerializationError::None};
    std::size_t offset{0};
    std::string message;

    operator bool() const noexcept { return error == DockSerializationError::None; }
};

class DockWorkspace {
public:
    static constexpr std::uint32_t CurrentVersion = 1;
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    DockNode root_{};
    std::vector<DockNode> floating_;

public:
    DockWorkspace() = default;
    explicit DockWorkspace(DockNode root) : root_(std::move(root)) { normalize(); }

    DockNode& root() noexcept { return root_; }
    const DockNode& root() const noexcept { return root_; }
    void set_root(DockNode root) { root_ = std::move(root); normalize(); }

    std::vector<DockNode>& floating() noexcept { return floating_; }
    const std::vector<DockNode>& floating() const noexcept { return floating_; }
    bool add_floating(DockNode node);

    bool add_tab(std::string_view targetPanelId, DockPanel panel, bool activate = true);
    bool remove_tab(std::string_view panelId);
    bool move_tab(std::string_view panelId, std::string_view targetPanelId, std::size_t index = npos);
    bool split(std::string_view targetPanelId, DockSplitOrientation direction, float ratio, DockPanel newPanel);
    bool split(std::string_view targetPanelId, DockSplitOrientation direction, float ratio, DockNode newNode);
    bool activate_tab(std::string_view panelId);
    bool set_panel_visibility(std::string_view panelId, bool visible);
    bool set_minimum_size(std::string_view panelId, math::Vec2 size);

    math::Vec2 minimum_size() const noexcept;
    static math::Vec2 minimum_size(const DockNode& node) noexcept;

    // Repairs empty/collapsible nodes and clamps split ratios. rebalance then applies minimum sizes
    // to a concrete viewport while preserving the serialized topology.
    void normalize();
    void rebalance(math::Vec2 availableSize, float splitterThickness = 4.0f);

    std::optional<DockSplitHit> hit_test_split_bar(DockRect area, math::Vec2 point,
                                                    float splitterThickness = 6.0f) const noexcept;

    std::string to_json(bool pretty = false) const;
    DockSerializationResult from_json(std::string_view json);
};

using DockLayout = DockWorkspace;

std::string serialize_json(const DockWorkspace& workspace, bool pretty = false);
DockSerializationResult deserialize_json(std::string_view json, DockWorkspace& workspace);
std::string serialize_dock_workspace(const DockWorkspace& workspace, bool pretty = false);
DockSerializationResult deserialize_dock_workspace(std::string_view json, DockWorkspace& workspace);

} // namespace shinkou::editor

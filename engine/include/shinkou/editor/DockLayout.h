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

    bool is_empty() const noexcept { return width <= 0.0f || height <= 0.0f; }

    // Dock geometry uses half-open rectangles. This keeps adjacent panels and
    // splitter hit zones deterministic when a pointer lies exactly on a
    // boundary.
    bool contains(math::Vec2 point) const noexcept {
        return width > 0.0f && height > 0.0f && point.x >= x && point.y >= y && point.x < x + width && point.y < y + height;
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

struct DockLayoutOptions {
    float splitterThickness{4.0f};
    float splitterHitSlop{4.0f};
    float tabBarHeight{26.0f};
    float tabWidth{128.0f};
    float minTabWidth{64.0f};
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

inline constexpr std::size_t kDockNoIndex = static_cast<std::size_t>(-1);

// A stable, pointer-free address for a node in the workspace tree. The root
// tree uses childIndices from root_ downward. Floating trees set floating and
// identify the floating root with floatingIndex before following children.
struct DockNodePath {
    bool floating{false};
    std::size_t floatingIndex{kDockNoIndex};
    std::vector<std::size_t> childIndices;

    bool operator==(const DockNodePath& other) const noexcept {
        return floating == other.floating && floatingIndex == other.floatingIndex && childIndices == other.childIndices;
    }
    bool operator!=(const DockNodePath& other) const noexcept { return !(*this == other); }
};

struct DockPanelLayout {
    std::string panelId;
    std::string title;
    DockRect rect{};
    DockNodePath nodePath{};
    std::size_t tabIndex{kDockNoIndex};
    bool visible{true};
    bool active{true};
    bool renderable{true};
    bool floating{false};
};

struct DockTabLayout {
    std::string panelId;
    std::string title;
    DockRect rect{};
    DockNodePath stackPath{};
    std::size_t tabIndex{kDockNoIndex};
    bool visible{true};
    bool active{false};
    bool closeable{true};
};

struct DockSplitterLayout {
    DockNodePath path{};
    DockRect parentRect{};
    DockRect bar{};
    DockRect hitZone{};
    DockSplitOrientation orientation{DockSplitOrientation::Horizontal};
    float ratio{0.5f};
    float minRatio{0.05f};
    float maxRatio{0.95f};
    bool minimumSizeExceeded{false};
};

struct DockTabHit {
    DockNodePath stackPath{};
    std::string panelId;
    DockRect tabRect{};
    std::size_t tabIndex{kDockNoIndex};
};

struct DockLayoutResult {
    DockRect area{};
    std::vector<DockPanelLayout> panels;
    std::vector<DockTabLayout> tabs;
    std::vector<DockSplitterLayout> splitters;
    bool minimumSizeExceeded{false};
};

struct DockSplitHit {
    const DockNode* node{nullptr};
    DockNodePath path{};
    DockRect bar{};
    DockRect hitZone{};
    DockRect parentRect{};
    DockSplitOrientation orientation{DockSplitOrientation::Horizontal};
    float ratio{0.5f};
    float minRatio{0.05f};
    float maxRatio{0.95f};
    bool minimumSizeExceeded{false};
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
    static constexpr std::size_t npos = kDockNoIndex;

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
    // Moves a docked panel into a single-panel floating node. Bounds use the
    // same logical client coordinates as layout().
    bool float_panel(std::string_view panelId, DockRect bounds);
    // Moves an existing floating node without changing its child topology.
    bool move_floating(std::string_view panelId, DockRect bounds);

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

    // Computes all derived geometry without mutating the serialized tree. A
    // result contains one panel entry per leaf/tab, one tab hit rect per tab,
    // and one splitter entry per split node.
    DockLayoutResult layout(DockRect area, const DockLayoutOptions& options = {}) const;

    // Repairs empty/collapsible nodes and clamps split ratios. rebalance then applies minimum sizes
    // to a concrete viewport while preserving the serialized topology.
    void normalize();
    void rebalance(math::Vec2 availableSize, float splitterThickness = 4.0f,
                   const DockLayoutOptions& options = {});

    // Pointer-free splitter mutation APIs for a drag session. Delta is in
    // parent-rect pixels along the splitter axis; pointer uses parent client
    // coordinates. Both clamp to the feasible ratio range exposed by layout.
    bool set_splitter_ratio(const DockNodePath& path, float ratio);
    bool drag_splitter(const DockNodePath& path, float delta, DockRect area,
                       const DockLayoutOptions& options = {});
    bool drag_splitter(const DockNodePath& path, math::Vec2 pointer, DockRect area,
                       const DockLayoutOptions& options = {});

    bool activate_tab(const DockNodePath& stackPath, std::size_t tabIndex);
    std::optional<DockTabHit> hit_test_tab(DockRect area, math::Vec2 point,
                                           const DockLayoutOptions& options = {}) const;

    std::optional<DockSplitHit> hit_test_splitter(DockRect area, math::Vec2 point,
                                                   const DockLayoutOptions& options = {}) const;

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

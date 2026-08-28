#include "shinkou/editor/DockLayout.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace shinkou::editor {
namespace {

constexpr float kMinRatio = 0.05f;
constexpr float kMaxRatio = 0.95f;
constexpr std::size_t kMaxJsonDepth = 128;

bool finite_non_negative(float value) noexcept { return std::isfinite(value) != 0 && value >= 0.0f; }
bool valid_ratio(float value) noexcept { return std::isfinite(value) != 0 && value >= 0.0f && value <= 1.0f; }

DockPanel panel_from_leaf(const DockNode& node) {
    return {node.panelId, node.title, node.visible, node.closeable, node.minSize};
}

bool panel_id_in(const DockNode& node, std::string_view id) {
    if (node.kind == DockNode::Kind::Leaf && node.panelId == id) return true;
    if (node.kind == DockNode::Kind::TabStack && std::any_of(node.tabs.begin(), node.tabs.end(), [id](const auto& tab) {
            return tab.id == id;
        })) return true;
    return std::any_of(node.children.begin(), node.children.end(), [id](const auto& child) {
        return panel_id_in(child, id);
    });
}

bool has_any_panel(const DockNode& node) {
    if (node.kind == DockNode::Kind::Leaf) return !node.panelId.empty();
    if (node.kind == DockNode::Kind::TabStack) return !node.tabs.empty();
    return std::any_of(node.children.begin(), node.children.end(), [](const auto& child) { return has_any_panel(child); });
}

bool has_any_panel(const DockWorkspace& workspace) {
    if (has_any_panel(workspace.root())) return true;
    return std::any_of(workspace.floating().begin(), workspace.floating().end(), [](const auto& node) {
        return has_any_panel(node);
    });
}

struct PanelRef {
    DockNode* node{nullptr};
    std::size_t tabIndex{DockWorkspace::npos};
};

PanelRef find_panel(DockNode& node, std::string_view id) {
    if (node.kind == DockNode::Kind::Leaf && node.panelId == id) return {&node, DockWorkspace::npos};
    if (node.kind == DockNode::Kind::TabStack) {
        for (std::size_t i = 0; i < node.tabs.size(); ++i) if (node.tabs[i].id == id) return {&node, i};
    }
    for (auto& child : node.children) {
        if (auto found = find_panel(child, id); found.node != nullptr) return found;
    }
    return {};
}

PanelRef find_panel(DockWorkspace& workspace, std::string_view id) {
    if (auto found = find_panel(workspace.root(), id); found.node != nullptr) return found;
    for (auto& node : workspace.floating()) {
        if (auto found = find_panel(node, id); found.node != nullptr) return found;
    }
    return {};
}

bool collect_ids(const DockNode& node, std::vector<std::string>& ids) {
    if (node.kind == DockNode::Kind::Leaf) {
        if (node.panelId.empty() || std::find(ids.begin(), ids.end(), node.panelId) != ids.end()) return false;
        ids.push_back(node.panelId);
    } else if (node.kind == DockNode::Kind::TabStack) {
        for (const auto& tab : node.tabs) {
            if (tab.id.empty() || std::find(ids.begin(), ids.end(), tab.id) != ids.end()) return false;
            ids.push_back(tab.id);
        }
    }
    for (const auto& child : node.children) if (!collect_ids(child, ids)) return false;
    return true;
}

bool valid_new_node(const DockWorkspace& workspace, const DockNode& candidate) {
    std::vector<std::string> ids;
    if (!collect_ids(candidate, ids) || ids.empty()) return false;
    for (const auto& id : ids) if (panel_id_in(workspace.root(), id) || std::any_of(workspace.floating().begin(), workspace.floating().end(),
            [&id](const auto& node) { return panel_id_in(node, id); })) return false;
    return true;
}

bool append_to_target(DockNode& node, std::string_view target, DockPanel& panel, bool activate) {
    if (node.kind == DockNode::Kind::TabStack) {
        if (!std::any_of(node.tabs.begin(), node.tabs.end(), [target](const auto& tab) { return tab.id == target; })) {
            for (auto& child : node.children) if (append_to_target(child, target, panel, activate)) return true;
            return false;
        }
        node.tabs.push_back(std::move(panel));
        if (activate) node.activeTab = node.tabs.size() - 1;
        return true;
    }
    if (node.kind == DockNode::Kind::Leaf && node.panelId == target) {
        const DockPanel old = panel_from_leaf(node);
        node = DockNode::tab_stack({old, std::move(panel)}, activate ? 1u : 0u);
        return true;
    }
    for (auto& child : node.children) if (append_to_target(child, target, panel, activate)) return true;
    return false;
}

bool insert_into_target(DockNode& node, std::string_view target, DockPanel& panel, std::size_t index) {
    if (node.kind == DockNode::Kind::TabStack && std::any_of(node.tabs.begin(), node.tabs.end(), [target](const auto& tab) {
            return tab.id == target;
        })) {
        const auto at = std::min(index, node.tabs.size());
        node.tabs.insert(node.tabs.begin() + static_cast<std::ptrdiff_t>(at), std::move(panel));
        if (node.activeTab >= at && node.tabs.size() > 1) ++node.activeTab;
        return true;
    }
    if (node.kind == DockNode::Kind::Leaf && node.panelId == target) {
        const DockPanel old = panel_from_leaf(node);
        if (index == 0) node = DockNode::tab_stack({std::move(panel), old}, 0);
        else node = DockNode::tab_stack({old, std::move(panel)}, 0);
        return true;
    }
    for (auto& child : node.children) if (insert_into_target(child, target, panel, index)) return true;
    return false;
}

bool detach_panel(DockNode& node, std::string_view id, DockPanel& result) {
    if (node.kind == DockNode::Kind::Leaf && node.panelId == id) {
        result = panel_from_leaf(node);
        node = DockNode{};
        return true;
    }
    if (node.kind == DockNode::Kind::TabStack) {
        const auto it = std::find_if(node.tabs.begin(), node.tabs.end(), [id](const auto& tab) { return tab.id == id; });
        if (it != node.tabs.end()) {
            result = std::move(*it);
            node.tabs.erase(it);
            if (node.activeTab >= node.tabs.size() && !node.tabs.empty()) node.activeTab = node.tabs.size() - 1;
            return true;
        }
    }
    for (auto& child : node.children) if (detach_panel(child, id, result)) return true;
    return false;
}

bool replace_for_split(DockNode& node, std::string_view target, DockSplitOrientation direction, float ratio, DockNode& newNode) {
    const bool isTarget = (node.kind == DockNode::Kind::Leaf && node.panelId == target) ||
        (node.kind == DockNode::Kind::TabStack && std::any_of(node.tabs.begin(), node.tabs.end(), [target](const auto& tab) {
            return tab.id == target;
        }));
    if (isTarget) {
        node = DockNode::split(std::move(node), std::move(newNode), direction, ratio);
        return true;
    }
    for (auto& child : node.children) if (replace_for_split(child, target, direction, ratio, newNode)) return true;
    return false;
}

bool set_panel_value(DockNode& node, std::string_view id, const std::function<void(DockPanel&)>& fn) {
    if (node.kind == DockNode::Kind::Leaf && node.panelId == id) {
        auto panel = panel_from_leaf(node); fn(panel);
        node = DockNode::leaf(std::move(panel));
        return true;
    }
    if (node.kind == DockNode::Kind::TabStack) {
        for (auto& tab : node.tabs) if (tab.id == id) { fn(tab); return true; }
    }
    for (auto& child : node.children) if (set_panel_value(child, id, fn)) return true;
    return false;
}

bool is_empty(const DockNode& node) {
    if (node.kind == DockNode::Kind::Leaf) return node.panelId.empty();
    if (node.kind == DockNode::Kind::TabStack) return node.tabs.empty();
    return node.children.empty();
}

void normalize_node(DockNode& node) {
    for (auto& child : node.children) normalize_node(child);
    if (node.kind == DockNode::Kind::TabStack) {
        node.tabs.erase(std::remove_if(node.tabs.begin(), node.tabs.end(), [](const auto& tab) { return tab.id.empty(); }), node.tabs.end());
        if (node.tabs.empty()) { node = DockNode{}; return; }
        if (node.activeTab >= node.tabs.size()) node.activeTab = node.tabs.size() - 1;
        if (node.tabs.size() == 1) {
            // Move the child out before overwriting the owning node. Moving
            // directly from node.tabs.front() while assigning to node aliases
            // the destination object and can corrupt the vector allocation.
            DockPanel promoted = std::move(node.tabs.front());
            node = DockNode::leaf(std::move(promoted));
            return;
        }
    } else if (node.kind == DockNode::Kind::Split) {
        node.children.erase(std::remove_if(node.children.begin(), node.children.end(), is_empty), node.children.end());
        node.ratio = std::clamp(std::isfinite(node.ratio) ? node.ratio : 0.5f, kMinRatio, kMaxRatio);
        if (node.children.size() == 1) {
            DockNode promoted = std::move(node.children.front());
            node = std::move(promoted);
            return;
        }
        if (node.children.empty()) { node = DockNode{}; return; }
    } else if (node.kind == DockNode::Kind::Floating) {
        node.children.erase(std::remove_if(node.children.begin(), node.children.end(), is_empty), node.children.end());
        if (node.children.size() > 1) node.children.resize(1);
        if (node.children.empty()) { node = DockNode{}; return; }
        node.bounds.width = std::max(0.0f, node.bounds.width);
        node.bounds.height = std::max(0.0f, node.bounds.height);
    }
}

math::Vec2 min_size_impl(const DockNode& node) noexcept {
    if (node.kind == DockNode::Kind::Leaf) return node.panelId.empty() ? math::Vec2{} : node.minSize;
    if (node.kind == DockNode::Kind::TabStack) {
        math::Vec2 result{};
        for (const auto& tab : node.tabs) { result.x = std::max(result.x, tab.minSize.x); result.y = std::max(result.y, tab.minSize.y); }
        return result;
    }
    if (node.children.empty()) return {};
    if (node.kind == DockNode::Kind::Floating) return min_size_impl(node.children.front());
    if (node.children.size() == 1) return min_size_impl(node.children.front());
    const auto first = min_size_impl(node.children[0]);
    const auto second = min_size_impl(node.children[1]);
    if (node.orientation == DockSplitOrientation::Horizontal) return {first.x + second.x, std::max(first.y, second.y)};
    return {std::max(first.x, second.x), first.y + second.y};
}

float layout_ratio(const DockNode& node, math::Vec2 size, float bar) noexcept {
    if (node.children.size() < 2) return std::clamp(node.ratio, kMinRatio, kMaxRatio);
    const auto first = min_size_impl(node.children[0]);
    const auto second = min_size_impl(node.children[1]);
    const float total = node.orientation == DockSplitOrientation::Horizontal ? size.x : size.y;
    const float firstMin = node.orientation == DockSplitOrientation::Horizontal ? first.x : first.y;
    const float secondMin = node.orientation == DockSplitOrientation::Horizontal ? second.x : second.y;
    const float usable = std::max(0.0f, total - std::max(0.0f, bar));
    if (usable <= 0.0f) return 0.5f;
    const float low = std::clamp(firstMin / usable, 0.0f, 1.0f);
    const float high = std::clamp(1.0f - secondMin / usable, 0.0f, 1.0f);
    if (low > high) return std::clamp((low + high) * 0.5f, 0.0f, 1.0f);
    return std::clamp(node.ratio, low, high);
}

void rebalance_node(DockNode& node, math::Vec2 size, float bar) noexcept {
    if (node.kind == DockNode::Kind::Floating) {
        if (!node.children.empty()) rebalance_node(node.children.front(), {node.bounds.width, node.bounds.height}, bar);
        return;
    }
    if (node.kind != DockNode::Kind::Split || node.children.size() < 2) return;
    node.ratio = layout_ratio(node, size, bar);
    const float total = node.orientation == DockSplitOrientation::Horizontal ? size.x : size.y;
    const float firstExtent = std::max(0.0f, (total - bar) * node.ratio);
    math::Vec2 firstSize = size;
    math::Vec2 secondSize = size;
    if (node.orientation == DockSplitOrientation::Horizontal) {
        firstSize.x = firstExtent; secondSize.x = std::max(0.0f, total - bar - firstExtent);
    } else {
        firstSize.y = firstExtent; secondSize.y = std::max(0.0f, total - bar - firstExtent);
    }
    rebalance_node(node.children[0], firstSize, bar);
    rebalance_node(node.children[1], secondSize, bar);
}

struct JsonValue {
    enum class Kind : std::uint8_t { Null, Boolean, Number, String, Array, Object } kind{Kind::Null};
    bool boolean{false};
    double number{0.0};
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;
};

class JsonParser {
    std::string_view input_;
    std::size_t position_{0};
    DockSerializationResult result_{};

    void fail(DockSerializationError error, std::string message) {
        if (result_) result_ = {error, position_, std::move(message)};
    }
    void whitespace() noexcept { while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\n' || input_[position_] == '\r' || input_[position_] == '\t')) ++position_; }
    bool consume(char value) { whitespace(); if (position_ >= input_.size() || input_[position_] != value) { fail(DockSerializationError::InvalidJson, "unexpected token"); return false; } ++position_; return true; }
    static void append_utf8(std::string& out, unsigned value) {
        if (value <= 0x7f) out.push_back(static_cast<char>(value));
        else if (value <= 0x7ff) { out.push_back(static_cast<char>(0xc0 | (value >> 6))); out.push_back(static_cast<char>(0x80 | (value & 0x3f))); }
        else if (value <= 0xffff) { out.push_back(static_cast<char>(0xe0 | (value >> 12))); out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (value & 0x3f))); }
        else { out.push_back(static_cast<char>(0xf0 | (value >> 18))); out.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f))); out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (value & 0x3f))); }
    }
    bool hex(unsigned& value) {
        if (position_ + 4 > input_.size()) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[position_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value += static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value += static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value += static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    JsonValue string_value() {
        JsonValue value; value.kind = JsonValue::Kind::String;
        if (!consume('"')) return value;
        while (position_ < input_.size()) {
            const char c = input_[position_++];
            if (c == '"') return value;
            if (static_cast<unsigned char>(c) < 0x20) { fail(DockSerializationError::InvalidJson, "control character in string"); return value; }
            if (c != '\\') { value.string.push_back(c); continue; }
            if (position_ >= input_.size()) break;
            const char escape = input_[position_++];
            switch (escape) {
            case '"': value.string.push_back('"'); break; case '\\': value.string.push_back('\\'); break; case '/': value.string.push_back('/'); break;
            case 'b': value.string.push_back('\b'); break; case 'f': value.string.push_back('\f'); break; case 'n': value.string.push_back('\n'); break;
            case 'r': value.string.push_back('\r'); break; case 't': value.string.push_back('\t'); break;
            case 'u': { unsigned code = 0; if (!hex(code)) { fail(DockSerializationError::InvalidJson, "invalid unicode escape"); return value; } append_utf8(value.string, code); break; }
            default: fail(DockSerializationError::InvalidJson, "invalid string escape"); return value;
            }
        }
        fail(DockSerializationError::InvalidJson, "unterminated string");
        return value;
    }
    JsonValue number_value() {
        JsonValue value; value.kind = JsonValue::Kind::Number;
        whitespace(); const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) { fail(DockSerializationError::InvalidJson, "invalid number"); return value; }
        if (input_[position_] == '0') ++position_;
        else if (input_[position_] >= '1' && input_[position_] <= '9') while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        else { fail(DockSerializationError::InvalidJson, "invalid number"); return value; }
        if (position_ < input_.size() && input_[position_] == '.') { ++position_; const auto before = position_; while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_; if (before == position_) { fail(DockSerializationError::InvalidJson, "invalid fraction"); return value; } }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) { ++position_; if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_; const auto before = position_; while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_; if (before == position_) { fail(DockSerializationError::InvalidJson, "invalid exponent"); return value; } }
        const std::string text(input_.substr(start, position_ - start));
        char* end = nullptr; errno = 0; value.number = std::strtod(text.c_str(), &end);
        if (errno == ERANGE || end != text.c_str() + text.size() || !std::isfinite(value.number)) fail(DockSerializationError::InvalidJson, "number out of range");
        return value;
    }
    JsonValue value(std::size_t depth) {
        if (depth > kMaxJsonDepth) { fail(DockSerializationError::InvalidJson, "maximum JSON depth exceeded"); return {}; }
        whitespace();
        if (position_ >= input_.size()) { fail(DockSerializationError::InvalidJson, "missing value"); return {}; }
        const char c = input_[position_];
        if (c == '"') return string_value();
        if (c == '{') {
            JsonValue object; object.kind = JsonValue::Kind::Object; ++position_; whitespace();
            if (position_ < input_.size() && input_[position_] == '}') { ++position_; return object; }
            while (result_) {
                whitespace(); if (position_ >= input_.size() || input_[position_] != '"') { fail(DockSerializationError::InvalidJson, "object key expected"); return object; }
                auto key = string_value().string; if (!consume(':')) return object;
                for (const auto& item : object.object) if (item.first == key) { fail(DockSerializationError::InvalidJson, "duplicate object field"); return object; }
                object.object.emplace_back(std::move(key), value(depth + 1));
                whitespace(); if (position_ < input_.size() && input_[position_] == '}') { ++position_; return object; }
                if (!consume(',')) return object;
            }
            return object;
        }
        if (c == '[') {
            JsonValue array; array.kind = JsonValue::Kind::Array; ++position_; whitespace();
            if (position_ < input_.size() && input_[position_] == ']') { ++position_; return array; }
            while (result_) { array.array.push_back(value(depth + 1)); whitespace(); if (position_ < input_.size() && input_[position_] == ']') { ++position_; return array; } if (!consume(',')) return array; }
            return array;
        }
        if (input_.substr(position_, 4) == "true") { position_ += 4; JsonValue v; v.kind = JsonValue::Kind::Boolean; v.boolean = true; return v; }
        if (input_.substr(position_, 5) == "false") { position_ += 5; JsonValue v; v.kind = JsonValue::Kind::Boolean; return v; }
        if (input_.substr(position_, 4) == "null") { position_ += 4; return {}; }
        if (c == '-' || (c >= '0' && c <= '9')) return number_value();
        fail(DockSerializationError::InvalidJson, "unknown JSON value"); return {};
    }

public:
    explicit JsonParser(std::string_view input) : input_(input) {}
    JsonValue parse() { auto result = value(0); whitespace(); if (result_ && position_ != input_.size()) fail(DockSerializationError::InvalidJson, "trailing JSON data"); return result; }
    const DockSerializationResult& result() const noexcept { return result_; }
};

const JsonValue* field(const JsonValue& object, std::string_view name) {
    if (object.kind != JsonValue::Kind::Object) return nullptr;
    for (const auto& item : object.object) if (item.first == name) return &item.second;
    return nullptr;
}

DockSerializationResult type_error(std::string message) { return {DockSerializationError::TypeMismatch, 0, std::move(message)}; }

bool read_string(const JsonValue& object, std::string_view name, std::string& out, bool required, DockSerializationResult& error) {
    const auto* value = field(object, name);
    if (!value) { if (required) error = {DockSerializationError::MissingField, 0, std::string("missing field: ") + std::string(name)}; return !required; }
    if (value->kind != JsonValue::Kind::String) { error = type_error(std::string("field is not a string: ") + std::string(name)); return false; }
    out = value->string; return true;
}
bool read_bool(const JsonValue& object, std::string_view name, bool& out, DockSerializationResult& error) {
    const auto* value = field(object, name); if (!value) return true;
    if (value->kind != JsonValue::Kind::Boolean) { error = type_error(std::string("field is not a boolean: ") + std::string(name)); return false; }
    out = value->boolean; return true;
}
bool read_number(const JsonValue& object, std::string_view name, double& out, bool required, DockSerializationResult& error) {
    const auto* value = field(object, name);
    if (!value) { if (required) error = {DockSerializationError::MissingField, 0, std::string("missing field: ") + std::string(name)}; return !required; }
    if (value->kind != JsonValue::Kind::Number || !std::isfinite(value->number)) { error = type_error(std::string("field is not a number: ") + std::string(name)); return false; }
    out = value->number; return true;
}

bool read_min_size(const JsonValue& object, math::Vec2& result, DockSerializationResult& error) {
    const auto* value = field(object, "minSize"); if (!value) return true;
    if (value->kind != JsonValue::Kind::Array || value->array.size() != 2 || value->array[0].kind != JsonValue::Kind::Number || value->array[1].kind != JsonValue::Kind::Number ||
        !finite_non_negative(static_cast<float>(value->array[0].number)) || !finite_non_negative(static_cast<float>(value->array[1].number))) {
        error = type_error("minSize must be a non-negative [width, height] array"); return false;
    }
    result = {static_cast<float>(value->array[0].number), static_cast<float>(value->array[1].number)}; return true;
}

bool read_panel(const JsonValue& value, DockPanel& panel, DockSerializationResult& error) {
    if (value.kind != JsonValue::Kind::Object) { error = type_error("tab must be an object"); return false; }
    if (!read_string(value, "id", panel.id, true, error) || !read_string(value, "title", panel.title, false, error) ||
        !read_bool(value, "visible", panel.visible, error) || !read_bool(value, "closeable", panel.closeable, error) || !read_min_size(value, panel.minSize, error)) return false;
    if (panel.id.empty()) { error = {DockSerializationError::InvalidTree, 0, "panel id cannot be empty"}; return false; }
    return true;
}

bool read_node(const JsonValue& value, DockNode& node, DockSerializationResult& error, std::size_t depth = 0) {
    if (depth > kMaxJsonDepth || value.kind != JsonValue::Kind::Object) { error = type_error("node must be an object"); return false; }
    std::string type; if (!read_string(value, "type", type, true, error)) return false;
    if (type == "leaf") {
        DockPanel panel;
        if (!read_string(value, "panelId", panel.id, true, error) || !read_string(value, "title", panel.title, false, error) ||
            !read_bool(value, "visible", panel.visible, error) || !read_bool(value, "closeable", panel.closeable, error) || !read_min_size(value, panel.minSize, error)) return false;
        if (panel.id.empty()) { error = {DockSerializationError::InvalidTree, 0, "panel id cannot be empty"}; return false; }
        node = DockNode::leaf(std::move(panel)); return true;
    }
    if (type == "tab_stack") {
        const auto* tabs = field(value, "tabs"); if (!tabs || tabs->kind != JsonValue::Kind::Array) { error = {DockSerializationError::MissingField, 0, "tab stack tabs must be an array"}; return false; }
        std::size_t active = 0; const auto* activeValue = field(value, "active");
        if (activeValue) { if (activeValue->kind != JsonValue::Kind::Number || activeValue->number < 0.0 || std::floor(activeValue->number) != activeValue->number || activeValue->number > static_cast<double>(DockWorkspace::npos)) { error = {DockSerializationError::RangeError, 0, "invalid active tab"}; return false; } active = static_cast<std::size_t>(activeValue->number); }
        std::vector<DockPanel> panels; for (const auto& tab : tabs->array) { DockPanel panel; if (!read_panel(tab, panel, error)) return false; panels.push_back(std::move(panel)); }
        if (!panels.empty() && active >= panels.size()) { error = {DockSerializationError::RangeError, 0, "active tab is out of range"}; return false; }
        node = DockNode::tab_stack(std::move(panels), active); return true;
    }
    if (type == "split") {
        std::string orientation; double ratio = 0.0; if (!read_string(value, "orientation", orientation, true, error) || !read_number(value, "ratio", ratio, true, error)) return false;
        if (orientation != "horizontal" && orientation != "vertical") { error = {DockSerializationError::InvalidTree, 0, "invalid split orientation"}; return false; }
        if (!valid_ratio(static_cast<float>(ratio))) { error = {DockSerializationError::RangeError, 0, "split ratio must be between zero and one"}; return false; }
        const auto* children = field(value, "children"); if (!children || children->kind != JsonValue::Kind::Array || children->array.size() != 2) { error = {DockSerializationError::InvalidTree, 0, "split must have two children"}; return false; }
        DockNode first, second; if (!read_node(children->array[0], first, error, depth + 1) || !read_node(children->array[1], second, error, depth + 1)) return false;
        node = DockNode::split(std::move(first), std::move(second), orientation == "horizontal" ? DockSplitOrientation::Horizontal : DockSplitOrientation::Vertical, static_cast<float>(ratio)); return true;
    }
    if (type == "floating") {
        const auto* bounds = field(value, "bounds"); if (!bounds || bounds->kind != JsonValue::Kind::Object) { error = {DockSerializationError::MissingField, 0, "floating bounds are required"}; return false; }
        double x, y, width, height; if (!read_number(*bounds, "x", x, true, error) || !read_number(*bounds, "y", y, true, error) || !read_number(*bounds, "width", width, true, error) || !read_number(*bounds, "height", height, true, error)) return false;
        if (!std::isfinite(static_cast<float>(x)) || !std::isfinite(static_cast<float>(y)) || !finite_non_negative(static_cast<float>(width)) || !finite_non_negative(static_cast<float>(height))) { error = {DockSerializationError::RangeError, 0, "floating bounds must be finite and have non-negative size"}; return false; }
        const auto* child = field(value, "child"); if (!child) { error = {DockSerializationError::MissingField, 0, "floating child is required"}; return false; }
        DockNode childNode; if (!read_node(*child, childNode, error, depth + 1)) return false;
        node = DockNode::floating(std::move(childNode), {static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height)}); return true;
    }
    error = {DockSerializationError::InvalidTree, 0, "unknown dock node type"}; return false;
}

void indent(std::string& out, bool pretty, int level) { if (pretty) out.append(static_cast<std::size_t>(level) * 2, ' '); }
void newline(std::string& out, bool pretty) { if (pretty) out.push_back('\n'); }
void escape_string(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const unsigned char c : value) {
        switch (c) { case '"': out += "\\\""; break; case '\\': out += "\\\\"; break; case '\b': out += "\\b"; break; case '\f': out += "\\f"; break; case '\n': out += "\\n"; break; case '\r': out += "\\r"; break; case '\t': out += "\\t"; break;
        default: if (c < 0x20) { std::ostringstream hex; hex << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c); out += hex.str(); } else out.push_back(static_cast<char>(c)); }
    }
    out.push_back('"');
}
void number(std::string& out, float value) { std::ostringstream stream; stream << std::setprecision(9) << (value == 0.0f ? 0.0f : value); out += stream.str(); }
void write_panel(std::string& out, const DockPanel& panel, bool pretty, int level) {
    out += "{"; newline(out, pretty); indent(out, pretty, level + 1); out += "\"id\":"; escape_string(out, panel.id); out += ","; newline(out, pretty);
    indent(out, pretty, level + 1); out += "\"title\":"; escape_string(out, panel.title); out += ","; newline(out, pretty);
    indent(out, pretty, level + 1); out += "\"visible\":"; out += panel.visible ? "true" : "false"; out += ","; newline(out, pretty);
    indent(out, pretty, level + 1); out += "\"closeable\":"; out += panel.closeable ? "true" : "false"; out += ","; newline(out, pretty);
    indent(out, pretty, level + 1); out += "\"minSize\":["; number(out, panel.minSize.x); out += ","; if (pretty) out.push_back(' '); number(out, panel.minSize.y); out += "]"; newline(out, pretty); indent(out, pretty, level); out += "}";
}
void write_node(std::string& out, const DockNode& node, bool pretty, int level) {
    out += "{"; newline(out, pretty); indent(out, pretty, level + 1); out += "\"type\":";
    if (node.kind == DockNode::Kind::Leaf) {
        escape_string(out, "leaf"); out += ","; newline(out, pretty); indent(out, pretty, level + 1); out += "\"panelId\":"; escape_string(out, node.panelId); out += ","; newline(out, pretty);
        indent(out, pretty, level + 1); out += "\"title\":"; escape_string(out, node.title); out += ","; newline(out, pretty); indent(out, pretty, level + 1); out += "\"visible\":"; out += node.visible ? "true" : "false"; out += ","; newline(out, pretty);
        indent(out, pretty, level + 1); out += "\"closeable\":"; out += node.closeable ? "true" : "false"; out += ","; newline(out, pretty); indent(out, pretty, level + 1); out += "\"minSize\":["; number(out, node.minSize.x); out += ","; if (pretty) out.push_back(' '); number(out, node.minSize.y); out += "]";
    } else if (node.kind == DockNode::Kind::TabStack) {
        escape_string(out, "tab_stack"); out += ","; newline(out, pretty); indent(out, pretty, level + 1); out += "\"active\":"; out += std::to_string(node.activeTab); out += ","; newline(out, pretty); indent(out, pretty, level + 1); out += "\"tabs\":[";
        for (std::size_t i = 0; i < node.tabs.size(); ++i) { newline(out, pretty); indent(out, pretty, level + 2); write_panel(out, node.tabs[i], pretty, level + 2); if (i + 1 < node.tabs.size()) out += ","; } if (!node.tabs.empty()) { newline(out, pretty); indent(out, pretty, level + 1); } out += "]";
    } else if (node.kind == DockNode::Kind::Split) {
        escape_string(out, "split"); out += ","; newline(out, pretty); indent(out, pretty, level + 1); out += "\"orientation\":"; escape_string(out, node.orientation == DockSplitOrientation::Horizontal ? "horizontal" : "vertical"); out += ","; newline(out, pretty); indent(out, pretty, level + 1); out += "\"ratio\":"; number(out, node.ratio); out += ","; newline(out, pretty); indent(out, pretty, level + 1); out += "\"children\":[";
        for (std::size_t i = 0; i < node.children.size(); ++i) { newline(out, pretty); indent(out, pretty, level + 2); write_node(out, node.children[i], pretty, level + 2); if (i + 1 < node.children.size()) out += ","; } if (!node.children.empty()) { newline(out, pretty); indent(out, pretty, level + 1); } out += "]";
    } else {
        escape_string(out, "floating"); out += ","; newline(out, pretty); indent(out, pretty, level + 1); out += "\"bounds\":{"; if (pretty) out.push_back(' '); out += "\"x\":"; number(out, node.bounds.x); out += ","; if (pretty) out.push_back(' '); out += "\"y\":"; number(out, node.bounds.y); out += ","; if (pretty) out.push_back(' '); out += "\"width\":"; number(out, node.bounds.width); out += ","; if (pretty) out.push_back(' '); out += "\"height\":"; number(out, node.bounds.height); if (pretty) out.push_back(' '); out += "},"; newline(out, pretty); indent(out, pretty, level + 1); out += "\"child\":"; if (!node.children.empty()) write_node(out, node.children.front(), pretty, level + 1); else out += "null";
    }
    newline(out, pretty); indent(out, pretty, level); out += "}";
}

std::optional<DockSplitHit> hit_node(const DockNode& node, DockRect area, math::Vec2 point, float bar) noexcept {
    if (node.kind == DockNode::Kind::Floating) {
        if (node.children.empty()) return std::nullopt;
        return hit_node(node.children.front(), node.bounds, point, bar);
    }
    if (node.kind != DockNode::Kind::Split || node.children.size() < 2) return std::nullopt;
    const float ratio = layout_ratio(node, {area.width, area.height}, bar);
    const float total = node.orientation == DockSplitOrientation::Horizontal ? area.width : area.height;
    const float firstExtent = std::max(0.0f, (total - std::max(0.0f, bar)) * ratio);
    DockRect first = area, second = area, splitBar = area;
    if (node.orientation == DockSplitOrientation::Horizontal) { first.width = firstExtent; splitBar = {area.x + firstExtent, area.y, bar, area.height}; second.x = area.x + firstExtent + bar; second.width = std::max(0.0f, area.width - firstExtent - bar); }
    else { first.height = firstExtent; splitBar = {area.x, area.y + firstExtent, area.width, bar}; second.y = area.y + firstExtent + bar; second.height = std::max(0.0f, area.height - firstExtent - bar); }
    if (first.contains(point)) if (auto hit = hit_node(node.children[0], first, point, bar)) return hit;
    if (second.contains(point)) if (auto hit = hit_node(node.children[1], second, point, bar)) return hit;
    if (splitBar.contains(point)) return DockSplitHit{&node, splitBar, node.orientation, ratio};
    return std::nullopt;
}

} // namespace

DockNode DockNode::leaf(DockPanel panel) {
    DockNode node; node.kind = Kind::Leaf; node.panelId = std::move(panel.id); node.title = std::move(panel.title); node.visible = panel.visible; node.closeable = panel.closeable; node.minSize = panel.minSize; return node;
}
DockNode DockNode::tab_stack(std::vector<DockPanel> panels, std::size_t active) {
    DockNode node; node.kind = Kind::TabStack; node.tabs = std::move(panels); node.activeTab = node.tabs.empty() ? 0 : std::min(active, node.tabs.size() - 1); return node;
}
DockNode DockNode::split(DockNode first, DockNode second, DockSplitOrientation direction, float splitRatio) {
    DockNode node; node.kind = Kind::Split; node.orientation = direction; node.ratio = valid_ratio(splitRatio) ? splitRatio : 0.5f; node.children = {std::move(first), std::move(second)}; return node;
}
DockNode DockNode::floating(DockNode child, DockRect rectangle) {
    DockNode node; node.kind = Kind::Floating; node.bounds = rectangle; node.children.push_back(std::move(child)); return node;
}

bool DockWorkspace::add_floating(DockNode node) {
    if (node.kind != DockNode::Kind::Floating || node.children.size() != 1 || !valid_new_node(*this, node)) return false;
    floating_.push_back(std::move(node)); return true;
}

bool DockWorkspace::add_tab(std::string_view targetPanelId, DockPanel panel, bool activate) {
    if (panel.id.empty() || panel_id_in(root_, panel.id) || std::any_of(floating_.begin(), floating_.end(), [&panel](const auto& node) { return panel_id_in(node, panel.id); })) return false;
    if (targetPanelId.empty()) { if (has_any_panel(*this)) return false; root_ = DockNode::leaf(std::move(panel)); return true; }
    if (!panel_id_in(root_, targetPanelId) && !std::any_of(floating_.begin(), floating_.end(), [targetPanelId](const auto& node) { return panel_id_in(node, targetPanelId); })) return false;
    bool inserted = append_to_target(root_, targetPanelId, panel, activate);
    if (!inserted) for (auto& node : floating_) if (append_to_target(node, targetPanelId, panel, activate)) { inserted = true; break; }
    if (inserted) normalize();
    return inserted;
}

bool DockWorkspace::remove_tab(std::string_view panelId) {
    if (panelId.empty()) return false;
    DockPanel removed; bool result = detach_panel(root_, panelId, removed);
    if (!result) for (auto& node : floating_) if (detach_panel(node, panelId, removed)) { result = true; break; }
    if (result) normalize();
    return result;
}

bool DockWorkspace::move_tab(std::string_view panelId, std::string_view targetPanelId, std::size_t index) {
    if (panelId.empty() || targetPanelId.empty() || panelId == targetPanelId || !find_panel(*this, panelId).node || !find_panel(*this, targetPanelId).node) return false;
    const DockWorkspace original = *this;
    DockPanel moved; bool result = detach_panel(root_, panelId, moved);
    if (!result) for (auto& node : floating_) if (detach_panel(node, panelId, moved)) { result = true; break; }
    if (!result) return false;
    bool inserted = insert_into_target(root_, targetPanelId, moved, index);
    if (!inserted) for (auto& node : floating_) if (insert_into_target(node, targetPanelId, moved, index)) { inserted = true; break; }
    if (!inserted) { *this = original; return false; }
    normalize(); return true;
}

bool DockWorkspace::split(std::string_view targetPanelId, DockSplitOrientation direction, float ratio, DockPanel newPanel) {
    return split(targetPanelId, direction, ratio, DockNode::leaf(std::move(newPanel)));
}
bool DockWorkspace::split(std::string_view targetPanelId, DockSplitOrientation direction, float ratio, DockNode newNode) {
    if (targetPanelId.empty() || !valid_ratio(ratio) || !find_panel(*this, targetPanelId).node || !valid_new_node(*this, newNode)) return false;
    bool result = replace_for_split(root_, targetPanelId, direction, ratio, newNode);
    if (!result) for (auto& node : floating_) if (replace_for_split(node, targetPanelId, direction, ratio, newNode)) { result = true; break; }
    if (result) normalize();
    return result;
}

bool DockWorkspace::activate_tab(std::string_view panelId) {
    const auto found = find_panel(*this, panelId); if (!found.node) return false;
    if (found.node->kind == DockNode::Kind::TabStack) found.node->activeTab = found.tabIndex;
    return true;
}
bool DockWorkspace::set_panel_visibility(std::string_view panelId, bool value) {
    return set_panel_value(root_, panelId, [value](DockPanel& panel) { panel.visible = value; }) ||
        std::any_of(floating_.begin(), floating_.end(), [panelId, value](auto& node) { return set_panel_value(node, panelId, [value](DockPanel& panel) { panel.visible = value; }); });
}
bool DockWorkspace::set_minimum_size(std::string_view panelId, math::Vec2 size) {
    if (!finite_non_negative(size.x) || !finite_non_negative(size.y)) return false;
    return set_panel_value(root_, panelId, [size](DockPanel& panel) { panel.minSize = size; }) ||
        std::any_of(floating_.begin(), floating_.end(), [panelId, size](auto& node) { return set_panel_value(node, panelId, [size](DockPanel& panel) { panel.minSize = size; }); });
}
math::Vec2 DockWorkspace::minimum_size() const noexcept { return min_size_impl(root_); }
math::Vec2 DockWorkspace::minimum_size(const DockNode& node) noexcept { return min_size_impl(node); }
void DockWorkspace::normalize() { normalize_node(root_); for (auto& node : floating_) normalize_node(node); floating_.erase(std::remove_if(floating_.begin(), floating_.end(), is_empty), floating_.end()); }
void DockWorkspace::rebalance(math::Vec2 availableSize, float splitterThickness) { if (!finite_non_negative(availableSize.x) || !finite_non_negative(availableSize.y) || !finite_non_negative(splitterThickness)) return; rebalance_node(root_, availableSize, splitterThickness); for (auto& node : floating_) rebalance_node(node, {node.bounds.width, node.bounds.height}, splitterThickness); }

std::optional<DockSplitHit> DockWorkspace::hit_test_split_bar(DockRect area, math::Vec2 point, float splitterThickness) const noexcept {
    if (!finite_non_negative(area.width) || !finite_non_negative(area.height) || !finite_non_negative(splitterThickness)) return std::nullopt;
    for (auto it = floating_.rbegin(); it != floating_.rend(); ++it) if (it->bounds.contains(point)) if (auto hit = hit_node(*it, it->bounds, point, splitterThickness)) return hit;
    if (area.contains(point)) return hit_node(root_, area, point, splitterThickness);
    return std::nullopt;
}

std::string DockWorkspace::to_json(bool pretty) const { return serialize_dock_workspace(*this, pretty); }
DockSerializationResult DockWorkspace::from_json(std::string_view json) { return deserialize_dock_workspace(json, *this); }

std::string serialize_dock_workspace(const DockWorkspace& workspace, bool pretty) {
    std::string output = "{\"version\":" + std::to_string(DockWorkspace::CurrentVersion) + ",\"root\":";
    if (pretty) output = "{\n  \"version\": " + std::to_string(DockWorkspace::CurrentVersion) + ",\n  \"root\": ";
    write_node(output, workspace.root(), pretty, pretty ? 1 : 0);
    output += pretty ? ",\n  \"floating\":[" : ",\"floating\":[";
    for (std::size_t i = 0; i < workspace.floating().size(); ++i) { if (pretty) output += "\n    "; write_node(output, workspace.floating()[i], pretty, pretty ? 2 : 0); if (i + 1 < workspace.floating().size()) output += ","; }
    if (pretty) output += "\n  ]\n}"; else output += "]}";
    return output;
}

DockSerializationResult deserialize_dock_workspace(std::string_view json, DockWorkspace& workspace) {
    if (json.size() > 4u * 1024u * 1024u) return {DockSerializationError::InvalidJson, 0, "JSON input is too large"};
    try {
        JsonParser parser(json); const auto value = parser.parse(); if (!parser.result()) return parser.result();
        if (value.kind != JsonValue::Kind::Object) return {DockSerializationError::TypeMismatch, 0, "workspace must be an object"};
        const auto* version = field(value, "version"); if (!version || version->kind != JsonValue::Kind::Number || version->number != DockWorkspace::CurrentVersion) return {DockSerializationError::UnsupportedVersion, 0, "unsupported or missing workspace version"};
        const auto* root = field(value, "root"); if (!root) return {DockSerializationError::MissingField, 0, "missing workspace root"};
        DockNode parsedRoot; DockSerializationResult error; if (!read_node(*root, parsedRoot, error)) return error;
        DockWorkspace parsed(std::move(parsedRoot));
        if (const auto* floating = field(value, "floating")) {
            if (floating->kind != JsonValue::Kind::Array) return type_error("floating must be an array");
            for (const auto& item : floating->array) { DockNode node; if (!read_node(item, node, error) || node.kind != DockNode::Kind::Floating) return error ? error : DockSerializationResult{DockSerializationError::InvalidTree, 0, "floating entry must be floating"}; parsed.floating().push_back(std::move(node)); }
        }
        parsed.normalize(); std::vector<std::string> ids; if (!collect_ids(parsed.root(), ids)) return {DockSerializationError::InvalidTree, 0, "duplicate or empty panel id"}; for (const auto& node : parsed.floating()) if (!collect_ids(node, ids)) return {DockSerializationError::InvalidTree, 0, "duplicate or empty panel id"};
        workspace = std::move(parsed); return {};
    } catch (const std::exception& exception) {
        return {DockSerializationError::InvalidJson, 0, std::string("unable to parse workspace: ") + exception.what()};
    } catch (...) { return {DockSerializationError::InvalidJson, 0, "unable to parse workspace"}; }
}

std::string serialize_json(const DockWorkspace& workspace, bool pretty) { return serialize_dock_workspace(workspace, pretty); }
DockSerializationResult deserialize_json(std::string_view json, DockWorkspace& workspace) { return deserialize_dock_workspace(json, workspace); }

} // namespace shinkou::editor

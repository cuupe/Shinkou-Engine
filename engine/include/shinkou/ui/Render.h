#pragma once

#include "shinkou/ui/Theme.h"
#include "shinkou/ui/Ui.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::ui {

// Backend-neutral retained commands. The editor renderer can translate this
// list to ImGui, a custom GPU batch, or a remote UI without changing widgets.
enum class DrawCommandType : std::uint8_t { Rect, Line, Text, Image, Clip };

struct UiDrawCommand {
    DrawCommandType type{DrawCommandType::Rect};
    Rect rect{};
    Vec2 from{};
    Vec2 to{};
    ThemeColor color{};
    ThemeColor secondary{};
    float thickness{1.0f};
    float rounding{0.0f};
    std::uint64_t texture{0};
    std::string text{};
};

class UiRenderList final {
    std::vector<UiDrawCommand> commands_;

public:
    explicit UiRenderList(std::size_t reserve = 256) { commands_.reserve(reserve); }

    void clear() noexcept { commands_.clear(); }
    void reserve(std::size_t count) { commands_.reserve(count); }
    std::size_t size() const noexcept { return commands_.size(); }
    bool empty() const noexcept { return commands_.empty(); }
    const std::vector<UiDrawCommand>& commands() const noexcept { return commands_; }

    void rect(Rect bounds, ThemeColor color, float rounding = 0.0f);
    void line(Vec2 from, Vec2 to, ThemeColor color, float thickness = 1.0f);
    void text(Vec2 position, std::string_view value, ThemeColor color);
    void image(Rect bounds, std::uint64_t texture, ThemeColor tint = ThemeColor{1, 1, 1, 1});
    void clip(Rect bounds);
};

struct VirtualRange {
    std::size_t first{0};
    std::size_t last{0};
    float contentExtent{0.0f};

    bool empty() const noexcept { return first >= last; }
};

// Computes the visible slice of a large list without allocating per-item UI
// nodes. Overscan keeps scrolling smooth while bounding work to O(visible).
VirtualRange visible_range(std::size_t itemCount, float itemExtent, float viewportOffset,
                           float viewportExtent, std::size_t overscan = 2) noexcept;

} // namespace shinkou::ui

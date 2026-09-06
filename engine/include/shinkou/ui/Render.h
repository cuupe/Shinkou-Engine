#pragma once

#include "shinkou/ui/Theme.h"
#include "shinkou/ui/Ui.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::ui {

// Backend-neutral retained commands. The editor renderer can translate this
// list to a native GPU batch without coupling widget/layout code to a desktop
// API. Clip commands are explicit so a backend can build a deterministic
// scissor stack instead of relying on the current window state.
enum class DrawCommandType : std::uint8_t {
    Rect,
    Border,
    Line,
    Text,
    Image,
    Path,
    Gradient,
    BeginClip,
    EndClip,
    // Kept as a compatibility spelling for older headless tests. New code
    // should use BeginClip/EndClip explicitly.
    Clip
};

enum class TextAlign : std::uint8_t { Start, Center, End };
enum class TextOverflow : std::uint8_t { Clip, Ellipsis };

struct UiDrawCommand {
    DrawCommandType type{DrawCommandType::Rect};
    Rect rect{};
    Vec2 from{};
    Vec2 to{};
    ThemeColor color{};
    ThemeColor secondary{};
    float thickness{1.0f};
    float rounding{0.0f};
    float fontSize{14.0f};
    TextAlign textAlign{TextAlign::Start};
    TextOverflow textOverflow{TextOverflow::Clip};
    std::string fontFamily{};
    std::uint64_t texture{0};
    std::string text{};
    // Immutable normalized vector geometry. Path points are mapped into
    // rect by the backend, so the same SVG geometry is reusable at any DPI.
    std::shared_ptr<const std::vector<Vec2>> pathPoints{};
    bool pathClosed{true};
    bool pathNormalized{true};
    bool pathFilled{true};
};

class UiRenderList final {
    std::vector<UiDrawCommand> commands_;
    float dpiScale_{1.0f};
    Rect dirtyRect_{};
    bool dirtyRectValid_{false};
    bool dirtyFull_{true};
    mutable std::uint64_t contentHash_{0};
    mutable bool contentHashValid_{false};

    void invalidate_hash() noexcept { contentHashValid_ = false; }

public:
    explicit UiRenderList(std::size_t reserve = 256) { commands_.reserve(reserve); }

    void clear() noexcept {
        commands_.clear();
        dirtyRect_ = {};
        dirtyRectValid_ = false;
        dirtyFull_ = true;
        invalidate_hash();
    }
    void set_dpi_scale(float value) noexcept {
        const float next = value > 0.0f ? value : 1.0f;
        if (dpiScale_ != next) { dpiScale_ = next; invalidate_hash(); }
    }
    float dpi_scale() const noexcept { return dpiScale_; }
    void reserve(std::size_t count) { commands_.reserve(count); }
    std::size_t size() const noexcept { return commands_.size(); }
    bool empty() const noexcept { return commands_.empty(); }
    const std::vector<UiDrawCommand>& commands() const noexcept { return commands_; }

    // Dirty bounds are in the same logical UI space as commands. Backends
    // can clip rasterization and upload only this rectangle after a hover,
    // press, or selection transition. Full repaint remains the safe default.
    void set_dirty_rect(Rect bounds, bool full = false) noexcept {
        dirtyRect_ = bounds;
        dirtyRectValid_ = bounds.width > 0.0f && bounds.height > 0.0f;
        dirtyFull_ = full;
    }
    bool has_dirty_rect() const noexcept { return dirtyRectValid_; }
    bool dirty_full() const noexcept { return dirtyFull_; }
    const Rect& dirty_rect() const noexcept { return dirtyRect_; }

    // Stable content identity for backend-side caching. The list remains
    // transient from the caller's perspective, but identical paint content
    // does not need to be rasterized and uploaded again.
    std::uint64_t content_hash() const noexcept;

    void rect(Rect bounds, ThemeColor color, float rounding = 0.0f);
    void border(Rect bounds, ThemeColor color, float thickness = 1.0f, float rounding = 0.0f);
    void line(Vec2 from, Vec2 to, ThemeColor color, float thickness = 1.0f);
    void text(Vec2 position, std::string_view value, ThemeColor color, float fontSize = 14.0f,
              std::string_view fontFamily = {}, TextAlign align = TextAlign::Start,
              TextOverflow overflow = TextOverflow::Clip);
    void text(Rect bounds, std::string_view value, ThemeColor color, float fontSize = 14.0f,
              std::string_view fontFamily = {}, TextAlign align = TextAlign::Start,
              TextOverflow overflow = TextOverflow::Clip);
    void image(Rect bounds, std::uint64_t texture, ThemeColor tint = ThemeColor{1, 1, 1, 1});
    void path(Rect bounds, std::shared_ptr<const std::vector<Vec2>> points,
              ThemeColor fill, ThemeColor stroke = {}, float thickness = 0.0f,
              bool closed = true, bool normalized = true);
    void begin_clip(Rect bounds);
    void end_clip();
    void clip(Rect bounds) { begin_clip(bounds); }
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

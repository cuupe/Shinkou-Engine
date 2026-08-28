#include "shinkou/ui/Render.h"

#include <algorithm>
#include <cmath>

namespace shinkou::ui {

void UiRenderList::rect(Rect bounds, ThemeColor color, float rounding) {
    UiDrawCommand command;
    command.type = DrawCommandType::Rect;
    command.rect = bounds;
    command.color = color;
    command.rounding = std::max(0.0f, rounding);
    commands_.push_back(std::move(command));
}

void UiRenderList::line(Vec2 from, Vec2 to, ThemeColor color, float thickness) {
    UiDrawCommand command;
    command.type = DrawCommandType::Line;
    command.from = from;
    command.to = to;
    command.color = color;
    command.thickness = std::max(0.0f, thickness);
    commands_.push_back(std::move(command));
}

void UiRenderList::text(Vec2 position, std::string_view value, ThemeColor color) {
    UiDrawCommand command;
    command.type = DrawCommandType::Text;
    command.from = position;
    command.color = color;
    command.text.assign(value);
    commands_.push_back(std::move(command));
}

void UiRenderList::image(Rect bounds, std::uint64_t texture, ThemeColor tint) {
    UiDrawCommand command;
    command.type = DrawCommandType::Image;
    command.rect = bounds;
    command.texture = texture;
    command.color = tint;
    commands_.push_back(std::move(command));
}

void UiRenderList::clip(Rect bounds) {
    UiDrawCommand command;
    command.type = DrawCommandType::Clip;
    command.rect = bounds;
    commands_.push_back(std::move(command));
}

VirtualRange visible_range(std::size_t itemCount, float itemExtent, float viewportOffset,
                           float viewportExtent, std::size_t overscan) noexcept {
    if (itemCount == 0 || !std::isfinite(itemExtent) || itemExtent <= 0.0f ||
        !std::isfinite(viewportOffset) || !std::isfinite(viewportExtent) || viewportExtent <= 0.0f) {
        return {0, 0, itemCount == 0 ? 0.0f : std::max(0.0f, itemExtent) * static_cast<float>(itemCount)};
    }
    const float clampedOffset = std::max(0.0f, viewportOffset);
    const std::size_t first = std::min(itemCount,
        static_cast<std::size_t>(std::floor(clampedOffset / itemExtent)) > overscan
            ? static_cast<std::size_t>(std::floor(clampedOffset / itemExtent)) - overscan : 0u);
    const auto visibleCount = static_cast<std::size_t>(std::ceil(viewportExtent / itemExtent));
    const std::size_t last = std::min(itemCount, first + visibleCount + overscan * 2u + 1u);
    return {first, last, itemExtent * static_cast<float>(itemCount)};
}

} // namespace shinkou::ui

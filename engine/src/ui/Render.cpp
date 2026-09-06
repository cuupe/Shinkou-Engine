#include "shinkou/ui/Render.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
}

template<class T>
void hash_value(std::uint64_t& hash, const T& value) noexcept {
    hash_bytes(hash, &value, sizeof(value));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    const auto size = static_cast<std::uint64_t>(value.size());
    hash_value(hash, size);
    hash_bytes(hash, value.data(), value.size());
}

void hash_float(std::uint64_t& hash, float value) noexcept {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    hash_value(hash, bits);
}

void hash_color(std::uint64_t& hash, const shinkou::ui::ThemeColor& color) noexcept {
    hash_float(hash, color.r);
    hash_float(hash, color.g);
    hash_float(hash, color.b);
    hash_float(hash, color.a);
}

} // namespace

namespace shinkou::ui {

std::uint64_t UiRenderList::content_hash() const noexcept {
    if (contentHashValid_) return contentHash_;
    std::uint64_t hash = kFnvOffset;
    hash_float(hash, dpiScale_);
    for (const auto& command : commands_) {
        hash_value(hash, command.type);
        hash_float(hash, command.rect.x);
        hash_float(hash, command.rect.y);
        hash_float(hash, command.rect.width);
        hash_float(hash, command.rect.height);
        hash_float(hash, command.from.x);
        hash_float(hash, command.from.y);
        hash_float(hash, command.to.x);
        hash_float(hash, command.to.y);
        hash_color(hash, command.color);
        hash_color(hash, command.secondary);
        hash_float(hash, command.thickness);
        hash_float(hash, command.rounding);
        hash_float(hash, command.fontSize);
        hash_value(hash, command.textAlign);
        hash_value(hash, command.textOverflow);
        hash_value(hash, command.texture);
        hash_string(hash, command.fontFamily);
        hash_string(hash, command.text);
        hash_value(hash, command.pathClosed);
        hash_value(hash, command.pathNormalized);
        hash_value(hash, command.pathFilled);
        if (command.pathPoints) {
            const auto count = static_cast<std::uint64_t>(command.pathPoints->size());
            hash_value(hash, count);
            for (const auto& point : *command.pathPoints) {
                hash_float(hash, point.x);
                hash_float(hash, point.y);
            }
        } else {
            hash_value(hash, static_cast<std::uint64_t>(0));
        }
    }
    const auto count = static_cast<std::uint64_t>(commands_.size());
    hash_value(hash, count);
    contentHash_ = hash;
    contentHashValid_ = true;
    return contentHash_;
}

void UiRenderList::rect(Rect bounds, ThemeColor color, float rounding) {
    UiDrawCommand command;
    command.type = DrawCommandType::Rect;
    command.rect = bounds;
    command.color = color;
    command.rounding = std::max(0.0f, rounding);
    commands_.push_back(std::move(command));
    invalidate_hash();
}

void UiRenderList::border(Rect bounds, ThemeColor color, float thickness, float rounding) {
    UiDrawCommand command;
    command.type = DrawCommandType::Border;
    command.rect = bounds;
    command.color = color;
    command.thickness = std::max(0.0f, thickness);
    command.rounding = std::max(0.0f, rounding);
    commands_.push_back(std::move(command));
    invalidate_hash();
}

void UiRenderList::line(Vec2 from, Vec2 to, ThemeColor color, float thickness) {
    UiDrawCommand command;
    command.type = DrawCommandType::Line;
    command.from = from;
    command.to = to;
    command.color = color;
    command.thickness = std::max(0.0f, thickness);
    commands_.push_back(std::move(command));
    invalidate_hash();
}

void UiRenderList::text(Vec2 position, std::string_view value, ThemeColor color, float fontSize,
                        std::string_view fontFamily, TextAlign align, TextOverflow overflow) {
    UiDrawCommand command;
    command.type = DrawCommandType::Text;
    command.from = position;
    command.color = color;
    command.fontSize = fontSize;
    command.textAlign = align;
    command.textOverflow = overflow;
    command.fontFamily.assign(fontFamily);
    command.text.assign(value);
    commands_.push_back(std::move(command));
    invalidate_hash();
}

void UiRenderList::text(Rect bounds, std::string_view value, ThemeColor color, float fontSize,
                        std::string_view fontFamily, TextAlign align, TextOverflow overflow) {
    UiDrawCommand command;
    command.type = DrawCommandType::Text;
    command.rect = bounds;
    command.from = {bounds.x, bounds.y};
    command.color = color;
    command.fontSize = fontSize;
    command.textAlign = align;
    command.textOverflow = overflow;
    command.fontFamily.assign(fontFamily);
    command.text.assign(value);
    commands_.push_back(std::move(command));
    invalidate_hash();
}

void UiRenderList::image(Rect bounds, std::uint64_t texture, ThemeColor tint) {
    UiDrawCommand command;
    command.type = DrawCommandType::Image;
    command.rect = bounds;
    command.texture = texture;
    command.color = tint;
    commands_.push_back(std::move(command));
    invalidate_hash();
}

void UiRenderList::path(Rect bounds, std::shared_ptr<const std::vector<Vec2>> points,
                        ThemeColor fill, ThemeColor stroke, float thickness,
                        bool closed, bool normalized) {
    UiDrawCommand command;
    command.type = DrawCommandType::Path;
    command.rect = bounds;
    command.color = fill;
    command.secondary = stroke;
    command.thickness = std::max(0.0f, thickness);
    command.pathPoints = std::move(points);
    command.pathClosed = closed;
    command.pathNormalized = normalized;
    command.pathFilled = fill.a > 0.0f;
    commands_.push_back(std::move(command));
    invalidate_hash();
}

void UiRenderList::begin_clip(Rect bounds) {
    UiDrawCommand command;
    command.type = DrawCommandType::BeginClip;
    command.rect = bounds;
    commands_.push_back(std::move(command));
    invalidate_hash();
}

void UiRenderList::end_clip() {
    UiDrawCommand command;
    command.type = DrawCommandType::EndClip;
    commands_.push_back(std::move(command));
    invalidate_hash();
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

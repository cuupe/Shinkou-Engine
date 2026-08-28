#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace shinkou::ui {

using WidgetId = std::uint64_t;
constexpr WidgetId InvalidWidgetId = 0;
constexpr float AutoSize = -1.0f;
constexpr float InfiniteSize = std::numeric_limits<float>::infinity();

struct Size {
    float width{0.0f};
    float height{0.0f};

    constexpr Size() noexcept = default;
    constexpr Size(float widthValue, float heightValue) noexcept : width(widthValue), height(heightValue) {}
};

struct Vec2 {
    float x{0.0f};
    float y{0.0f};

    constexpr Vec2() noexcept = default;
    constexpr Vec2(float xValue, float yValue) noexcept : x(xValue), y(yValue) {}
};

struct Insets {
    float left{0.0f};
    float top{0.0f};
    float right{0.0f};
    float bottom{0.0f};

    constexpr Insets() noexcept = default;
    constexpr explicit Insets(float all) noexcept : left(all), top(all), right(all), bottom(all) {}
    constexpr Insets(float horizontal, float vertical) noexcept
        : left(horizontal), top(vertical), right(horizontal), bottom(vertical) {}
    constexpr Insets(float leftValue, float topValue, float rightValue, float bottomValue) noexcept
        : left(leftValue), top(topValue), right(rightValue), bottom(bottomValue) {}

    constexpr float horizontal() const noexcept { return left + right; }
    constexpr float vertical() const noexcept { return top + bottom; }
};
using EdgeInsets = Insets;

struct Rect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};

    constexpr Rect() noexcept = default;
    constexpr Rect(float xValue, float yValue, float widthValue, float heightValue) noexcept
        : x(xValue), y(yValue), width(widthValue), height(heightValue) {}

    constexpr float right() const noexcept { return x + width; }
    constexpr float bottom() const noexcept { return y + height; }
    constexpr bool contains(Vec2 point) const noexcept {
        return point.x >= x && point.y >= y && point.x < right() && point.y < bottom();
    }
    constexpr Rect inset(Insets padding) const noexcept {
        return {x + padding.left, y + padding.top,
                std::max(0.0f, width - padding.horizontal()),
                std::max(0.0f, height - padding.vertical())};
    }
};

enum class Layout {
    FlexRow,
    FlexColumn,
    Overlay
};
using LayoutType = Layout;
using LayoutMode = Layout;

enum class UiEventType {
    PointerMove,
    PointerDown,
    PointerUp,
    PointerCancel,
    Scroll,
    KeyDown,
    KeyUp,
    TextInput,
    FocusGained,
    FocusLost
};

enum class PointerButton : std::uint8_t {
    None,
    Left,
    Middle,
    Right,
    X1,
    X2
};

enum class EventResult {
    Continue,
    Handled,
    Stop
};

struct UiEvent {
    UiEventType type{UiEventType::PointerMove};
    PointerButton button{PointerButton::None};
    Vec2 position{};
    Vec2 delta{};
    std::int32_t key{0};
    std::uint32_t modifiers{0};
    char32_t character{U'\0'};
    std::string control{};
    std::string text{};
    bool repeat{false};
    WidgetId target{InvalidWidgetId};
    WidgetId currentTarget{InvalidWidgetId};
    bool handled{false};
    bool stopPropagation{false};

    constexpr bool is_pointer_event() const noexcept {
        return type == UiEventType::PointerMove || type == UiEventType::PointerDown ||
               type == UiEventType::PointerUp || type == UiEventType::PointerCancel ||
               type == UiEventType::Scroll;
    }
};

struct LayoutStyle {
    // A negative size is automatic. `size` is the convenient spelling; when
    // left automatic, preferredSize is used as the compatibility spelling.
    Size size{AutoSize, AutoSize};
    Size preferredSize{AutoSize, AutoSize};
    Size minSize{0.0f, 0.0f};
    Size maxSize{InfiniteSize, InfiniteSize};
    Insets padding{};
    Insets margin{};
    Layout layout{Layout::FlexColumn};
    float flex{0.0f};
    float flexGrow{0.0f};
    bool hitTestVisible{true};
    bool focusable{false};
};
using WidgetStyle = LayoutStyle;

struct FrameStats {
    std::uint32_t widgetCount{0};
    std::uint32_t dirtyWidgetCount{0};
    std::uint32_t layoutNodeCount{0};
    std::uint32_t hitTestCount{0};
    std::uint32_t dispatchedEventCount{0};
    bool layoutRebuilt{false};

    // Short aliases make the struct convenient in an editor overlay.
    std::uint32_t widgets{0};
    std::uint32_t layouts{0};
    std::uint32_t hitTests{0};
    std::uint32_t events{0};
};

using EventHandler = std::function<EventResult(WidgetId, UiEvent&)>;

struct Widget {
    WidgetId id{InvalidWidgetId};
    WidgetId parent{InvalidWidgetId};
    std::vector<WidgetId> children;
    LayoutStyle style{};
    Rect rect{};
    bool visible{true};
    bool enabled{true};

private:
    friend class UiRuntime;
    EventHandler eventHandler;
    bool dirty{true};
    bool subtreeDirty{true};
};

class UiRuntime final {
public:
    UiRuntime();
    ~UiRuntime();

    UiRuntime(const UiRuntime&) = delete;
    UiRuntime& operator=(const UiRuntime&) = delete;
    UiRuntime(UiRuntime&&) noexcept = delete;
    UiRuntime& operator=(UiRuntime&&) noexcept = delete;

    WidgetId root() const noexcept { return rootId_; }

    WidgetId create_widget(WidgetId parent = InvalidWidgetId, const LayoutStyle& style = {});
    WidgetId create(WidgetId parent = InvalidWidgetId, const LayoutStyle& style = {}) {
        return create_widget(parent, style);
    }
    bool destroy_widget(WidgetId id);
    bool destroy(WidgetId id) { return destroy_widget(id); }
    bool reparent(WidgetId id, WidgetId newParent);

    const Widget* widget(WidgetId id) const noexcept;
    Widget* widget(WidgetId id) noexcept;
    const Widget* find(WidgetId id) const noexcept { return widget(id); }
    Widget* find(WidgetId id) noexcept { return widget(id); }
    const std::vector<WidgetId>& children(WidgetId id) const noexcept;

    bool set_style(WidgetId id, const LayoutStyle& style);
    bool set_layout(WidgetId id, Layout layout);
    bool set_padding(WidgetId id, Insets padding);
    bool set_margin(WidgetId id, Insets margin);
    bool set_size(WidgetId id, Size size);
    bool set_min_size(WidgetId id, Size size);
    bool set_max_size(WidgetId id, Size size);
    bool set_flex(WidgetId id, float flex);
    bool set_visible(WidgetId id, bool visible);
    bool set_enabled(WidgetId id, bool enabled);
    bool set_focusable(WidgetId id, bool focusable);

    void mark_dirty(WidgetId id);
    void invalidate(WidgetId id) { mark_dirty(id); }
    bool is_dirty(WidgetId id) const noexcept;
    bool is_subtree_dirty(WidgetId id) const noexcept;

    void begin_frame() noexcept;
    void layout(Size viewport);
    void layout(Rect viewport);
    const FrameStats& frame_stats() const noexcept { return stats_; }
    const FrameStats& stats() const noexcept { return stats_; }

    WidgetId hit_test(Vec2 point) const;
    WidgetId hit_test(float x, float y) const { return hit_test({x, y}); }

    bool capture_pointer(WidgetId id);
    void release_pointer(WidgetId id = InvalidWidgetId) noexcept;
    WidgetId captured_pointer() const noexcept { return capturedId_; }

    bool focus(WidgetId id);
    void clear_focus() noexcept;
    WidgetId focused() const noexcept { return focusedId_; }

    template<class Handler>
    bool set_event_handler(WidgetId id, Handler&& handler) {
        Widget* target = widget(id);
        if (!target) return false;
        target->eventHandler = make_handler(std::forward<Handler>(handler));
        mark_dirty(id);
        return true;
    }

    bool set_event_handler(WidgetId id, EventHandler handler);

    // Pointer events select a hit target (or the capture owner), keyboard and
    // text events select the focused widget. Events bubble from target to root.
    bool dispatch(UiEvent& event);
    bool dispatch_event(UiEvent& event) { return dispatch(event); }
    bool route_event(WidgetId target, UiEvent& event);

private:
    template<class Handler>
    static EventHandler make_handler(Handler&& handler) {
        using Callable = std::decay_t<Handler>;
        return [callable = Callable(std::forward<Handler>(handler))](WidgetId id, UiEvent& event) mutable {
            if constexpr (std::is_invocable_r_v<EventResult, Callable&, WidgetId, UiEvent&>) {
                return callable(id, event);
            } else if constexpr (std::is_invocable_r_v<bool, Callable&, WidgetId, UiEvent&>) {
                return callable(id, event) ? EventResult::Handled : EventResult::Continue;
            } else if constexpr (std::is_invocable_v<Callable&, WidgetId, UiEvent&>) {
                callable(id, event);
                return EventResult::Continue;
            } else if constexpr (std::is_invocable_r_v<EventResult, Callable&, UiEvent&>) {
                return callable(event);
            } else if constexpr (std::is_invocable_r_v<bool, Callable&, UiEvent&>) {
                return callable(event) ? EventResult::Handled : EventResult::Continue;
            } else {
                static_assert(std::is_invocable_v<Callable&, WidgetId, UiEvent&>,
                              "UI event handlers must accept (WidgetId, UiEvent&) or (UiEvent&)");
            }
        };
    }

    struct Measure { float width{0.0f}; float height{0.0f}; };

    std::unordered_map<WidgetId, Widget> widgets_;
    WidgetId rootId_{InvalidWidgetId};
    WidgetId nextId_{1};
    WidgetId focusedId_{InvalidWidgetId};
    WidgetId capturedId_{InvalidWidgetId};
    Size lastViewport_{AutoSize, AutoSize};
    FrameStats stats_{};

    Widget* find_mutable(WidgetId id) noexcept;
    static float finite_non_negative(float value) noexcept;
    static float resolve_dimension(float explicitValue, float preferredValue, float intrinsic,
                                   float minimum, float maximum) noexcept;
    static Measure measure(const Widget& widget, const std::unordered_map<WidgetId, Widget>& widgets);
    static bool visible_enabled(const Widget& widget) noexcept;
    bool is_descendant(WidgetId ancestor, WidgetId candidate) const noexcept;
    void detach_from_parent(WidgetId id);
    void destroy_subtree(WidgetId id);
    void arrange(Widget& widget, const Rect& rect);
    void clear_dirty(Widget& widget);
    std::uint32_t count_dirty(const Widget& widget) const;
    bool route_to_target(WidgetId target, UiEvent& event);
    bool dispatch_focus_event(WidgetId target, UiEventType type);
};

using Ui = UiRuntime;

} // namespace shinkou::ui

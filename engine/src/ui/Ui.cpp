#include "shinkou/ui/Ui.h"

#include <cmath>
#include <limits>

namespace shinkou::ui {
namespace {

constexpr float Epsilon = 0.0001f;

float non_negative(float value) noexcept {
    return std::isfinite(value) ? std::max(0.0f, value) : 0.0f;
}

bool approximately_equal(float a, float b) noexcept {
    return std::abs(a - b) <= Epsilon;
}

bool approximately_equal(Size a, Size b) noexcept {
    return approximately_equal(a.width, b.width) && approximately_equal(a.height, b.height);
}

} // namespace

UiRuntime::UiRuntime() {
    Widget rootWidget;
    rootWidget.id = nextId_++;
    rootWidget.style.layout = Layout::Overlay;
    rootWidget.style.hitTestVisible = false;
    widgets_.emplace(rootWidget.id, std::move(rootWidget));
    rootId_ = rootWidget.id;
}

UiRuntime::~UiRuntime() = default;

Widget* UiRuntime::find_mutable(WidgetId id) noexcept {
    const auto it = widgets_.find(id);
    return it == widgets_.end() ? nullptr : &it->second;
}

const Widget* UiRuntime::widget(WidgetId id) const noexcept {
    const auto it = widgets_.find(id);
    return it == widgets_.end() ? nullptr : &it->second;
}

Widget* UiRuntime::widget(WidgetId id) noexcept {
    return find_mutable(id);
}

const std::vector<WidgetId>& UiRuntime::children(WidgetId id) const noexcept {
    static const std::vector<WidgetId> empty;
    const Widget* target = widget(id);
    return target ? target->children : empty;
}

WidgetId UiRuntime::create_widget(WidgetId parent, const LayoutStyle& style) {
    if (parent == InvalidWidgetId) parent = rootId_;
    Widget* parentWidget = find_mutable(parent);
    if (!parentWidget) return InvalidWidgetId;

    Widget child;
    child.id = nextId_++;
    child.parent = parent;
    child.style = style;
    child.style.flex = std::max(0.0f, std::isfinite(child.style.flex) ? child.style.flex : 0.0f);
    child.style.flexGrow = std::max(0.0f, std::isfinite(child.style.flexGrow) ? child.style.flexGrow : 0.0f);
    widgets_.emplace(child.id, std::move(child));
    parentWidget->children.push_back(child.id);
    mark_dirty(parent);
    return child.id;
}

void UiRuntime::detach_from_parent(WidgetId id) {
    Widget* target = find_mutable(id);
    if (!target || target->parent == InvalidWidgetId) return;
    Widget* parentWidget = find_mutable(target->parent);
    if (parentWidget) {
        const auto it = std::find(parentWidget->children.begin(), parentWidget->children.end(), id);
        if (it != parentWidget->children.end()) parentWidget->children.erase(it);
        mark_dirty(parentWidget->id);
    }
    target->parent = InvalidWidgetId;
}

bool UiRuntime::reparent(WidgetId id, WidgetId newParent) {
    if (id == InvalidWidgetId || id == rootId_ || newParent == InvalidWidgetId) return false;
    if (!find_mutable(id) || !find_mutable(newParent) || id == newParent || is_descendant(id, newParent)) return false;
    detach_from_parent(id);
    Widget* target = find_mutable(id);
    Widget* parentWidget = find_mutable(newParent);
    target->parent = newParent;
    parentWidget->children.push_back(id);
    mark_dirty(id);
    mark_dirty(newParent);
    return true;
}

void UiRuntime::destroy_subtree(WidgetId id) {
    Widget* target = find_mutable(id);
    if (!target) return;
    const std::vector<WidgetId> descendants = target->children;
    for (const WidgetId child : descendants) destroy_subtree(child);
    widgets_.erase(id);
}

bool UiRuntime::destroy_widget(WidgetId id) {
    if (id == InvalidWidgetId || id == rootId_ || !find_mutable(id)) return false;
    if (is_descendant(id, focusedId_)) focusedId_ = InvalidWidgetId;
    if (is_descendant(id, capturedId_)) capturedId_ = InvalidWidgetId;
    Widget* target = find_mutable(id);
    const WidgetId parent = target->parent;
    detach_from_parent(id);
    destroy_subtree(id);
    if (parent != InvalidWidgetId) mark_dirty(parent);
    return true;
}

bool UiRuntime::set_style(WidgetId id, const LayoutStyle& style) {
    Widget* target = find_mutable(id);
    if (!target) return false;
    target->style = style;
    target->style.flex = std::max(0.0f, std::isfinite(target->style.flex) ? target->style.flex : 0.0f);
    target->style.flexGrow = std::max(0.0f, std::isfinite(target->style.flexGrow) ? target->style.flexGrow : 0.0f);
    mark_dirty(id);
    return true;
}

bool UiRuntime::set_layout(WidgetId id, Layout value) {
    Widget* target = find_mutable(id);
    if (!target) return false;
    if (target->style.layout != value) { target->style.layout = value; mark_dirty(id); }
    return true;
}

bool UiRuntime::set_padding(WidgetId id, Insets value) {
    Widget* target = find_mutable(id);
    if (!target) return false;
    target->style.padding = value;
    mark_dirty(id);
    return true;
}

bool UiRuntime::set_margin(WidgetId id, Insets value) {
    Widget* target = find_mutable(id);
    if (!target) return false;
    target->style.margin = value;
    mark_dirty(id);
    return true;
}

bool UiRuntime::set_size(WidgetId id, Size value) {
    Widget* target = find_mutable(id);
    if (!target) return false;
    target->style.size = value;
    mark_dirty(id);
    return true;
}

bool UiRuntime::set_min_size(WidgetId id, Size value) {
    Widget* target = find_mutable(id);
    if (!target) return false;
    target->style.minSize = value;
    mark_dirty(id);
    return true;
}

bool UiRuntime::set_max_size(WidgetId id, Size value) {
    Widget* target = find_mutable(id);
    if (!target) return false;
    target->style.maxSize = value;
    mark_dirty(id);
    return true;
}

bool UiRuntime::set_flex(WidgetId id, float value) {
    Widget* target = find_mutable(id);
    if (!target) return false;
    target->style.flex = std::max(0.0f, std::isfinite(value) ? value : 0.0f);
    mark_dirty(id);
    return true;
}

bool UiRuntime::set_visible(WidgetId id, bool value) {
    Widget* target = find_mutable(id);
    if (!target) return false;
    target->visible = value;
    mark_dirty(id);
    if (!value && focusedId_ == id) clear_focus();
    if (!value && capturedId_ == id) release_pointer(id);
    return true;
}

bool UiRuntime::set_enabled(WidgetId id, bool value) {
    Widget* target = find_mutable(id);
    if (!target) return false;
    target->enabled = value;
    mark_dirty(id);
    if (!value && focusedId_ == id) clear_focus();
    if (!value && capturedId_ == id) release_pointer(id);
    return true;
}

bool UiRuntime::set_focusable(WidgetId id, bool value) {
    Widget* target = find_mutable(id);
    if (!target) return false;
    target->style.focusable = value;
    mark_dirty(id);
    if (!value && focusedId_ == id) clear_focus();
    return true;
}

void UiRuntime::mark_dirty(WidgetId id) {
    Widget* target = find_mutable(id);
    if (!target) return;
    target->dirty = true;
    for (WidgetId current = id; current != InvalidWidgetId;) {
        Widget* node = find_mutable(current);
        if (!node) break;
        node->subtreeDirty = true;
        current = node->parent;
    }
}

bool UiRuntime::is_dirty(WidgetId id) const noexcept {
    const Widget* target = widget(id);
    return target ? target->dirty : false;
}

bool UiRuntime::is_subtree_dirty(WidgetId id) const noexcept {
    const Widget* target = widget(id);
    return target ? target->subtreeDirty : false;
}

void UiRuntime::begin_frame() noexcept {
    stats_ = {};
    stats_.widgetCount = static_cast<std::uint32_t>(widgets_.size());
    stats_.widgets = stats_.widgetCount;
}

float UiRuntime::finite_non_negative(float value) noexcept {
    return std::isfinite(value) ? std::max(0.0f, value) : 0.0f;
}

float UiRuntime::resolve_dimension(float explicitValue, float preferredValue, float intrinsic,
                                   float minimum, float maximum) noexcept {
    const float wanted = explicitValue >= 0.0f ? explicitValue :
                         (preferredValue >= 0.0f ? preferredValue : intrinsic);
    const float low = finite_non_negative(minimum);
    const float high = std::isfinite(maximum) ? std::max(low, maximum) : InfiniteSize;
    return std::clamp(finite_non_negative(wanted), low, high);
}

UiRuntime::Measure UiRuntime::measure(const Widget& widget, const std::unordered_map<WidgetId, Widget>& widgets) {
    float contentWidth = 0.0f;
    float contentHeight = 0.0f;
    bool first = true;
    for (const WidgetId childId : widget.children) {
        const auto it = widgets.find(childId);
        if (it == widgets.end() || !it->second.visible) continue;
        const Measure child = measure(it->second, widgets);
        const Insets margin = it->second.style.margin;
        const float childWidth = child.width + finite_non_negative(margin.horizontal());
        const float childHeight = child.height + finite_non_negative(margin.vertical());
        if (widget.style.layout == Layout::FlexRow) {
            contentWidth += childWidth;
            contentHeight = std::max(contentHeight, childHeight);
        } else if (widget.style.layout == Layout::FlexColumn) {
            contentWidth = std::max(contentWidth, childWidth);
            contentHeight += childHeight;
        } else {
            contentWidth = std::max(contentWidth, childWidth);
            contentHeight = std::max(contentHeight, childHeight);
        }
        first = false;
    }
    if (first) { contentWidth = 0.0f; contentHeight = 0.0f; }
    const Size intrinsic{
        contentWidth + finite_non_negative(widget.style.padding.horizontal()),
        contentHeight + finite_non_negative(widget.style.padding.vertical())};
    return {
        resolve_dimension(widget.style.size.width, widget.style.preferredSize.width, intrinsic.width,
                          widget.style.minSize.width, widget.style.maxSize.width),
        resolve_dimension(widget.style.size.height, widget.style.preferredSize.height, intrinsic.height,
                          widget.style.minSize.height, widget.style.maxSize.height)};
}

bool UiRuntime::visible_enabled(const Widget& widget) noexcept {
    return widget.visible && widget.enabled;
}

void UiRuntime::layout(Size viewport) {
    layout(Rect{0.0f, 0.0f, finite_non_negative(viewport.width), finite_non_negative(viewport.height)});
}

void UiRuntime::layout(Rect viewport) {
    stats_.widgetCount = static_cast<std::uint32_t>(widgets_.size());
    stats_.widgets = stats_.widgetCount;
    if (!approximately_equal(lastViewport_, Size{viewport.width, viewport.height})) mark_dirty(rootId_);
    stats_.dirtyWidgetCount = count_dirty(widgets_.at(rootId_));
    Widget& rootWidget = widgets_.at(rootId_);
    if (!rootWidget.subtreeDirty) return;
    stats_.layoutRebuilt = true;
    arrange(rootWidget, viewport);
    clear_dirty(rootWidget);
    lastViewport_ = {viewport.width, viewport.height};
    stats_.layouts = stats_.layoutNodeCount;
}

void UiRuntime::arrange(Widget& widget, const Rect& rect) {
    widget.rect = rect;
    ++stats_.layoutNodeCount;
    const Rect content = rect.inset(widget.style.padding);
    struct ChildLayout {
        Widget* widget{nullptr};
        Measure intrinsic{};
        float main{0.0f};
        float cross{0.0f};
        float mainMarginStart{0.0f};
        float mainMarginEnd{0.0f};
        float crossMarginStart{0.0f};
        float crossMarginEnd{0.0f};
        float flex{0.0f};
    };
    std::vector<ChildLayout> children;
    children.reserve(widget.children.size());
    const bool row = widget.style.layout == Layout::FlexRow;
    for (const WidgetId childId : widget.children) {
        Widget* child = find_mutable(childId);
        if (!child || !child->visible) continue;
        const Measure intrinsic = measure(*child, widgets_);
        const Insets margin = child->style.margin;
        const float mainExplicit = row ? child->style.size.width : child->style.size.height;
        const float mainPreferred = row ? child->style.preferredSize.width : child->style.preferredSize.height;
        const float crossExplicit = row ? child->style.size.height : child->style.size.width;
        const float crossPreferred = row ? child->style.preferredSize.height : child->style.preferredSize.width;
        const float minMain = row ? child->style.minSize.width : child->style.minSize.height;
        const float maxMain = row ? child->style.maxSize.width : child->style.maxSize.height;
        const float minCross = row ? child->style.minSize.height : child->style.minSize.width;
        const float maxCross = row ? child->style.maxSize.height : child->style.maxSize.width;
        ChildLayout item;
        item.widget = child;
        item.intrinsic = intrinsic;
        item.main = resolve_dimension(mainExplicit, mainPreferred, row ? intrinsic.width : intrinsic.height, minMain, maxMain);
        item.cross = resolve_dimension(crossExplicit, crossPreferred, row ? intrinsic.height : intrinsic.width, minCross, maxCross);
        item.flex = std::max(0.0f, std::max(child->style.flex, child->style.flexGrow));
        if (row) {
            item.mainMarginStart = non_negative(margin.left);
            item.mainMarginEnd = non_negative(margin.right);
            item.crossMarginStart = non_negative(margin.top);
            item.crossMarginEnd = non_negative(margin.bottom);
        } else {
            item.mainMarginStart = non_negative(margin.top);
            item.mainMarginEnd = non_negative(margin.bottom);
            item.crossMarginStart = non_negative(margin.left);
            item.crossMarginEnd = non_negative(margin.right);
        }
        children.push_back(item);
    }

    if (widget.style.layout == Layout::Overlay) {
        for (ChildLayout& item : children) {
            const Insets margin = item.widget->style.margin;
            const float availableWidth = std::max(0.0f, content.width - non_negative(margin.left) - non_negative(margin.right));
            const float availableHeight = std::max(0.0f, content.height - non_negative(margin.top) - non_negative(margin.bottom));
            const float width = resolve_dimension(item.widget->style.size.width, item.widget->style.preferredSize.width,
                                                 availableWidth, item.widget->style.minSize.width, item.widget->style.maxSize.width);
            const float height = resolve_dimension(item.widget->style.size.height, item.widget->style.preferredSize.height,
                                                   availableHeight, item.widget->style.minSize.height, item.widget->style.maxSize.height);
            arrange(*item.widget, {content.x + non_negative(margin.left), content.y + non_negative(margin.top), width, height});
        }
        return;
    }

    const float availableMain = row ? content.width : content.height;
    float occupied = 0.0f;
    float flexTotal = 0.0f;
    for (const ChildLayout& item : children) {
        occupied += item.main + item.mainMarginStart + item.mainMarginEnd;
        flexTotal += item.flex;
    }
    const float extra = std::max(0.0f, availableMain - occupied);
    for (ChildLayout& item : children) {
        if (item.flex > 0.0f && flexTotal > 0.0f) {
            const float minMain = row ? item.widget->style.minSize.width : item.widget->style.minSize.height;
            const float maxMain = row ? item.widget->style.maxSize.width : item.widget->style.maxSize.height;
            item.main = resolve_dimension(item.main + extra * (item.flex / flexTotal), AutoSize, item.main,
                                          minMain, maxMain);
        }
        const float availableCross = std::max(0.0f, (row ? content.height : content.width) -
                                              item.crossMarginStart - item.crossMarginEnd);
        const bool explicitCross = (row ? item.widget->style.size.height : item.widget->style.size.width) >= 0.0f ||
                                    (row ? item.widget->style.preferredSize.height : item.widget->style.preferredSize.width) >= 0.0f;
        if (!explicitCross) {
            const float minCross = row ? item.widget->style.minSize.height : item.widget->style.minSize.width;
            const float maxCross = row ? item.widget->style.maxSize.height : item.widget->style.maxSize.width;
            item.cross = resolve_dimension(availableCross, AutoSize, availableCross, minCross, maxCross);
        }
    }

    float cursor = row ? content.x : content.y;
    for (ChildLayout& item : children) {
        cursor += item.mainMarginStart;
        const float crossStart = (row ? content.y : content.x) + item.crossMarginStart;
        const Rect childRect = row
            ? Rect{cursor, crossStart, item.main, item.cross}
            : Rect{crossStart, cursor, item.cross, item.main};
        arrange(*item.widget, childRect);
        cursor += item.main + item.mainMarginEnd;
    }
}

void UiRuntime::clear_dirty(Widget& widget) {
    widget.dirty = false;
    widget.subtreeDirty = false;
    for (const WidgetId childId : widget.children) {
        Widget* child = find_mutable(childId);
        if (child) clear_dirty(*child);
    }
}

std::uint32_t UiRuntime::count_dirty(const Widget& widget) const {
    std::uint32_t count = widget.dirty ? 1u : 0u;
    for (const WidgetId childId : widget.children) {
        const Widget* child = this->widget(childId);
        if (child) count += count_dirty(*child);
    }
    return count;
}

WidgetId UiRuntime::hit_test(Vec2 point) const {
    ++const_cast<UiRuntime*>(this)->stats_.hitTestCount;
    const_cast<UiRuntime*>(this)->stats_.hitTests = stats_.hitTestCount;
    std::function<WidgetId(WidgetId)> visit = [&](WidgetId id) -> WidgetId {
        const Widget* target = widget(id);
        if (!target || !visible_enabled(*target) || !target->rect.contains(point)) return InvalidWidgetId;
        for (auto it = target->children.rbegin(); it != target->children.rend(); ++it) {
            const WidgetId hit = visit(*it);
            if (hit != InvalidWidgetId) return hit;
        }
        if (target->style.hitTestVisible) return id;
        return InvalidWidgetId;
    };
    const WidgetId result = visit(rootId_);
    return result == rootId_ ? InvalidWidgetId : result;
}

bool UiRuntime::capture_pointer(WidgetId id) {
    Widget* target = find_mutable(id);
    if (!target || !visible_enabled(*target)) return false;
    capturedId_ = id;
    return true;
}

void UiRuntime::release_pointer(WidgetId id) noexcept {
    if (id == InvalidWidgetId || capturedId_ == id) capturedId_ = InvalidWidgetId;
}

bool UiRuntime::dispatch_focus_event(WidgetId target, UiEventType type) {
    Widget* node = find_mutable(target);
    if (!node || !node->eventHandler) return false;
    UiEvent event;
    event.type = type;
    event.target = target;
    event.currentTarget = target;
    ++stats_.dispatchedEventCount;
    stats_.events = stats_.dispatchedEventCount;
    const EventResult result = node->eventHandler(target, event);
    event.handled = result != EventResult::Continue;
    return event.handled;
}

bool UiRuntime::focus(WidgetId id) {
    Widget* target = find_mutable(id);
    if (!target || !visible_enabled(*target) || !target->style.focusable) return false;
    if (focusedId_ == id) return true;
    const WidgetId old = focusedId_;
    focusedId_ = id;
    if (old != InvalidWidgetId) dispatch_focus_event(old, UiEventType::FocusLost);
    dispatch_focus_event(id, UiEventType::FocusGained);
    return true;
}

void UiRuntime::clear_focus() noexcept {
    const WidgetId old = focusedId_;
    focusedId_ = InvalidWidgetId;
    if (old != InvalidWidgetId) const_cast<UiRuntime*>(this)->dispatch_focus_event(old, UiEventType::FocusLost);
}

bool UiRuntime::set_event_handler(WidgetId id, EventHandler handler) {
    Widget* target = find_mutable(id);
    if (!target) return false;
    target->eventHandler = std::move(handler);
    mark_dirty(id);
    return true;
}

bool UiRuntime::is_descendant(WidgetId ancestor, WidgetId candidate) const noexcept {
    if (ancestor == InvalidWidgetId || candidate == InvalidWidgetId) return false;
    WidgetId current = candidate;
    while (current != InvalidWidgetId) {
        if (current == ancestor) return true;
        const Widget* node = widget(current);
        if (!node) break;
        current = node->parent;
    }
    return false;
}

bool UiRuntime::route_to_target(WidgetId target, UiEvent& event) {
    if (!find_mutable(target)) return false;
    event.target = target;
    WidgetId current = target;
    bool handled = false;
    while (current != InvalidWidgetId) {
        Widget* node = find_mutable(current);
        if (!node) break;
        event.currentTarget = current;
        if (node->eventHandler) {
            ++stats_.dispatchedEventCount;
            stats_.events = stats_.dispatchedEventCount;
            const EventResult result = node->eventHandler(current, event);
            if (result == EventResult::Handled) { event.handled = true; handled = true; }
            if (result == EventResult::Stop) { event.stopPropagation = true; handled = true; }
        }
        if (event.stopPropagation || (event.handled && current == target)) break;
        current = node->parent;
    }
    return handled || event.handled;
}

bool UiRuntime::route_event(WidgetId target, UiEvent& event) {
    return route_to_target(target, event);
}

bool UiRuntime::dispatch(UiEvent& event) {
    WidgetId target = InvalidWidgetId;
    if (event.is_pointer_event()) {
        target = capturedId_ != InvalidWidgetId ? capturedId_ : hit_test(event.position);
    } else {
        target = focusedId_ != InvalidWidgetId ? focusedId_ : rootId_;
    }
    const bool handled = target != InvalidWidgetId && route_to_target(target, event);
    if (event.type == UiEventType::PointerDown && target != InvalidWidgetId) {
        const Widget* targetWidget = widget(target);
        if (targetWidget && targetWidget->style.focusable) focus(target);
        else clear_focus();
    }
    if (event.type == UiEventType::PointerUp || event.type == UiEventType::PointerCancel) release_pointer();
    return handled;
}

} // namespace shinkou::ui

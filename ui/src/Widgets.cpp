#include "shinkou/uikit/Widgets.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace shinkou::uikit {
namespace {

Color style_color(const StyleSheet& style, const std::string& selector, const std::string& state, const std::string& property, Color fallback) {
    const std::string value = style.property(selector, state, property);
    if (value.empty()) return fallback;
    return style.color(value, Color::from_hex(value));
}

float style_number(const StyleSheet& style, const std::string& selector, const std::string& state, const std::string& property, float fallback) {
    const std::string value = style.property(selector, state, property);
    if (value.empty()) return fallback;
    char* end = nullptr;
    const float parsed = std::strtof(value.c_str(), &end);
    return end == value.c_str() ? fallback : parsed;
}

Insets style_radii(const StyleSheet& style, const std::string& selector, const std::string& state, const ControlStyle& fallback) {
    const float radius = style_number(style, selector, state, "radius", fallback.radii.topLeft);
    return {
        style_number(style, selector, state, "radius-top-left", radius),
        style_number(style, selector, state, "radius-top-right", radius),
        style_number(style, selector, state, "radius-bottom-right", radius),
        style_number(style, selector, state, "radius-bottom-left", radius)
    };
}

}

WidgetId Widget::s_nextId = 1;

Widget::Widget() : m_id(s_nextId++) {}

void Widget::add(std::unique_ptr<Widget> child) {
    if (!child) return;
    child->m_parent = this;
    m_children.push_back(std::move(child));
}

void Widget::clear() {
    m_children.clear();
}

std::unique_ptr<Widget> Widget::remove(WidgetId target) {
    const auto found = std::find_if(m_children.begin(), m_children.end(), [target](const std::unique_ptr<Widget>& child) { return child && child->id() == target; });
    if (found == m_children.end()) {
        for (auto& child : m_children) if (child) if (auto removed = child->remove(target)) return removed;
        return nullptr;
    }
    (*found)->m_parent = nullptr;
    std::unique_ptr<Widget> result = std::move(*found);
    m_children.erase(found);
    return result;
}

void Widget::serialize(XmlNode& node) const {
    node.name = type_name();
    node.attributes["id"] = std::to_string(m_id);
    node.attributes["style"] = styleClass;
    node.attributes["variant"] = styleVariant;
    node.attributes["name"] = name;
    node.attributes["visible"] = visible ? "true" : "false";
    node.attributes["enabled"] = enabled ? "true" : "false";
    node.attributes["focusable"] = focusable ? "true" : "false";
    node.attributes["hit-test"] = hitTestVisible ? "true" : "false";
    node.attributes["tab-index"] = std::to_string(tabIndex);
    node.attributes["selected"] = selected ? "true" : "false";
    node.attributes["checked"] = checked ? "true" : "false";
    node.attributes["flex"] = std::to_string(flex);
    node.attributes["gap"] = std::to_string(gap);
    node.attributes["padding"] = std::to_string(padding.left) + "," + std::to_string(padding.top) + "," + std::to_string(padding.right) + "," + std::to_string(padding.bottom);
    for (const auto& child : m_children) { XmlNode childNode; child->serialize(childNode); node.children.push_back(std::move(childNode)); }
}

Widget* Widget::find(WidgetId target) {
    if (m_id == target) return this;
    for (auto& child : m_children) if (Widget* found = child->find(target)) return found;
    return nullptr;
}
const Widget* Widget::find(WidgetId target) const {
    if (m_id == target) return this;
    for (const auto& child : m_children) if (const Widget* found = child->find(target)) return found;
    return nullptr;
}

Size Widget::measure(Size) const { return {}; }
void Widget::arrange(Rect bounds) { m_bounds = bounds; }
void Widget::paint(RenderList& render, const StyleSheet& style) const {
    if (!visible) return;
    for (const auto& child : m_children) child->paint(render, style);
}
bool Widget::on_event(UiEvent& event) {
    if (!enabled || !visible) return false;
    if (m_eventHandler) m_eventHandler(*this, event);
    return event.handled;
}

Label::Label(std::string value) : text(std::move(value)) { styleClass = "label"; }
Size Label::measure(Size available) const {
    const float preferredWidth = std::max(24.0f, static_cast<float>(text.size()) * 7.5f + 4.0f);
    return {wrap ? std::max(0.0f, available.width) : preferredWidth, 24.0f};
}
void Label::serialize(XmlNode& node) const {
    Widget::serialize(node);
    node.attributes["text"] = text;
    node.attributes["secondary"] = secondary ? "true" : "false";
    node.attributes["wrap"] = wrap ? "true" : "false";
}
void Label::paint(RenderList& render, const StyleSheet& style) const {
    if (!visible) return;
    const ControlStyle& appearance = style.control("label", styleVariant);
    const Color color = secondary ? appearance.textSecondary : appearance.text;
    render.text(m_bounds, text, color, style.font.family, style.font.size * style.uiScale);
}

Panel::Panel() { styleClass = "panel"; }
void Panel::serialize(XmlNode& node) const {
    Widget::serialize(node);
    node.attributes["layout"] = layout == LayoutMode::Horizontal ? "horizontal" : layout == LayoutMode::Vertical ? "vertical" : "overlay";
    node.attributes["clip"] = clipChildren ? "true" : "false";
}
Size Panel::measure(Size available) const {
    if (layout == LayoutMode::Overlay) return available;
    float main = 0.0f;
    float cross = 0.0f;
    const bool horizontal = layout == LayoutMode::Horizontal;
    for (const auto& child : m_children) {
        if (!child->visible) continue;
        const Size measured = child->measure(available);
        if (horizontal) {
            if (child->flex <= 0.0f) main += measured.width + child->margin.left + child->margin.right;
            cross = std::max(cross, measured.height + child->margin.top + child->margin.bottom);
        } else {
            if (child->flex <= 0.0f) main += measured.height + child->margin.top + child->margin.bottom;
            cross = std::max(cross, measured.width + child->margin.left + child->margin.right);
        }
    }
    std::size_t visibleCount = 0;
    for (const auto& child : m_children) if (child->visible) ++visibleCount;
    const float totalGap = std::max(0.0f, static_cast<float>(visibleCount > 0 ? visibleCount - 1 : 0) * gap);
    return horizontal ? Size{main + totalGap + padding.left + padding.right, cross + padding.top + padding.bottom} : Size{cross + padding.left + padding.right, main + totalGap + padding.top + padding.bottom};
}

void Panel::arrange(Rect bounds) {
    m_bounds = bounds;
    const Rect content{bounds.x + padding.left, bounds.y + padding.top, std::max(0.0f, bounds.width - padding.left - padding.right), std::max(0.0f, bounds.height - padding.top - padding.bottom)};
    if (layout == LayoutMode::Overlay) {
        for (auto& child : m_children) {
            if (!child->visible) continue;
            Rect childBounds = content;
            childBounds.x += child->margin.left; childBounds.y += child->margin.top;
            childBounds.width = std::max(0.0f, childBounds.width - child->margin.left - child->margin.right);
            childBounds.height = std::max(0.0f, childBounds.height - child->margin.top - child->margin.bottom);
            child->arrange(childBounds);
        }
        return;
    }
    const bool horizontal = layout == LayoutMode::Horizontal;
    float fixed = 0.0f, flexTotal = 0.0f;
    std::size_t visibleCount = 0;
    for (const auto& child : m_children) {
        if (!child->visible) continue;
        ++visibleCount; flexTotal += std::max(0.0f, child->flex);
        const Size measured = child->measure({content.width, content.height});
        if (child->flex <= 0.0f) {
            fixed += (horizontal ? measured.width + child->margin.left + child->margin.right : measured.height + child->margin.top + child->margin.bottom);
        }
    }
    fixed += std::max(0.0f, static_cast<float>(visibleCount > 0 ? visibleCount - 1 : 0) * gap);
    const float availableMain = std::max(0.0f, (horizontal ? content.width : content.height) - fixed);
    float cursor = horizontal ? content.x : content.y;
    for (auto& child : m_children) {
        if (!child->visible) continue;
        const Size measured = child->measure({content.width, content.height});
        const float main = child->flex > 0.0f && flexTotal > 0.0f ? availableMain * child->flex / flexTotal : (horizontal ? measured.width : measured.height);
        const float crossAvailable = horizontal ? content.height : content.width;
        const float crossSize = horizontal ? measured.height : measured.width;
        float cross = crossSize;
        if ((horizontal ? child->verticalAlign : child->horizontalAlign) == Align::Stretch) cross = std::max(0.0f, crossAvailable - (horizontal ? child->margin.top + child->margin.bottom : child->margin.left + child->margin.right));
        float crossPos = horizontal ? content.y : content.x;
        const Align alignment = horizontal ? child->verticalAlign : child->horizontalAlign;
        if (alignment == Align::Center) crossPos += (crossAvailable - cross) * 0.5f;
        else if (alignment == Align::End) crossPos += crossAvailable - cross;
        Rect childBounds = horizontal ? Rect{cursor + child->margin.left, crossPos + child->margin.top, std::max(0.0f, main), std::max(0.0f, cross)} : Rect{crossPos + child->margin.left, cursor + child->margin.top, std::max(0.0f, cross), std::max(0.0f, main)};
        child->arrange(childBounds);
        cursor += main + gap + (horizontal ? child->margin.left + child->margin.right : child->margin.top + child->margin.bottom);
    }
}

void Panel::paint(RenderList& render, const StyleSheet& style) const {
    if (!visible) return;
    const ControlStyle& appearance = style.control(styleClass, styleVariant);
    render.rect(m_bounds, hasBackgroundOverride ? backgroundOverride : appearance.background, {appearance.radii.topLeft, appearance.radii.topRight, appearance.radii.bottomRight, appearance.radii.bottomLeft});
    if (appearance.borderWidth > 0.0f) render.border(m_bounds, appearance.border, appearance.borderWidth);
    if (clipChildren) render.begin_clip(m_bounds);
    for (const auto& child : m_children) child->paint(render, style);
    if (clipChildren) render.end_clip();
}

Button::Button(std::string value) : label(std::move(value)) { styleClass = "button"; focusable = true; }
void Button::serialize(XmlNode& node) const { Widget::serialize(node); node.attributes["label"] = label; }
Size Button::measure(Size) const { return {std::max(88.0f, 24.0f + static_cast<float>(label.size()) * 8.0f), 32.0f}; }
bool Button::on_event(UiEvent& event) {
    if (!enabled) return false;
    if (event.type == UiEventType::PointerMove) { hovered = m_bounds.contains(event.position); return hovered; }
    if (event.type == UiEventType::PointerDown && event.button == PointerButton::Left && m_bounds.contains(event.position)) { pressed = true; event.handled = true; return true; }
    if (event.type == UiEventType::PointerUp && event.button == PointerButton::Left) { const bool activate = pressed && m_bounds.contains(event.position); pressed = false; if (activate && clicked) clicked(); event.handled = activate; return activate; }
    return Widget::on_event(event);
}
void Button::paint(RenderList& render, const StyleSheet& style) const {
    const ControlStyle& appearance = style.control(styleClass, styleVariant);
    const std::string state = pressed ? "pressed" : (focused ? "focus" : (hovered ? "hover" : "normal"));
    const Color fallback = pressed ? appearance.pressed : (hovered ? appearance.hover : appearance.background);
    const Color fill = style_color(style, styleClass, state, "background", fallback);
    const Insets radii = style_radii(style, styleClass, state, appearance);
    const float borderWidth = style_number(style, styleClass, state, "border-width", appearance.borderWidth);
    render.rect(m_bounds, fill, radii);
    if (borderWidth > 0.0f) render.border(m_bounds, style_color(style, styleClass, state, "border", appearance.border), borderWidth, radii);
    render.text(m_bounds, label, style_color(style, styleClass, state, "text", appearance.text), style.font.family, style_number(style, styleClass, state, "font-size", style.font.size * style.uiScale));
}

ToggleButton::ToggleButton(std::string value) : Button(std::move(value)) {
    styleClass = "toggle";
}

bool ToggleButton::on_event(UiEvent& event) {
    if (event.type == UiEventType::PointerUp && event.button == PointerButton::Left && pressed && m_bounds.contains(event.position)) {
        checked = !checked;
        if (clicked) clicked();
        event.type = UiEventType::ValueChanged;
        event.handled = true;
        pressed = false;
        return true;
    }
    return Button::on_event(event);
}

void ToggleButton::serialize(XmlNode& node) const {
    Button::serialize(node);
    node.attributes["checked"] = checked ? "true" : "false";
}

void ToggleButton::paint(RenderList& render, const StyleSheet& style) const {
    const ControlStyle& appearance = style.control(styleClass, styleVariant);
    const std::string state = checked ? "checked" : (pressed ? "pressed" : (hovered ? "hover" : "normal"));
    const Color fallback = checked || pressed ? appearance.pressed : (hovered ? appearance.hover : appearance.background);
    const Color fill = style_color(style, styleClass, state, "background", fallback);
    const Insets radii = style_radii(style, styleClass, state, appearance);
    render.rect(m_bounds, fill, radii);
    if (appearance.borderWidth > 0.0f) render.border(m_bounds, style_color(style, styleClass, state, "border", appearance.border), appearance.borderWidth, radii);
    render.text(m_bounds, label, style_color(style, styleClass, state, "text", appearance.text), style.font.family, style_number(style, styleClass, state, "font-size", style.font.size * style.uiScale));
}

CheckBox::CheckBox(std::string value) : ToggleButton(std::move(value)) { styleClass = "checkbox"; }

void CheckBox::serialize(XmlNode& node) const {
    ToggleButton::serialize(node);
    node.attributes["checked"] = checked ? "true" : "false";
}

void CheckBox::paint(RenderList& render, const StyleSheet& style) const {
    const ControlStyle& appearance = style.control(styleClass, styleVariant);
    const float side = std::min(18.0f, m_bounds.height - 8.0f);
    const Rect box{m_bounds.x + 4.0f, m_bounds.y + (m_bounds.height - side) * 0.5f, side, side};
    render.rect(box, checked ? style.color("accent") : appearance.background, {4, 4, 4, 4});
    render.border(box, checked ? style.color("accent") : appearance.border, std::max(1.0f, appearance.borderWidth));
    render.text({box.x + side + 8.0f, m_bounds.y, std::max(0.0f, m_bounds.width - side - 12.0f), m_bounds.height}, label, appearance.text, style.font.family, style.font.size * style.uiScale);
}

bool ComboBox::on_event(UiEvent& event) {
    if (!enabled || !visible) return false;
    if (event.type == UiEventType::PointerDown && event.button == PointerButton::Left && m_bounds.contains(event.position)) {
        open = !open;
        event.handled = true;
        return true;
    }
    if (open && event.type == UiEventType::PointerUp && event.button == PointerButton::Left && m_bounds.contains(event.position)) {
        const float itemHeight = std::max(1.0f, m_bounds.height);
        const std::size_t index = std::min(items.empty() ? 0u : items.size() - 1, static_cast<std::size_t>(std::max(0.0f, event.position.y - m_bounds.y) / itemHeight));
        if (!items.empty()) { selectedIndex = index; if (selectionChanged) selectionChanged(selectedIndex); }
        open = false;
        event.handled = true;
        return true;
    }
    if (event.type == UiEventType::KeyDown && (event.key == 38 || event.key == 40) && !items.empty()) {
        if (event.key == 38 && selectedIndex > 0) --selectedIndex;
        if (event.key == 40 && selectedIndex + 1 < items.size()) ++selectedIndex;
        if (selectionChanged) selectionChanged(selectedIndex);
        event.handled = true;
        return true;
    }
    return Widget::on_event(event);
}

Size ComboBox::measure(Size) const { return {180.0f, 32.0f}; }

void ComboBox::serialize(XmlNode& node) const {
    Widget::serialize(node);
    node.attributes["selected"] = std::to_string(selectedIndex);
    node.attributes["items"] = [&] { std::ostringstream result; for (std::size_t i = 0; i < items.size(); ++i) { if (i) result << '|'; result << items[i]; } return result.str(); }();
}

void ComboBox::paint(RenderList& render, const StyleSheet& style) const {
    const ControlStyle& appearance = style.control(styleClass == "panel" ? "input" : styleClass, styleVariant);
    render.rect(m_bounds, appearance.background, {appearance.radii.topLeft, appearance.radii.topRight, appearance.radii.bottomRight, appearance.radii.bottomLeft});
    render.border(m_bounds, focused ? appearance.focus : appearance.border, std::max(1.0f, appearance.borderWidth), {appearance.radii.topLeft, appearance.radii.topRight, appearance.radii.bottomRight, appearance.radii.bottomLeft});
    const std::string value = !items.empty() && selectedIndex < items.size() ? items[selectedIndex] : std::string{};
    render.text(m_bounds, value + (open ? "  ▲" : "  ▼"), appearance.text, style.font.family, style.font.size * style.uiScale);
}

void ProgressBar::serialize(XmlNode& node) const {
    Widget::serialize(node);
    node.attributes["min"] = std::to_string(min);
    node.attributes["max"] = std::to_string(max);
    node.attributes["value"] = std::to_string(value);
}

void ProgressBar::paint(RenderList& render, const StyleSheet& style) const {
    const ControlStyle& appearance = style.control("input", styleVariant);
    render.rect(m_bounds, appearance.border, {appearance.radii.topLeft, appearance.radii.topRight, appearance.radii.bottomRight, appearance.radii.bottomLeft});
    const float ratio = clamp01((value - min) / std::max(0.0001f, max - min));
    render.rect({m_bounds.x, m_bounds.y, m_bounds.width * ratio, m_bounds.height}, style.color("accent"), {appearance.radii.topLeft, appearance.radii.topRight, appearance.radii.bottomRight, appearance.radii.bottomLeft});
}

void Separator::serialize(XmlNode& node) const { Widget::serialize(node); node.attributes["vertical"] = vertical ? "true" : "false"; }
Size Separator::measure(Size) const { return vertical ? Size{1.0f, 24.0f} : Size{24.0f, 1.0f}; }
void Separator::paint(RenderList& render, const StyleSheet& style) const {
    const Color color = style.color("border");
    if (vertical) render.line({m_bounds.x + m_bounds.width * 0.5f, m_bounds.y}, {m_bounds.x + m_bounds.width * 0.5f, m_bounds.y + m_bounds.height}, color);
    else render.line({m_bounds.x, m_bounds.y + m_bounds.height * 0.5f}, {m_bounds.x + m_bounds.width, m_bounds.y + m_bounds.height * 0.5f}, color);
}

bool ScrollView::on_event(UiEvent& event) {
    if (event.type == UiEventType::Wheel && m_bounds.contains(event.position)) {
        offset.y = std::max(0.0f, std::min(std::max(0.0f, contentSize.y - m_bounds.height), offset.y - event.wheelDelta));
        event.handled = true;
        return true;
    }
    return Panel::on_event(event);
}

void ScrollView::serialize(XmlNode& node) const {
    Panel::serialize(node);
    node.attributes["offset-x"] = std::to_string(offset.x);
    node.attributes["offset-y"] = std::to_string(offset.y);
}

void ScrollView::arrange(Rect bounds) {
    Panel::arrange(bounds);
    contentSize = {bounds.width, std::max(bounds.height, measure({bounds.width, bounds.height}).height)};
}

bool ListView::on_event(UiEvent& event) {
    if (event.type == UiEventType::PointerUp && event.button == PointerButton::Left && m_bounds.contains(event.position)) {
        const std::size_t index = static_cast<std::size_t>(std::max(0.0f, event.position.y - m_bounds.y + offset.y) / std::max(1.0f, itemExtent));
        if (index != selectedIndex) { selectedIndex = index; if (itemSelected) itemSelected(index); }
        event.handled = true;
        return true;
    }
    return ScrollView::on_event(event);
}

void ListView::serialize(XmlNode& node) const { ScrollView::serialize(node); node.attributes["item-extent"] = std::to_string(itemExtent); node.attributes["selected-index"] = std::to_string(selectedIndex); }
void ListView::paint(RenderList& render, const StyleSheet& style) const {
    Panel::paint(render, style);
    const Color accent = style.color("accent");
    if (selectedIndex < 100000) render.rect({m_bounds.x, m_bounds.y + selectedIndex * itemExtent - offset.y, m_bounds.width, itemExtent}, accent, {4, 4, 4, 4});
}

bool TabView::on_event(UiEvent& event) {
    if (event.type == UiEventType::PointerUp && event.button == PointerButton::Left && m_bounds.contains(event.position) && !tabs.empty()) {
        const float tabWidth = m_bounds.width / static_cast<float>(tabs.size());
        activeTab = std::min(tabs.size() - 1, static_cast<std::size_t>((event.position.x - m_bounds.x) / std::max(1.0f, tabWidth)));
        if (tabChanged) tabChanged(activeTab);
        event.handled = true;
        return true;
    }
    return Widget::on_event(event);
}

Size TabView::measure(Size available) const { return {available.width, 36.0f}; }
void TabView::serialize(XmlNode& node) const {
    Widget::serialize(node);
    node.attributes["active"] = std::to_string(activeTab);
    for (const auto& tab : tabs) { XmlNode item; item.name = "tab"; item.attributes["label"] = tab; node.children.push_back(std::move(item)); }
}
void TabView::paint(RenderList& render, const StyleSheet& style) const {
    const ControlStyle& appearance = style.control("tab", styleVariant);
    if (tabs.empty()) return;
    const float tabWidth = m_bounds.width / static_cast<float>(tabs.size());
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const Rect tabBounds{m_bounds.x + tabWidth * i, m_bounds.y, tabWidth, m_bounds.height};
        render.rect(tabBounds, i == activeTab ? appearance.pressed : appearance.background, {appearance.radii.topLeft, appearance.radii.topRight, appearance.radii.bottomRight, appearance.radii.bottomLeft});
        render.text(tabBounds, tabs[i], appearance.text, style.font.family, style.font.size * style.uiScale);
    }
}

bool Slider::on_event(UiEvent& event) {
    if (!enabled) return false;
    if ((event.type == UiEventType::PointerDown || event.type == UiEventType::PointerMove) && (pressed || event.type == UiEventType::PointerDown) && m_bounds.contains(event.position)) {
        pressed = true;
        const float ratio = clamp01((event.position.x - m_bounds.x) / std::max(1.0f, m_bounds.width));
        const float raw = min + (max - min) * ratio;
        value = step > 0.0f ? std::round(raw / step) * step : raw;
        value = std::max(min, std::min(max, value));
        if (changed) changed(value);
        event.handled = true;
        return true;
    }
    if (event.type == UiEventType::PointerUp && pressed) { pressed = false; event.handled = true; return true; }
    return Widget::on_event(event);
}
void Slider::serialize(XmlNode& node) const { Widget::serialize(node); node.attributes["min"] = std::to_string(min); node.attributes["max"] = std::to_string(max); node.attributes["step"] = std::to_string(step); node.attributes["value"] = std::to_string(value); }
Size Slider::measure(Size) const { return {160.0f, 32.0f}; }
void Slider::paint(RenderList& render, const StyleSheet& style) const {
    const ControlStyle& appearance = style.control("input", styleVariant);
    const float center = m_bounds.y + m_bounds.height * 0.5f;
    render.line({m_bounds.x, center}, {m_bounds.x + m_bounds.width, center}, appearance.border, 2.0f);
    const float ratio = clamp01((value - min) / std::max(0.0001f, max - min));
    render.line({m_bounds.x, center}, {m_bounds.x + m_bounds.width * ratio, center}, style.color("accent"), 3.0f);
    render.rect({m_bounds.x + m_bounds.width * ratio - 6, center - 6, 12, 12}, style.color("accent"), {6, 6, 6, 6});
}

TextBox::TextBox(std::string value) : text(std::move(value)), cursor(text.size()) { styleClass = "input"; focusable = true; }
void TextBox::serialize(XmlNode& node) const { Widget::serialize(node); node.attributes["text"] = text; node.attributes["multiline"] = multiline ? "true" : "false"; }
Size TextBox::measure(Size) const { return {180.0f, multiline ? 96.0f : 32.0f}; }
bool TextBox::on_event(UiEvent& event) {
    if (!enabled) return false;
    if (event.type == UiEventType::PointerDown && m_bounds.contains(event.position)) { focused = true; event.handled = true; return true; }
    if (!focused) return Widget::on_event(event);
    if (event.type == UiEventType::TextInput) { text.insert(cursor, event.text); cursor += event.text.size(); if (changed) changed(text); event.handled = true; return true; }
    if (event.type == UiEventType::KeyDown) {
        if (event.key == 8 && cursor > 0) { text.erase(cursor - 1, 1); --cursor; if (changed) changed(text); event.handled = true; return true; }
        if (event.key == 37 && cursor > 0) { --cursor; event.handled = true; return true; }
        if (event.key == 39 && cursor < text.size()) { ++cursor; event.handled = true; return true; }
    }
    return Widget::on_event(event);
}
void TextBox::paint(RenderList& render, const StyleSheet& style) const {
    const ControlStyle& appearance = style.control(styleClass, styleVariant);
    const std::string state = focused ? "focus" : "normal";
    const Insets radii = style_radii(style, styleClass, state, appearance);
    render.rect(m_bounds, style_color(style, styleClass, state, "background", appearance.background), radii);
    if (appearance.borderWidth > 0.0f) render.border(m_bounds, style_color(style, styleClass, state, "border", focused ? appearance.focus : appearance.border), appearance.borderWidth, radii);
    render.text(m_bounds, text, style_color(style, styleClass, state, "text", appearance.text), style.font.family, style_number(style, styleClass, state, "font-size", style.font.size * style.uiScale));
}

void RichTextBox::paint(RenderList& render, const StyleSheet& style) const {
    Rect cursor = m_bounds;
    for (const RichTextRun& run : runs) { render.text(cursor, run.text, run.color.a == 0.0f ? style.color("text") : run.color, style.font.family, style.font.size * style.uiScale); cursor.x += static_cast<float>(run.text.size()) * style.font.size * 0.55f; }
}
void RichTextBox::serialize(XmlNode& node) const {
    Widget::serialize(node);
    for (const RichTextRun& run : runs) { XmlNode runNode; runNode.name = "run"; runNode.text = run.text; runNode.attributes["color"] = "#" + [&run] { std::ostringstream s; s << std::hex << std::setw(8) << std::setfill('0') << run.color.to_rgba8(); return s.str(); }(); runNode.attributes["bold"] = run.bold ? "true" : "false"; runNode.attributes["italic"] = run.italic ? "true" : "false"; node.children.push_back(std::move(runNode)); }
}
Image::Image(std::string value) : uri(std::move(value)) { styleClass = "panel"; }
void Image::serialize(XmlNode& node) const { Widget::serialize(node); node.attributes["uri"] = uri; }
Size Image::measure(Size) const { return {256.0f, 256.0f}; }
void Image::paint(RenderList& render, const StyleSheet&) const { if (visible) render.image(m_bounds, uri, tint); }
Video::Video(std::string value) : uri(std::move(value)) { styleClass = "panel"; }
void Video::serialize(XmlNode& node) const { Widget::serialize(node); node.attributes["uri"] = uri; node.attributes["controls"] = controlsVisible ? "true" : "false"; }
void Video::paint(RenderList& render, const StyleSheet&) const { if (visible) render.image(m_bounds, uri); }
Audio::Audio(std::string value) : uri(std::move(value)) { styleClass = "panel"; }
void Audio::serialize(XmlNode& node) const { Widget::serialize(node); node.attributes["uri"] = uri; node.attributes["volume"] = std::to_string(volume); node.attributes["duration"] = std::to_string(durationSeconds); }
void Audio::paint(RenderList& render, const StyleSheet& style) const {
    if (!visible) return;
    const ControlStyle& appearance = style.control("panel", styleVariant);
    render.rect(m_bounds, appearance.background, {4, 4, 4, 4});
    const float progress = durationSeconds > 0.0f ? clamp01(positionSeconds / durationSeconds) : 0.0f;
    render.line({m_bounds.x + 12, m_bounds.y + m_bounds.height - 10}, {m_bounds.x + m_bounds.width - 12, m_bounds.y + m_bounds.height - 10}, appearance.border, 2.0f);
    render.line({m_bounds.x + 12, m_bounds.y + m_bounds.height - 10}, {m_bounds.x + 12 + (m_bounds.width - 24) * progress, m_bounds.y + m_bounds.height - 10}, style.color("accent"), 3.0f);
    render.text(m_bounds, state == MediaPlaybackState::Playing ? "播放中" : "已暂停", appearance.text, style.font.family, style.font.size);
}

} // namespace shinkou::uikit

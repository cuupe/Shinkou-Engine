#pragma once

#include "Animation.h"
#include "Render.h"
#include "Style.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace shinkou::uikit {

class Widget {
public:
    using EventHandler = std::function<void(Widget&, UiEvent&)>;
    using PropertyValue = std::variant<bool, float, std::string, Color>;
    Widget();
    virtual ~Widget() = default;
    Widget(const Widget&) = delete;

    WidgetId id() const { return m_id; }
    Widget* parent() const { return m_parent; }
    const std::vector<std::unique_ptr<Widget>>& children() const { return m_children; }
    void add(std::unique_ptr<Widget> child);
    std::unique_ptr<Widget> remove(WidgetId id);
    void clear();
    template<typename T, typename... Args> T& emplace(Args&&... args) {
        auto child = std::make_unique<T>(std::forward<Args>(args)...);
        T& result = *child; add(std::move(child)); return result;
    }
    Widget* find(WidgetId id);
    const Widget* find(WidgetId id) const;

    virtual const char* type_name() const { return "widget"; }
    virtual void serialize(XmlNode& node) const;
    virtual Size measure(Size available) const;
    virtual void arrange(Rect bounds);
    virtual void paint(RenderList& render, const StyleSheet& style) const;
    virtual bool on_event(UiEvent& event);

    Rect bounds() const { return m_bounds; }
    void set_bounds(Rect value) { m_bounds = value; }
    void set_event_handler(EventHandler handler) { m_eventHandler = std::move(handler); }
    void set_property(std::string key, PropertyValue value) { m_properties[std::move(key)] = std::move(value); }
    const PropertyValue* property(const std::string& key) const {
        const auto found = m_properties.find(key);
        return found == m_properties.end() ? nullptr : &found->second;
    }
    const std::map<std::string, PropertyValue>& properties() const { return m_properties; }
    bool visible = true;
    bool enabled = true;
    bool focusable = false;
    bool hovered = false;
    bool pressed = false;
    bool focused = false;
    bool selected = false;
    bool checked = false;
    bool hitTestVisible = true;
    int tabIndex = -1;
    std::string name;
    std::string styleVariant = "default";
    std::string styleClass = "panel";
    LayoutMode layout = LayoutMode::Overlay;
    Align horizontalAlign = Align::Stretch;
    Align verticalAlign = Align::Stretch;
    float flex = 0.0f;
    float gap = 0.0f;
    Insets padding{};
    Insets margin{};

protected:
    std::vector<std::unique_ptr<Widget>> m_children;
    Widget* m_parent = nullptr;
    Rect m_bounds{};
    EventHandler m_eventHandler;
    std::map<std::string, PropertyValue> m_properties;

private:
    WidgetId m_id = 0;
    static WidgetId s_nextId;
};

class Label : public Widget {
public:
    explicit Label(std::string value = {});
    std::string text;
    bool secondary = false;
    bool wrap = false;
    const char* type_name() const override { return "label"; }
    Size measure(Size available) const override;
    void serialize(XmlNode& node) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

class Panel : public Widget {
public:
    Panel();
    Color backgroundOverride{};
    bool hasBackgroundOverride = false;
    bool clipChildren = false;
    Size measure(Size available) const override;
    const char* type_name() const override { return "panel"; }
    void serialize(XmlNode& node) const override;
    void arrange(Rect bounds) override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

class Button : public Widget {
public:
    explicit Button(std::string label = {});
    std::string label;
    std::function<void()> clicked;
    bool on_event(UiEvent& event) override;
    Size measure(Size available) const override;
    const char* type_name() const override { return "button"; }
    void serialize(XmlNode& node) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

class ToggleButton : public Button {
public:
    explicit ToggleButton(std::string label = {});
    bool on_event(UiEvent& event) override;
    const char* type_name() const override { return "toggle-button"; }
    void serialize(XmlNode& node) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

class CheckBox : public ToggleButton {
public:
    explicit CheckBox(std::string label = {});
    const char* type_name() const override { return "checkbox"; }
    void serialize(XmlNode& node) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

class ComboBox : public Widget {
public:
    std::vector<std::string> items;
    std::size_t selectedIndex = 0;
    bool open = false;
    std::function<void(std::size_t)> selectionChanged;
    bool on_event(UiEvent& event) override;
    Size measure(Size available) const override;
    const char* type_name() const override { return "combobox"; }
    void serialize(XmlNode& node) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

class ProgressBar : public Widget {
public:
    float value = 0.0f;
    float min = 0.0f;
    float max = 1.0f;
    const char* type_name() const override { return "progress"; }
    void serialize(XmlNode& node) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

class Separator : public Widget {
public:
    bool vertical = false;
    const char* type_name() const override { return "separator"; }
    void serialize(XmlNode& node) const override;
    Size measure(Size available) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

class ScrollView : public Panel {
public:
    Vec2 offset{};
    Vec2 contentSize{};
    bool on_event(UiEvent& event) override;
    const char* type_name() const override { return "scroll-view"; }
    void serialize(XmlNode& node) const override;
    void arrange(Rect bounds) override;
};

class ListView : public ScrollView {
public:
    float itemExtent = 32.0f;
    std::size_t selectedIndex = 0;
    std::function<void(std::size_t)> itemSelected;
    bool on_event(UiEvent& event) override;
    const char* type_name() const override { return "list-view"; }
    void serialize(XmlNode& node) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

class TabView : public Widget {
public:
    std::vector<std::string> tabs;
    std::size_t activeTab = 0;
    std::function<void(std::size_t)> tabChanged;
    bool on_event(UiEvent& event) override;
    Size measure(Size available) const override;
    const char* type_name() const override { return "tab-view"; }
    void serialize(XmlNode& node) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

class Slider : public Widget {
public:
    float min = 0.0f;
    float max = 1.0f;
    float step = 0.01f;
    float value = 0.0f;
    std::function<void(float)> changed;
    bool on_event(UiEvent& event) override;
    Size measure(Size available) const override;
    const char* type_name() const override { return "slider"; }
    void serialize(XmlNode& node) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

class TextBox : public Widget {
public:
    explicit TextBox(std::string value = {});
    std::string text;
    std::size_t cursor = 0;
    bool multiline = false;
    std::function<void(const std::string&)> changed;
    bool on_event(UiEvent& event) override;
    Size measure(Size available) const override;
    const char* type_name() const override { return "textbox"; }
    void serialize(XmlNode& node) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

struct RichTextRun { std::string text; Color color{}; bool bold = false; bool italic = false; };
class RichTextBox : public Widget {
public:
    std::vector<RichTextRun> runs;
    const char* type_name() const override { return "rich-textbox"; }
    void serialize(XmlNode& node) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

class Image : public Widget {
public:
    explicit Image(std::string uri = {});
    std::string uri;
    Color tint{1, 1, 1, 1};
    Size measure(Size available) const override;
    const char* type_name() const override { return "image"; }
    void serialize(XmlNode& node) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

enum class MediaPlaybackState { Stopped, Playing, Paused };
class Video : public Widget {
public:
    explicit Video(std::string uri = {});
    std::string uri;
    MediaPlaybackState state = MediaPlaybackState::Stopped;
    float positionSeconds = 0.0f;
    bool controlsVisible = true;
    const char* type_name() const override { return "video"; }
    void serialize(XmlNode& node) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

class Audio : public Widget {
public:
    explicit Audio(std::string uri = {});
    std::string uri;
    MediaPlaybackState state = MediaPlaybackState::Stopped;
    float positionSeconds = 0.0f;
    float durationSeconds = 0.0f;
    float volume = 1.0f;
    const char* type_name() const override { return "audio"; }
    void serialize(XmlNode& node) const override;
    void paint(RenderList& render, const StyleSheet& style) const override;
};

} // namespace shinkou::uikit

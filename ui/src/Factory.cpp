#include "shinkou/uikit/Factory.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace shinkou::uikit {
namespace {

float number(const std::string& value, float fallback = 0.0f) {
    if (value.empty()) return fallback;
    char* end = nullptr;
    const float parsed = std::strtof(value.c_str(), &end);
    return end == value.c_str() ? fallback : parsed;
}
bool boolean(const std::string& value, bool fallback = false) {
    return value.empty() ? fallback : value == "true" || value == "1" || value == "yes";
}
LayoutMode layout_mode(const std::string& value) {
    if (value == "horizontal") return LayoutMode::Horizontal;
    if (value == "vertical") return LayoutMode::Vertical;
    return LayoutMode::Overlay;
}
Insets insets(const std::string& value) {
    if (value.empty()) return {};
    std::stringstream stream(value);
    std::string part;
    float values[4]{};
    int count = 0;
    while (std::getline(stream, part, ',') && count < 4) values[count++] = number(part);
    if (count == 1) return Insets(values[0]);
    if (count == 2) return Insets(values[0], values[1]);
    if (count == 4) return Insets(values[0], values[1], values[2], values[3]);
    return {};
}
void apply_common(Widget& widget, const XmlNode& node) {
    widget.styleClass = node.attribute("style", widget.styleClass);
    widget.styleVariant = node.attribute("variant", widget.styleVariant);
    widget.name = node.attribute("name", widget.name);
    widget.visible = boolean(node.attribute("visible"), widget.visible);
    widget.enabled = boolean(node.attribute("enabled"), widget.enabled);
    widget.focusable = boolean(node.attribute("focusable"), widget.focusable);
    widget.hitTestVisible = boolean(node.attribute("hit-test"), widget.hitTestVisible);
    widget.tabIndex = static_cast<int>(number(node.attribute("tab-index"), static_cast<float>(widget.tabIndex)));
    widget.selected = boolean(node.attribute("selected"), widget.selected);
    widget.checked = boolean(node.attribute("checked"), widget.checked);
    widget.flex = number(node.attribute("flex"), widget.flex);
    widget.gap = number(node.attribute("gap"), widget.gap);
    widget.padding = insets(node.attribute("padding"));
    widget.margin = insets(node.attribute("margin"));
    widget.layout = layout_mode(node.attribute("layout"));
}

} // namespace

WidgetRegistry::WidgetRegistry() {
    register_type("widget", [](const XmlNode&) { return std::make_unique<Widget>(); });
    register_type("label", [](const XmlNode& node) { return std::make_unique<Label>(node.attribute("text")); });
    register_type("panel", [](const XmlNode&) { return std::make_unique<Panel>(); });
    register_type("button", [](const XmlNode& node) { return std::make_unique<Button>(node.attribute("label")); });
    register_type("toggle-button", [](const XmlNode& node) { return std::make_unique<ToggleButton>(node.attribute("label")); });
    register_type("checkbox", [](const XmlNode& node) { return std::make_unique<CheckBox>(node.attribute("label")); });
    register_type("combobox", [](const XmlNode&) { return std::make_unique<ComboBox>(); });
    register_type("progress", [](const XmlNode&) { return std::make_unique<ProgressBar>(); });
    register_type("separator", [](const XmlNode&) { return std::make_unique<Separator>(); });
    register_type("scroll-view", [](const XmlNode&) { return std::make_unique<ScrollView>(); });
    register_type("list-view", [](const XmlNode&) { return std::make_unique<ListView>(); });
    register_type("tab-view", [](const XmlNode&) { return std::make_unique<TabView>(); });
    register_type("slider", [](const XmlNode&) { return std::make_unique<Slider>(); });
    register_type("textbox", [](const XmlNode& node) { return std::make_unique<TextBox>(node.attribute("text")); });
    register_type("rich-textbox", [](const XmlNode&) { return std::make_unique<RichTextBox>(); });
    register_type("image", [](const XmlNode& node) { return std::make_unique<Image>(node.attribute("uri")); });
    register_type("video", [](const XmlNode& node) { return std::make_unique<Video>(node.attribute("uri")); });
    register_type("audio", [](const XmlNode& node) { return std::make_unique<Audio>(node.attribute("uri")); });
}

void WidgetRegistry::register_type(std::string type, Creator creator) {
    if (!type.empty() && creator) m_creators[std::move(type)] = std::move(creator);
}
bool WidgetRegistry::unregister_type(const std::string& type) { return m_creators.erase(type) != 0; }
bool WidgetRegistry::has_type(const std::string& type) const { return m_creators.find(type) != m_creators.end(); }

std::unique_ptr<Widget> WidgetRegistry::create(const XmlNode& node, std::string* error) const {
    const auto found = m_creators.find(node.name);
    if (found == m_creators.end()) {
        if (error) *error = "unknown widget type: " + node.name;
        return nullptr;
    }
    std::unique_ptr<Widget> widget = found->second(node);
    if (!widget) { if (error) *error = "widget creator returned null: " + node.name; return nullptr; }
    apply_common(*widget, node);
    if (auto* panel = dynamic_cast<Panel*>(widget.get())) panel->clipChildren = boolean(node.attribute("clip"), panel->clipChildren);
    if (auto* combo = dynamic_cast<ComboBox*>(widget.get())) {
        combo->selectedIndex = static_cast<std::size_t>(std::max(0.0f, number(node.attribute("selected"), 0.0f)));
        std::stringstream items(node.attribute("items"));
        std::string item;
        while (std::getline(items, item, '|')) combo->items.push_back(item);
        if (combo->selectedIndex >= combo->items.size() && !combo->items.empty()) combo->selectedIndex = combo->items.size() - 1;
    }
    if (auto* slider = dynamic_cast<Slider*>(widget.get())) { slider->min = number(node.attribute("min"), slider->min); slider->max = number(node.attribute("max"), slider->max); slider->step = number(node.attribute("step"), slider->step); slider->value = std::max(slider->min, std::min(slider->max, number(node.attribute("value"), slider->value))); }
    if (auto* textbox = dynamic_cast<TextBox*>(widget.get())) textbox->multiline = boolean(node.attribute("multiline"), textbox->multiline);
    if (auto* video = dynamic_cast<Video*>(widget.get())) video->controlsVisible = boolean(node.attribute("controls"), video->controlsVisible);
    if (auto* audio = dynamic_cast<Audio*>(widget.get())) { audio->volume = number(node.attribute("volume"), audio->volume); audio->durationSeconds = number(node.attribute("duration"), audio->durationSeconds); }
    if (auto* progress = dynamic_cast<ProgressBar*>(widget.get())) { progress->min = number(node.attribute("min"), progress->min); progress->max = number(node.attribute("max"), progress->max); progress->value = number(node.attribute("value"), progress->value); }
    if (auto* separator = dynamic_cast<Separator*>(widget.get())) separator->vertical = boolean(node.attribute("vertical"), separator->vertical);
    if (auto* scroll = dynamic_cast<ScrollView*>(widget.get())) { scroll->offset.x = number(node.attribute("offset-x"), scroll->offset.x); scroll->offset.y = number(node.attribute("offset-y"), scroll->offset.y); }
    if (auto* list = dynamic_cast<ListView*>(widget.get())) { list->itemExtent = number(node.attribute("item-extent"), list->itemExtent); list->selectedIndex = static_cast<std::size_t>(std::max(0.0f, number(node.attribute("selected-index"), 0.0f))); }
    if (auto* tab = dynamic_cast<TabView*>(widget.get())) { tab->activeTab = static_cast<std::size_t>(std::max(0.0f, number(node.attribute("active"), 0.0f))); for (const XmlNode* item : node.children_named("tab")) tab->tabs.push_back(item->attribute("label")); }
    for (const XmlNode& childNode : node.children) {
        if (childNode.name == "run") {
            if (auto* richText = dynamic_cast<RichTextBox*>(widget.get())) richText->runs.push_back({childNode.text, Color::from_hex(childNode.attribute("color")), boolean(childNode.attribute("bold")), boolean(childNode.attribute("italic"))});
            continue;
        }
        std::unique_ptr<Widget> child = create(childNode, error);
        if (!child) return nullptr;
        widget->add(std::move(child));
    }
    return widget;
}

std::unique_ptr<Widget> WidgetRegistry::create_document(const std::string& source, std::string* error) const {
    XmlDocument document;
    if (!document.parse(source, error) || !document.root()) return nullptr;
    const XmlNode* root = document.root();
    if (root->name == "ui" && !root->children.empty()) root = &root->children.front();
    return create(*root, error);
}

XmlNode WidgetRegistry::serialize(const Widget& widget) const {
    XmlNode result;
    widget.serialize(result);
    return result;
}

} // namespace shinkou::uikit

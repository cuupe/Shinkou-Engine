#include "shinkou/uikit/Editor.h"
#include <algorithm>
#include <cmath>

namespace shinkou::uikit {
namespace {

bool set_string(Widget& widget, const std::string& name, const std::string& value) {
    if (name == "name") widget.name = value;
    else if (name == "style") widget.styleClass = value;
    else if (name == "variant") widget.styleVariant = value.empty() ? "default" : value;
    else if (name == "label") {
        if (auto* button = dynamic_cast<Button*>(&widget)) button->label = value;
        else return false;
    } else if (name == "text") {
        if (auto* textbox = dynamic_cast<TextBox*>(&widget)) { textbox->text = value; textbox->cursor = textbox->text.size(); }
        else return false;
    } else if (name == "uri") {
        if (auto* image = dynamic_cast<Image*>(&widget)) image->uri = value;
        else if (auto* video = dynamic_cast<Video*>(&widget)) video->uri = value;
        else if (auto* audio = dynamic_cast<Audio*>(&widget)) audio->uri = value;
        else return false;
    } else {
        return false;
    }
    widget.set_property(name, value);
    return true;
}

bool set_float(Widget& widget, const std::string& name, float value) {
    if (name == "flex") widget.flex = value;
    else if (name == "gap") widget.gap = std::max(0.0f, value);
    else if (name == "value") {
        if (auto* slider = dynamic_cast<Slider*>(&widget)) slider->value = std::max(slider->min, std::min(slider->max, value));
        else if (auto* progress = dynamic_cast<ProgressBar*>(&widget)) progress->value = std::max(progress->min, std::min(progress->max, value));
        else return false;
    } else if (name == "min") {
        if (auto* slider = dynamic_cast<Slider*>(&widget)) slider->min = value;
        else if (auto* progress = dynamic_cast<ProgressBar*>(&widget)) progress->min = value;
        else return false;
    } else if (name == "max") {
        if (auto* slider = dynamic_cast<Slider*>(&widget)) slider->max = value;
        else if (auto* progress = dynamic_cast<ProgressBar*>(&widget)) progress->max = value;
        else return false;
    } else if (name == "volume") {
        if (auto* audio = dynamic_cast<Audio*>(&widget)) audio->volume = clamp01(value);
        else return false;
    } else {
        return false;
    }
    widget.set_property(name, value);
    return true;
}

bool set_bool(Widget& widget, const std::string& name, bool value) {
    if (name == "visible") widget.visible = value;
    else if (name == "enabled") widget.enabled = value;
    else if (name == "checked") widget.checked = value;
    else if (name == "clip") {
        if (auto* panel = dynamic_cast<Panel*>(&widget)) panel->clipChildren = value;
        else return false;
    } else {
        return false;
    }
    widget.set_property(name, value);
    return true;
}

} // namespace

EditorDocument::EditorDocument() {
    m_ui.root().name = "Root";
    m_ui.root().styleClass = "panel";
}

Widget* EditorDocument::selected() { return m_selection ? m_ui.root().find(m_selection) : nullptr; }
const Widget* EditorDocument::selected() const { return m_selection ? m_ui.root().find(m_selection) : nullptr; }

bool EditorDocument::select(WidgetId id) {
    Widget* target = id ? m_ui.root().find(id) : nullptr;
    if (id && !target) return false;
    std::function<void(Widget&)> clearSelection = [&](Widget& widget) {
        widget.selected = false;
        for (const auto& child : widget.children()) clearSelection(*child);
    };
    clearSelection(m_ui.root());
    m_selection = target ? target->id() : 0;
    if (target) target->selected = true;
    return true;
}

WidgetId EditorDocument::add(std::string type, WidgetId parentId) {
    const std::string before = snapshot();
    XmlNode node; node.name = std::move(type);
    std::unique_ptr<Widget> widget = m_registry.create(node);
    if (!widget) return 0;
    Widget* parent = parentId ? m_ui.root().find(parentId) : &m_ui.root();
    if (!parent) return 0;
    const WidgetId newId = widget->id();
    widget->name = node.name + "-" + std::to_string(newId);
    parent->add(std::move(widget));
    select(newId);
    m_ui.invalidate_layout();
    record(EditorOperationKind::Add, before, snapshot());
    return newId;
}

bool EditorDocument::remove(WidgetId id) {
    if (!id || id == m_ui.root().id() || !m_ui.root().find(id)) return false;
    const std::string before = snapshot();
    std::unique_ptr<Widget> removed = m_ui.root().remove(id);
    if (!removed) return false;
    select(0);
    m_ui.invalidate_layout();
    record(EditorOperationKind::Remove, before, snapshot());
    return true;
}

WidgetId EditorDocument::duplicate(WidgetId id) {
    Widget* source = id ? m_ui.root().find(id) : nullptr;
    if (!source || source == &m_ui.root() || !source->parent()) return 0;
    const std::string before = snapshot();
    XmlNode node; source->serialize(node);
    std::unique_ptr<Widget> copy = m_registry.create(node);
    if (!copy) return 0;
    const WidgetId newId = copy->id();
    copy->name = source->name + "-copy";
    source->parent()->add(std::move(copy));
    select(newId);
    m_ui.invalidate_layout();
    record(EditorOperationKind::Duplicate, before, snapshot());
    return newId;
}

bool EditorDocument::set_property(WidgetId id, std::string name, Widget::PropertyValue value) {
    Widget* widget = id ? m_ui.root().find(id) : nullptr;
    if (!widget) return false;
    const std::string before = snapshot();
    bool changed = false;
    if (const auto* item = std::get_if<std::string>(&value)) changed = set_string(*widget, name, *item);
    else if (const auto* item = std::get_if<float>(&value)) changed = set_float(*widget, name, *item);
    else if (const auto* item = std::get_if<bool>(&value)) changed = set_bool(*widget, name, *item);
    else if (const auto* item = std::get_if<Color>(&value)) { widget->set_property(name, *item); changed = true; }
    if (changed) { m_ui.invalidate_layout(); record(EditorOperationKind::Property, before, snapshot()); }
    return changed;
}

bool EditorDocument::set_style_property(std::string selector, std::string state, std::string name, std::string value) {
    const std::string before = snapshot();
    m_ui.style().set_property(std::move(selector), std::move(state), std::move(name), std::move(value));
    record(EditorOperationKind::Style, before, snapshot());
    return true;
}

std::string EditorDocument::snapshot() const { return serialize_xml(); }

void EditorDocument::record(EditorOperationKind kind, std::string before, std::string after) {
    if (before == after) return;
    m_undo.push_back({kind, std::move(before), std::move(after)});
    m_redo.clear();
}

bool EditorDocument::restore(const std::string& source, std::string* error) {
    XmlDocument document;
    if (!document.parse(source, error) || !document.root()) return false;
    const XmlNode* root = document.root();
    m_ui.root().clear();
    if (root->name == "ui") {
        for (const XmlNode& node : root->children) {
            std::unique_ptr<Widget> widget = m_registry.create(node, error);
            if (!widget) return false;
            m_ui.root().add(std::move(widget));
        }
    } else {
        std::unique_ptr<Widget> widget = m_registry.create(*root, error);
        if (!widget) return false;
        m_ui.root().add(std::move(widget));
    }
    m_ui.invalidate_layout();
    m_selection = 0;
    return true;
}

bool EditorDocument::undo() {
    if (m_undo.empty()) return false;
    EditorOperation operation = std::move(m_undo.back());
    m_undo.pop_back();
    const std::string current = snapshot();
    if (!restore(operation.before)) return false;
    m_redo.push_back({operation.kind, operation.before, current});
    return true;
}

bool EditorDocument::redo() {
    if (m_redo.empty()) return false;
    EditorOperation operation = std::move(m_redo.back());
    m_redo.pop_back();
    const std::string current = snapshot();
    if (!restore(operation.after)) return false;
    m_undo.push_back({operation.kind, current, operation.after});
    return true;
}

std::vector<EditorComponentInfo> EditorDocument::hierarchy() const {
    std::vector<EditorComponentInfo> result;
    collect(m_ui.root(), 0, result);
    return result;
}

void EditorDocument::collect(const Widget& widget, WidgetId parentId, std::vector<EditorComponentInfo>& result) const {
    if (&widget != &m_ui.root()) result.push_back({widget.id(), parentId, widget.type_name(), widget.name, widget.styleClass, widget.styleVariant, widget.bounds(), widget.visible, widget.enabled, widget.selected});
    for (const auto& child : widget.children()) collect(*child, widget.id(), result);
}

std::string EditorDocument::serialize_xml() const {
    XmlNode root; root.name = "ui"; root.attributes["schema"] = "shinkou-ui-editor"; root.attributes["version"] = "1";
    for (const auto& child : m_ui.root().children()) { XmlNode node; child->serialize(node); root.children.push_back(std::move(node)); }
    XmlDocument document; document.set_root(std::move(root)); return document.serialize();
}

bool EditorDocument::load_xml(const std::string& source, std::string* error) {
    const bool loaded = restore(source, error);
    if (loaded) clear_history();
    return loaded;
}

void EditorDocument::clear_history() {
    m_undo.clear();
    m_redo.clear();
}

} // namespace shinkou::uikit

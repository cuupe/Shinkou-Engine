#pragma once

#include "Factory.h"
#include "Ui.h"
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace shinkou::uikit {

enum class EditorOperationKind { Add, Remove, Duplicate, Property, Reorder, Style };

struct EditorComponentInfo {
    WidgetId id = 0;
    WidgetId parentId = 0;
    std::string type;
    std::string name;
    std::string styleClass;
    std::string styleVariant;
    Rect bounds{};
    bool visible = true;
    bool enabled = true;
    bool selected = false;
};

struct EditorOperation {
    EditorOperationKind kind = EditorOperationKind::Property;
    std::string before;
    std::string after;
};

class EditorDocument {
public:
    EditorDocument();

    UiContext& ui() { return m_ui; }
    const UiContext& ui() const { return m_ui; }
    WidgetRegistry& registry() { return m_registry; }
    const WidgetRegistry& registry() const { return m_registry; }

    WidgetId selection() const { return m_selection; }
    Widget* selected();
    const Widget* selected() const;
    bool select(WidgetId id);

    WidgetId add(std::string type, WidgetId parentId = 0);
    bool remove(WidgetId id);
    WidgetId duplicate(WidgetId id);
    bool set_property(WidgetId id, std::string name, Widget::PropertyValue value);
    bool set_style_property(std::string selector, std::string state, std::string name, std::string value);
    bool undo();
    bool redo();
    bool can_undo() const { return !m_undo.empty(); }
    bool can_redo() const { return !m_redo.empty(); }

    std::vector<EditorComponentInfo> hierarchy() const;
    std::string serialize_xml() const;
    bool load_xml(const std::string& source, std::string* error = nullptr);
    void clear_history();

private:
    void collect(const Widget& widget, WidgetId parentId, std::vector<EditorComponentInfo>& result) const;
    std::string snapshot() const;
    bool restore(const std::string& source, std::string* error = nullptr);
    void record(EditorOperationKind kind, std::string before, std::string after);

    UiContext m_ui;
    WidgetRegistry m_registry;
    WidgetId m_selection = 0;
    std::vector<EditorOperation> m_undo;
    std::vector<EditorOperation> m_redo;
};

} // namespace shinkou::uikit

#pragma once

#include "shinkou/World.h"
#include <string>
#include <vector>

namespace shinkou::editor {

// Text is also the inspector edit contract. Parsing must consume the complete
// value and reject non-finite numbers before calling a component setter.
std::string property_text(const PropertyValue& value);
bool parse_property_text(PropertyType type, std::string_view text, PropertyValue& value);

struct DocumentProperty { std::string name; std::uint32_t type{0}; std::string value; };
struct DocumentComponent {
    std::string type;
    bool enabled{true};
    std::vector<DocumentProperty> properties;
    std::vector<std::string> tags;
};
struct DocumentObject {
    std::string name;
    std::int32_t parent{-1};
    bool active{true};
    std::vector<DocumentComponent> components;
};
struct EditorDocument {
    std::uint32_t version{1};
    std::int32_t selected{-1};
    std::vector<DocumentObject> objects;

    static bool capture(World& world, ObjectId selection, EditorDocument& document, std::string& error);
    bool restore(World& world, ObjectId& selection, std::string& error) const;
    bool to_json(std::string& json, std::string& error) const;
    static bool from_json(std::string_view json, EditorDocument& document, std::string& error);
};

} // namespace shinkou::editor

#include "shinkou/reflection/Reflection.h"
namespace shinkou::reflection {
SHINKOU_REFLECT_TYPE(::shinkou::editor::DocumentProperty, "editor.DocumentProperty");
SHINKOU_REFLECT_TYPE(::shinkou::editor::DocumentComponent, "editor.DocumentComponent");
SHINKOU_REFLECT_TYPE(::shinkou::editor::DocumentObject, "editor.DocumentObject");
SHINKOU_REFLECT_TYPE(::shinkou::editor::EditorDocument, "editor.Document");
SHINKOU_REFLECT_TYPE(std::vector<::shinkou::editor::DocumentProperty>, "editor.Properties");
SHINKOU_REFLECT_TYPE(std::vector<::shinkou::editor::DocumentComponent>, "editor.Components");
SHINKOU_REFLECT_TYPE(std::vector<::shinkou::editor::DocumentObject>, "editor.Objects");
SHINKOU_REFLECT_TYPE(std::vector<std::string>, "editor.Strings");
}

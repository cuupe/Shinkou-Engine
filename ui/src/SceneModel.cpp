#include "shinkou/uikit/SceneModel.h"

#include "shinkou/uikit/Xml.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace shinkou::uikit {
namespace {

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

bool finite(float value) noexcept { return std::isfinite(static_cast<double>(value)); }
bool finite(double value) noexcept { return std::isfinite(value); }

bool valid_name(std::string_view value) noexcept {
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        return static_cast<unsigned char>(character) >= 0x20u;
    });
}

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](char character) {
        return std::isspace(static_cast<unsigned char>(character)) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](char character) {
        return std::isspace(static_cast<unsigned char>(character)) != 0;
    }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::string number_text(double value) {
    std::ostringstream output;
    output << std::setprecision(9) << value;
    return output.str();
}

std::string vector_text(std::initializer_list<float> values) {
    std::ostringstream output;
    output << std::setprecision(9);
    bool first = true;
    for (float value : values) {
        if (!first) output << ',';
        first = false;
        output << value;
    }
    return output.str();
}

bool parse_unsigned(std::string_view text, SceneNodeId& result) {
    const std::string value = trim(std::string(text));
    if (value.empty()) return false;
    std::size_t consumed = 0;
    try {
        const auto parsed = std::stoull(value, &consumed, 10);
        if (consumed != value.size() || parsed == InvalidSceneNodeId ||
            parsed > std::numeric_limits<SceneNodeId>::max()) return false;
        result = static_cast<SceneNodeId>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_size(std::string_view text, std::size_t& result) {
    const std::string value = trim(std::string(text));
    if (value.empty()) return false;
    std::size_t consumed = 0;
    try {
        const auto parsed = std::stoull(value, &consumed, 10);
        if (consumed != value.size() || parsed > std::numeric_limits<std::size_t>::max()) return false;
        result = static_cast<std::size_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_bool(std::string_view text, bool& result) {
    const std::string value = trim(std::string(text));
    if (value == "true" || value == "1") { result = true; return true; }
    if (value == "false" || value == "0") { result = false; return true; }
    return false;
}

bool parse_double(std::string_view text, double& result) {
    const std::string value = trim(std::string(text));
    if (value.empty()) return false;
    std::size_t consumed = 0;
    try {
        result = std::stod(value, &consumed);
        return consumed == value.size() && finite(result);
    } catch (...) {
        return false;
    }
}

bool parse_integer(std::string_view text, std::int64_t& result) {
    const std::string value = trim(std::string(text));
    if (value.empty()) return false;
    std::size_t consumed = 0;
    try {
        result = std::stoll(value, &consumed, 10);
        return consumed == value.size();
    } catch (...) {
        return false;
    }
}

template <typename T>
bool parse_components(std::string_view text, T& result, std::size_t count) {
    std::stringstream stream(trim(std::string(text)));
    float values[4]{};
    char comma = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (!(stream >> values[index])) return false;
        if (!finite(values[index])) return false;
        if (index + 1 < count && (!(stream >> comma) || comma != ',')) return false;
    }
    stream >> std::ws;
    if (!stream.eof()) return false;
    if constexpr (std::is_same_v<T, SceneColor>) result = {values[0], values[1], values[2], values[3]};
    if constexpr (std::is_same_v<T, SceneVector2>) result = {values[0], values[1]};
    if constexpr (std::is_same_v<T, SceneVector3>) result = {values[0], values[1], values[2]};
    if constexpr (std::is_same_v<T, SceneVector4>) result = {values[0], values[1], values[2], values[3]};
    return true;
}

bool is_numeric(ScenePropertyType type) noexcept {
    return type == ScenePropertyType::Integer || type == ScenePropertyType::Number;
}

bool get_numeric(const ScenePropertyValue& value, double& result) noexcept {
    if (const auto* number = std::get_if<double>(&value)) { result = *number; return finite(result); }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) { result = static_cast<double>(*integer); return true; }
    return false;
}

bool has_ancestor(const SceneModel& model, SceneNodeId candidate, SceneNodeId ancestor) noexcept {
    const SceneNode* current = model.node(candidate);
    std::size_t guard = 0;
    while (current && current->parent != InvalidSceneNodeId && guard++ <= model.size()) {
        if (current->parent == ancestor) return true;
        current = model.node(current->parent);
    }
    return false;
}

bool contains_text(std::string_view value, std::string_view query, bool caseSensitive) {
    if (caseSensitive) return value.find(query) != std::string_view::npos;
    if (query.empty()) return true;
    auto lower = [](unsigned char character) { return static_cast<char>(std::tolower(character)); };
    std::string haystack(value.begin(), value.end());
    std::string needle(query.begin(), query.end());
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), lower);
    std::transform(needle.begin(), needle.end(), needle.begin(), lower);
    return haystack.find(needle) != std::string::npos;
}

bool value_contains_text(const ScenePropertyValue& value, std::string_view query, bool caseSensitive) {
    if (const auto* text = std::get_if<std::string>(&value)) return contains_text(*text, query, caseSensitive);
    if (const auto* boolean = std::get_if<bool>(&value)) return contains_text(*boolean ? "true" : "false", query, caseSensitive);
    if (const auto* integer = std::get_if<std::int64_t>(&value)) return contains_text(std::to_string(*integer), query, caseSensitive);
    if (const auto* number = std::get_if<double>(&value)) return contains_text(number_text(*number), query, caseSensitive);
    return false;
}

XmlNode value_node(const ScenePropertyValue& value, ScenePropertyType type) {
    XmlNode node;
    node.name = "value";
    node.attributes["type"] = scene_property_type_name(type);
    if (const auto* boolean = std::get_if<bool>(&value)) node.text = *boolean ? "true" : "false";
    else if (const auto* integer = std::get_if<std::int64_t>(&value)) node.text = std::to_string(*integer);
    else if (const auto* number = std::get_if<double>(&value)) node.text = number_text(*number);
    else if (const auto* text = std::get_if<std::string>(&value)) node.text = *text;
    else if (const auto* color = std::get_if<SceneColor>(&value)) node.text = vector_text({color->r, color->g, color->b, color->a});
    else if (const auto* vector = std::get_if<SceneVector2>(&value)) node.text = vector_text({vector->x, vector->y});
    else if (const auto* vector = std::get_if<SceneVector3>(&value)) node.text = vector_text({vector->x, vector->y, vector->z});
    else if (const auto* vector = std::get_if<SceneVector4>(&value)) node.text = vector_text({vector->x, vector->y, vector->z, vector->w});
    return node;
}

bool parse_value(const XmlNode& node, ScenePropertyType type, ScenePropertyValue& result, std::string* error) {
    const std::string text = node.text;
    switch (type) {
    case ScenePropertyType::Null: result = std::monostate{}; return trim(text).empty();
    case ScenePropertyType::Boolean: {
        bool value = false;
        if (!parse_bool(text, value)) { set_error(error, "invalid boolean property value"); return false; }
        result = value; return true;
    }
    case ScenePropertyType::Integer: {
        std::int64_t value = 0;
        if (!parse_integer(text, value)) { set_error(error, "invalid integer property value"); return false; }
        result = value; return true;
    }
    case ScenePropertyType::Number: {
        double value = 0.0;
        if (!parse_double(text, value)) { set_error(error, "invalid number property value"); return false; }
        result = value; return true;
    }
    case ScenePropertyType::Color: {
        SceneColor value;
        if (!parse_components(text, value, 4)) { set_error(error, "invalid color property value"); return false; }
        result = value; return true;
    }
    case ScenePropertyType::Vector2: {
        SceneVector2 value;
        if (!parse_components(text, value, 2)) { set_error(error, "invalid vector2 property value"); return false; }
        result = value; return true;
    }
    case ScenePropertyType::Vector3: {
        SceneVector3 value;
        if (!parse_components(text, value, 3)) { set_error(error, "invalid vector3 property value"); return false; }
        result = value; return true;
    }
    case ScenePropertyType::Vector4: {
        SceneVector4 value;
        if (!parse_components(text, value, 4)) { set_error(error, "invalid vector4 property value"); return false; }
        result = value; return true;
    }
    default: result = text; return true;
    }
}

const XmlNode* value_child(const XmlNode& node) {
    return node.child("value") ? node.child("value") : &node;
}

void add_value_child(XmlNode& parent, const ScenePropertyValue& value, ScenePropertyType type) {
    parent.children.push_back(value_node(value, type));
}

XmlNode serialize_field(const InspectorField& field) {
    XmlNode node;
    node.name = "field";
    node.attributes["id"] = field.id;
    node.attributes["label"] = field.label;
    node.attributes["tooltip"] = field.tooltip;
    node.attributes["type"] = scene_property_type_name(field.type);
    node.attributes["required"] = field.required ? "true" : "false";
    node.attributes["readOnly"] = field.readOnly ? "true" : "false";
    node.attributes["hasRange"] = field.hasRange ? "true" : "false";
    node.attributes["minimum"] = number_text(field.minimum);
    node.attributes["maximum"] = number_text(field.maximum);
    node.attributes["step"] = number_text(field.step);
    node.attributes["maxLength"] = std::to_string(field.maxLength);
    add_value_child(node, field.value, field.type);
    if (field.defaultValue) {
        XmlNode defaultNode = value_node(*field.defaultValue, field.type);
        defaultNode.name = "default";
        node.children.push_back(std::move(defaultNode));
    }
    if (!field.enumOptions.empty()) {
        XmlNode options;
        options.name = "options";
        for (const auto& option : field.enumOptions) {
            XmlNode item;
            item.name = "option";
            item.text = option;
            options.children.push_back(std::move(item));
        }
        node.children.push_back(std::move(options));
    }
    return node;
}

XmlNode serialize_node(const SceneNode& source) {
    XmlNode node;
    node.name = "node";
    node.attributes["id"] = std::to_string(source.id);
    node.attributes["name"] = source.name;
    node.attributes["type"] = source.type;
    node.attributes["visible"] = source.visible ? "true" : "false";
    node.attributes["enabled"] = source.enabled ? "true" : "false";
    node.attributes["locked"] = source.locked ? "true" : "false";

    if (!source.properties.empty()) {
        XmlNode properties;
        properties.name = "properties";
        for (const auto& [name, property] : source.properties) {
            XmlNode item;
            item.name = "property";
            item.attributes["name"] = name;
            item.attributes["type"] = scene_property_type_name(property.type);
            add_value_child(item, property.value, property.type);
            properties.children.push_back(std::move(item));
        }
        node.children.push_back(std::move(properties));
    }

    if (!source.inspector.empty()) {
        XmlNode inspector;
        inspector.name = "inspector";
        for (const auto& section : source.inspector) {
            XmlNode sectionNode;
            sectionNode.name = "section";
            sectionNode.attributes["id"] = section.id;
            sectionNode.attributes["title"] = section.title;
            sectionNode.attributes["expanded"] = section.expanded ? "true" : "false";
            for (const auto& field : section.fields) sectionNode.children.push_back(serialize_field(field));
            inspector.children.push_back(std::move(sectionNode));
        }
        node.children.push_back(std::move(inspector));
    }

    if (!source.children.empty()) {
        XmlNode children;
        children.name = "children";
        for (SceneNodeId childId : source.children) {
            // The caller only serializes validated models, so the lookup is
            // performed by SceneModel::serialize_xml before this helper.
            XmlNode reference;
            reference.name = "child";
            reference.attributes["id"] = std::to_string(childId);
            children.children.push_back(std::move(reference));
        }
        node.children.push_back(std::move(children));
    }
    return node;
}

bool parse_field(const XmlNode& node, InspectorField& result, std::string* error) {
    result = {};
    result.id = node.attribute("id");
    result.label = node.attribute("label");
    result.tooltip = node.attribute("tooltip");
    if (!scene_property_type_from_name(node.attribute("type"), result.type)) {
        set_error(error, "unknown inspector field type"); return false;
    }
    if (result.id.empty()) { set_error(error, "inspector field id is empty"); return false; }
    parse_bool(node.attribute("required", "false"), result.required);
    parse_bool(node.attribute("readOnly", "false"), result.readOnly);
    parse_bool(node.attribute("hasRange", "false"), result.hasRange);
    parse_double(node.attribute("minimum", "0"), result.minimum);
    parse_double(node.attribute("maximum", "0"), result.maximum);
    parse_double(node.attribute("step", "0"), result.step);
    parse_size(node.attribute("maxLength", "0"), result.maxLength);

    const XmlNode* value = value_child(node);
    if (!parse_value(*value, result.type, result.value, error)) return false;
    if (const auto* defaultNode = node.child("default")) {
        ScenePropertyValue defaultValue;
        if (!parse_value(*defaultNode, result.type, defaultValue, error)) return false;
        result.defaultValue = std::move(defaultValue);
    }
    if (const auto* options = node.child("options")) {
        for (const auto* option : options->children_named("option")) result.enumOptions.push_back(option->text);
    }
    return result.valid(error);
}

bool parse_scene_node(const XmlNode& node, SceneNodeId parent, std::map<SceneNodeId, SceneNode>& nodes,
                      std::string* error) {
    if (node.name != "node") { set_error(error, "scene node element is invalid"); return false; }
    SceneNode result;
    if (!parse_unsigned(node.attribute("id"), result.id)) { set_error(error, "scene node id is invalid"); return false; }
    if (!nodes.emplace(result.id, SceneNode{}).second) { set_error(error, "duplicate scene node id"); return false; }
    result.parent = parent;
    result.name = node.attribute("name");
    result.type = node.attribute("type", "Node");
    if (result.name.empty() || result.type.empty()) { set_error(error, "scene node name or type is empty"); return false; }
    if (!parse_bool(node.attribute("visible", "true"), result.visible) ||
        !parse_bool(node.attribute("enabled", "true"), result.enabled) ||
        !parse_bool(node.attribute("locked", "false"), result.locked)) {
        set_error(error, "scene node boolean attribute is invalid"); return false;
    }

    if (const auto* properties = node.child("properties")) {
        for (const auto* propertyNode : properties->children_named("property")) {
            const std::string name = propertyNode->attribute("name");
            SceneProperty property;
            if (name.empty() || !scene_property_type_from_name(propertyNode->attribute("type"), property.type)) {
                set_error(error, "scene property name or type is invalid"); return false;
            }
            if (!parse_value(*value_child(*propertyNode), property.type, property.value, error) ||
                !property.valid(error) || !result.properties.emplace(name, std::move(property)).second) {
                if (error && error->empty()) *error = "duplicate or invalid scene property";
                return false;
            }
        }
    }

    if (const auto* inspector = node.child("inspector")) {
        for (const auto* sectionNode : inspector->children_named("section")) {
            InspectorSection section;
            section.id = sectionNode->attribute("id");
            section.title = sectionNode->attribute("title");
            if (!parse_bool(sectionNode->attribute("expanded", "true"), section.expanded) ||
                section.id.empty()) { set_error(error, "inspector section is invalid"); return false; }
            for (const auto* fieldNode : sectionNode->children_named("field")) {
                InspectorField field;
                if (!parse_field(*fieldNode, field, error)) return false;
                const auto duplicate = std::find_if(section.fields.begin(), section.fields.end(),
                    [&](const InspectorField& item) { return item.id == field.id; });
                if (duplicate != section.fields.end()) { set_error(error, "duplicate inspector field id"); return false; }
                section.fields.push_back(std::move(field));
            }
            if (!section.valid(error)) return false;
            const auto duplicate = std::find_if(result.inspector.begin(), result.inspector.end(),
                [&](const InspectorSection& item) { return item.id == section.id; });
            if (duplicate != result.inspector.end()) { set_error(error, "duplicate inspector section id"); return false; }
            result.inspector.push_back(std::move(section));
        }
    }
    if (!result.valid(error)) return false;
    nodes[result.id] = std::move(result);

    if (const auto* children = node.child("children")) {
        for (const auto* child : children->children_named("child")) {
            SceneNodeId childId = InvalidSceneNodeId;
            if (!parse_unsigned(child->attribute("id"), childId)) { set_error(error, "scene child id is invalid"); return false; }
            // Child references are resolved after all node records have been
            // parsed; the nested node form is used by the serializer below.
            if (childId == InvalidSceneNodeId) { set_error(error, "scene child id is invalid"); return false; }
            nodes[result.id].children.push_back(childId);
        }
    }
    return true;
}

} // namespace

const char* scene_property_type_name(ScenePropertyType type) noexcept {
    switch (type) {
    case ScenePropertyType::Null: return "null";
    case ScenePropertyType::Boolean: return "boolean";
    case ScenePropertyType::Integer: return "integer";
    case ScenePropertyType::Number: return "number";
    case ScenePropertyType::String: return "string";
    case ScenePropertyType::MultilineText: return "multiline";
    case ScenePropertyType::Color: return "color";
    case ScenePropertyType::Vector2: return "vector2";
    case ScenePropertyType::Vector3: return "vector3";
    case ScenePropertyType::Vector4: return "vector4";
    case ScenePropertyType::Enum: return "enum";
    case ScenePropertyType::Asset: return "asset";
    case ScenePropertyType::NodeReference: return "node-reference";
    }
    return "null";
}

bool scene_property_type_from_name(std::string_view name, ScenePropertyType& type) noexcept {
    constexpr ScenePropertyType values[] = {ScenePropertyType::Null, ScenePropertyType::Boolean,
        ScenePropertyType::Integer, ScenePropertyType::Number, ScenePropertyType::String,
        ScenePropertyType::MultilineText, ScenePropertyType::Color, ScenePropertyType::Vector2,
        ScenePropertyType::Vector3, ScenePropertyType::Vector4, ScenePropertyType::Enum,
        ScenePropertyType::Asset, ScenePropertyType::NodeReference};
    for (const ScenePropertyType candidate : values) {
        if (name == scene_property_type_name(candidate)) { type = candidate; return true; }
    }
    return false;
}

bool scene_property_value_matches(ScenePropertyType type, const ScenePropertyValue& value) noexcept {
    switch (type) {
    case ScenePropertyType::Null: return std::holds_alternative<std::monostate>(value);
    case ScenePropertyType::Boolean: return std::holds_alternative<bool>(value);
    case ScenePropertyType::Integer: return std::holds_alternative<std::int64_t>(value);
    case ScenePropertyType::Number: return std::holds_alternative<double>(value) || std::holds_alternative<std::int64_t>(value);
    case ScenePropertyType::Color: return std::holds_alternative<SceneColor>(value);
    case ScenePropertyType::Vector2: return std::holds_alternative<SceneVector2>(value);
    case ScenePropertyType::Vector3: return std::holds_alternative<SceneVector3>(value);
    case ScenePropertyType::Vector4: return std::holds_alternative<SceneVector4>(value);
    default: return std::holds_alternative<std::string>(value);
    }
}

bool SceneProperty::valid(std::string* error) const {
    if (!scene_property_value_matches(type, value)) {
        set_error(error, "scene property value does not match its type"); return false;
    }
    if (const auto* number = std::get_if<double>(&value); number && !finite(*number)) {
        set_error(error, "scene number is not finite"); return false;
    }
    if (const auto* color = std::get_if<SceneColor>(&value)) {
        if (!finite(color->r) || !finite(color->g) || !finite(color->b) || !finite(color->a)) {
            set_error(error, "scene color contains a non-finite component"); return false;
        }
    }
    if (const auto* vector = std::get_if<SceneVector2>(&value)) {
        if (!finite(vector->x) || !finite(vector->y)) { set_error(error, "scene vector2 is not finite"); return false; }
    }
    if (const auto* vector = std::get_if<SceneVector3>(&value)) {
        if (!finite(vector->x) || !finite(vector->y) || !finite(vector->z)) { set_error(error, "scene vector3 is not finite"); return false; }
    }
    if (const auto* vector = std::get_if<SceneVector4>(&value)) {
        if (!finite(vector->x) || !finite(vector->y) || !finite(vector->z) || !finite(vector->w)) { set_error(error, "scene vector4 is not finite"); return false; }
    }
    return true;
}

bool InspectorField::validate_value(const ScenePropertyValue& candidate, std::string* error) const {
    if (!scene_property_value_matches(type, candidate)) {
        set_error(error, "inspector value does not match its type"); return false;
    }
    SceneProperty candidateProperty{type, candidate};
    if (!candidateProperty.valid(error)) return false;
    if (required) {
        if (const auto* text = std::get_if<std::string>(&candidate); text && text->empty()) {
            set_error(error, "required inspector value is empty"); return false;
        }
    }
    if (type == ScenePropertyType::Enum) {
        const auto* text = std::get_if<std::string>(&candidate);
        if (!text || std::find(enumOptions.begin(), enumOptions.end(), *text) == enumOptions.end()) {
            set_error(error, "inspector enum value is not an option"); return false;
        }
    }
    if (maxLength != 0) {
        const auto* text = std::get_if<std::string>(&candidate);
        if (!text || text->size() > maxLength) { set_error(error, "inspector text exceeds max length"); return false; }
    }
    if (hasRange) {
        double number = 0.0;
        if (!is_numeric(type) || !get_numeric(candidate, number) || !finite(number) ||
            !finite(minimum) || !finite(maximum) || minimum > maximum || number < minimum || number > maximum) {
            set_error(error, "inspector numeric value is outside its range"); return false;
        }
    }
    return true;
}

bool InspectorField::valid(std::string* error) const {
    if (!valid_name(id) || !valid_name(label.empty() ? id : label)) {
        set_error(error, "inspector field id or label is invalid"); return false;
    }
    if (hasRange && (!is_numeric(type) || !finite(minimum) || !finite(maximum) || minimum > maximum ||
                     !finite(step) || step < 0.0)) {
        set_error(error, "inspector field range is invalid"); return false;
    }
    if (type == ScenePropertyType::Enum && enumOptions.empty()) {
        set_error(error, "enum inspector field has no options"); return false;
    }
    if (!validate_value(value, error)) return false;
    if (defaultValue && !validate_value(*defaultValue, error)) return false;
    return true;
}

bool InspectorSection::valid(std::string* error) const {
    if (!valid_name(id) || !valid_name(title.empty() ? id : title)) {
        set_error(error, "inspector section id or title is invalid"); return false;
    }
    std::unordered_set<std::string> ids;
    for (const auto& field : fields) {
        if (!ids.insert(field.id).second || !field.valid(error)) {
            if (error && error->empty()) *error = "duplicate or invalid inspector field";
            return false;
        }
    }
    return true;
}

bool SceneNode::valid(std::string* error) const {
    if (id == InvalidSceneNodeId || !valid_name(name) || !valid_name(type)) {
        set_error(error, "scene node id, name or type is invalid"); return false;
    }
    std::unordered_set<SceneNodeId> childIds;
    for (SceneNodeId child : children) {
        if (child == InvalidSceneNodeId || !childIds.insert(child).second) {
            set_error(error, "scene node has duplicate or invalid child id"); return false;
        }
    }
    for (const auto& [name, property] : properties) {
        if (!valid_name(name) || !property.valid(error)) return false;
    }
    std::unordered_set<std::string> sectionIds;
    for (const auto& section : inspector) {
        if (!sectionIds.insert(section.id).second || !section.valid(error)) {
            if (error && error->empty()) *error = "duplicate or invalid inspector section";
            return false;
        }
    }
    return true;
}

SceneModel::SceneModel() {
    rootId_ = nextId_++;
    SceneNode root;
    root.id = rootId_;
    root.name = "Scene";
    root.type = "Scene";
    nodes_.emplace(root.id, std::move(root));
}

SceneNodeId SceneModel::root_id() const noexcept { return rootId_; }
SceneNode* SceneModel::root() noexcept { return node(rootId_); }
const SceneNode* SceneModel::root() const noexcept { return node(rootId_); }
SceneNode* SceneModel::node(SceneNodeId id) noexcept {
    const auto found = nodes_.find(id);
    return found == nodes_.end() ? nullptr : &found->second;
}
const SceneNode* SceneModel::node(SceneNodeId id) const noexcept {
    const auto found = nodes_.find(id);
    return found == nodes_.end() ? nullptr : &found->second;
}
bool SceneModel::contains(SceneNodeId id) const noexcept { return nodes_.find(id) != nodes_.end(); }
std::size_t SceneModel::size() const noexcept { return nodes_.size(); }

SceneNodeId SceneModel::add_node(SceneNodeId parent, std::string name, std::string type) {
    SceneNode node;
    node.id = nextId_++;
    node.name = std::move(name);
    node.type = std::move(type);
    std::string error;
    if (!insert_node(std::move(node), parent, SceneAppendIndex, &error)) return InvalidSceneNodeId;
    return nextId_ - 1;
}

bool SceneModel::insert_node(SceneNode value, SceneNodeId parent, std::size_t index, std::string* error) {
    SceneNode* parentNode = node(parent);
    if (!parentNode || value.id == InvalidSceneNodeId || value.id == rootId_ || !value.children.empty()) {
        set_error(error, "scene node parent, id or children are invalid"); return false;
    }
    if (nodes_.find(value.id) != nodes_.end() || !value.valid(error)) return false;
    value.parent = parent;
    const SceneNodeId id = value.id;
    nodes_.emplace(id, std::move(value));
    const std::size_t insertAt = index == SceneAppendIndex ? parentNode->children.size() : std::min(index, parentNode->children.size());
    parentNode->children.insert(parentNode->children.begin() + static_cast<std::ptrdiff_t>(insertAt), id);
    nextId_ = std::max(nextId_, id + 1);
    return true;
}

bool SceneModel::remove_node(SceneNodeId id, std::string* error) {
    SceneNode* target = node(id);
    if (!target || id == rootId_) { set_error(error, "cannot remove missing or root scene node"); return false; }
    const SceneNodeId parent = target->parent;
    if (SceneNode* parentNode = node(parent)) {
        parentNode->children.erase(std::remove(parentNode->children.begin(), parentNode->children.end(), id), parentNode->children.end());
    }
    std::vector<SceneNodeId> removed;
    collect(id, removed);
    for (SceneNodeId removedId : removed) { nodes_.erase(removedId); remove_from_selection(removedId); }
    return true;
}

bool SceneModel::rename_node(SceneNodeId id, std::string name, std::string* error) {
    SceneNode* target = node(id);
    if (!target || !valid_name(name)) { set_error(error, "scene node name is invalid"); return false; }
    target->name = std::move(name);
    return true;
}

bool SceneModel::set_node_type(SceneNodeId id, std::string type, std::string* error) {
    SceneNode* target = node(id);
    if (!target || !valid_name(type)) { set_error(error, "scene node type is invalid"); return false; }
    target->type = std::move(type);
    return true;
}

bool SceneModel::is_descendant(SceneNodeId candidate, SceneNodeId ancestor) const noexcept {
    return candidate != InvalidSceneNodeId && ancestor != InvalidSceneNodeId && has_ancestor(*this, candidate, ancestor);
}

bool SceneModel::move_node(SceneNodeId id, SceneNodeId newParent, std::size_t index, std::string* error) {
    SceneNode* target = node(id);
    SceneNode* destination = node(newParent);
    if (!target || !destination || id == rootId_ || id == newParent || is_descendant(newParent, id)) {
        set_error(error, "scene node move would create an invalid hierarchy"); return false;
    }
    SceneNode* oldParent = node(target->parent);
    if (!oldParent) { set_error(error, "scene node has no valid parent"); return false; }
    const auto old = std::find(oldParent->children.begin(), oldParent->children.end(), id);
    if (old == oldParent->children.end()) { set_error(error, "scene node is missing from its parent"); return false; }
    oldParent->children.erase(old);
    const std::size_t insertAt = index == SceneAppendIndex ? destination->children.size() : std::min(index, destination->children.size());
    destination->children.insert(destination->children.begin() + static_cast<std::ptrdiff_t>(insertAt), id);
    target->parent = newParent;
    return true;
}

bool SceneModel::reorder_node(SceneNodeId id, std::size_t index, std::string* error) {
    SceneNode* target = node(id);
    if (!target || id == rootId_) { set_error(error, "cannot reorder missing or root scene node"); return false; }
    return move_node(id, target->parent, index, error);
}

bool SceneModel::select(SceneNodeId id, SelectionMode mode) {
    if (id == InvalidSceneNodeId) { clear_selection(); return mode == SelectionMode::Replace; }
    if (!contains(id)) return false;
    const auto found = std::find(selection_.begin(), selection_.end(), id);
    const bool selected = found != selection_.end();
    switch (mode) {
    case SelectionMode::Replace: selection_.assign(1, id); break;
    case SelectionMode::Add: if (!selected) selection_.push_back(id); break;
    case SelectionMode::Toggle: if (selected) selection_.erase(found); else selection_.push_back(id); break;
    case SelectionMode::Remove: if (selected) selection_.erase(found); break;
    }
    return true;
}

bool SceneModel::set_selection(const std::vector<SceneNodeId>& ids) {
    std::vector<SceneNodeId> next;
    for (SceneNodeId id : ids) {
        if (!contains(id)) return false;
        if (std::find(next.begin(), next.end(), id) == next.end()) next.push_back(id);
    }
    selection_ = std::move(next);
    return true;
}

void SceneModel::clear_selection() noexcept { selection_.clear(); }
const std::vector<SceneNodeId>& SceneModel::selection() const noexcept { return selection_; }
bool SceneModel::is_selected(SceneNodeId id) const noexcept {
    return std::find(selection_.begin(), selection_.end(), id) != selection_.end();
}

void SceneModel::collect(SceneNodeId id, std::vector<SceneNodeId>& result) const {
    const SceneNode* current = node(id);
    if (!current) return;
    result.push_back(id);
    for (SceneNodeId child : current->children) collect(child, result);
}

std::vector<SceneNodeId> SceneModel::flatten() const {
    std::vector<SceneNodeId> result;
    result.reserve(nodes_.size());
    collect(rootId_, result);
    return result;
}

std::vector<SceneNodeId> SceneModel::search(std::string_view query, const SceneSearchOptions& options) const {
    std::vector<SceneNodeId> result;
    const auto ordered = flatten();
    std::unordered_set<SceneNodeId> matches;
    for (SceneNodeId id : ordered) {
        const SceneNode* current = node(id);
        bool match = contains_text(current->name, query, options.caseSensitive);
        match = match || (options.searchType && contains_text(current->type, query, options.caseSensitive));
        if (!match && options.searchProperties) {
            for (const auto& [name, property] : current->properties) {
                if (contains_text(name, query, options.caseSensitive) || value_contains_text(property.value, query, options.caseSensitive)) {
                    match = true; break;
                }
            }
        }
        if (match) matches.insert(id);
    }
    if (options.includeAncestors) {
        for (SceneNodeId id : std::vector<SceneNodeId>(matches.begin(), matches.end())) {
            const SceneNode* current = node(id);
            while (current && current->parent != InvalidSceneNodeId) {
                matches.insert(current->parent);
                current = node(current->parent);
            }
        }
    }
    for (SceneNodeId id : ordered) if (matches.find(id) != matches.end()) result.push_back(id);
    return result;
}

InspectorSection* SceneModel::inspector_section(SceneNodeId nodeId, std::string_view sectionId) noexcept {
    SceneNode* current = node(nodeId);
    if (!current) return nullptr;
    const auto found = std::find_if(current->inspector.begin(), current->inspector.end(),
        [&](const InspectorSection& section) { return section.id == sectionId; });
    return found == current->inspector.end() ? nullptr : &*found;
}
const InspectorSection* SceneModel::inspector_section(SceneNodeId nodeId, std::string_view sectionId) const noexcept {
    const SceneNode* current = node(nodeId);
    if (!current) return nullptr;
    const auto found = std::find_if(current->inspector.begin(), current->inspector.end(),
        [&](const InspectorSection& section) { return section.id == sectionId; });
    return found == current->inspector.end() ? nullptr : &*found;
}

bool SceneModel::add_inspector_section(SceneNodeId nodeId, InspectorSection section, std::string* error) {
    SceneNode* current = node(nodeId);
    if (!current || !section.valid(error) || inspector_section(nodeId, section.id)) {
        if (error && error->empty()) *error = "inspector section is missing or already exists";
        return false;
    }
    current->inspector.push_back(std::move(section));
    return true;
}

bool SceneModel::remove_inspector_section(SceneNodeId nodeId, std::string_view sectionId, std::string* error) {
    SceneNode* current = node(nodeId);
    if (!current) { set_error(error, "scene node is missing"); return false; }
    const auto found = std::find_if(current->inspector.begin(), current->inspector.end(),
        [&](const InspectorSection& section) { return section.id == sectionId; });
    if (found == current->inspector.end()) { set_error(error, "inspector section is missing"); return false; }
    current->inspector.erase(found);
    return true;
}

InspectorField* SceneModel::inspector_field(SceneNodeId nodeId, std::string_view sectionId,
                                            std::string_view fieldId) noexcept {
    InspectorSection* section = inspector_section(nodeId, sectionId);
    if (!section) return nullptr;
    const auto found = std::find_if(section->fields.begin(), section->fields.end(),
        [&](const InspectorField& field) { return field.id == fieldId; });
    return found == section->fields.end() ? nullptr : &*found;
}
const InspectorField* SceneModel::inspector_field(SceneNodeId nodeId, std::string_view sectionId,
                                                  std::string_view fieldId) const noexcept {
    const InspectorSection* section = inspector_section(nodeId, sectionId);
    if (!section) return nullptr;
    const auto found = std::find_if(section->fields.begin(), section->fields.end(),
        [&](const InspectorField& field) { return field.id == fieldId; });
    return found == section->fields.end() ? nullptr : &*found;
}

bool SceneModel::add_inspector_field(SceneNodeId nodeId, std::string_view sectionId,
                                     InspectorField field, std::string* error) {
    InspectorSection* section = inspector_section(nodeId, sectionId);
    if (!section || !field.valid(error)) {
        if (error && error->empty()) *error = "inspector section is missing or field is invalid";
        return false;
    }
    if (inspector_field(nodeId, sectionId, field.id)) { set_error(error, "inspector field already exists"); return false; }
    section->fields.push_back(std::move(field));
    return true;
}

bool SceneModel::remove_inspector_field(SceneNodeId nodeId, std::string_view sectionId,
                                        std::string_view fieldId, std::string* error) {
    InspectorSection* section = inspector_section(nodeId, sectionId);
    if (!section) { set_error(error, "inspector section is missing"); return false; }
    const auto found = std::find_if(section->fields.begin(), section->fields.end(),
        [&](const InspectorField& field) { return field.id == fieldId; });
    if (found == section->fields.end()) { set_error(error, "inspector field is missing"); return false; }
    section->fields.erase(found);
    return true;
}

bool SceneModel::set_inspector_value(SceneNodeId nodeId, std::string_view sectionId,
                                     std::string_view fieldId, ScenePropertyValue value, std::string* error) {
    InspectorField* field = inspector_field(nodeId, sectionId, fieldId);
    if (!field) { set_error(error, "inspector field is missing"); return false; }
    if (field->readOnly) { set_error(error, "inspector field is read-only"); return false; }
    if (!field->validate_value(value, error)) return false;
    field->value = std::move(value);
    return true;
}

bool SceneModel::valid(std::string* error) const {
    const SceneNode* rootNode = root();
    if (!rootNode || rootNode->parent != InvalidSceneNodeId || nodes_.find(rootId_) == nodes_.end()) {
        set_error(error, "scene root is invalid"); return false;
    }
    for (const auto& [id, current] : nodes_) {
        if (id != current.id || !current.valid(error)) return false;
        if (id != rootId_) {
            const SceneNode* parentNode = node(current.parent);
            if (!parentNode || std::find(parentNode->children.begin(), parentNode->children.end(), id) == parentNode->children.end()) {
                set_error(error, "scene node parent relationship is invalid"); return false;
            }
        }
        for (SceneNodeId child : current.children) {
            const SceneNode* childNode = node(child);
            if (!childNode || childNode->parent != id) { set_error(error, "scene child relationship is invalid"); return false; }
        }
    }
    std::vector<SceneNodeId> ordered = flatten();
    if (ordered.size() != nodes_.size()) { set_error(error, "scene hierarchy contains a cycle or disconnected node"); return false; }
    for (SceneNodeId id : selection_) if (!contains(id)) { set_error(error, "scene selection contains a missing node"); return false; }
    return true;
}

void SceneModel::remove_from_selection(SceneNodeId id) noexcept {
    selection_.erase(std::remove(selection_.begin(), selection_.end(), id), selection_.end());
}

std::string SceneModel::serialize_xml(bool /*pretty*/) const {
    if (!valid()) return {};
    XmlNode documentNode;
    documentNode.name = "scene-model";
    documentNode.attributes["schema"] = std::string(kSceneModelSchema);
    documentNode.attributes["version"] = std::to_string(kSceneModelSchemaVersion);
    documentNode.attributes["root"] = std::to_string(rootId_);
    documentNode.attributes["nextId"] = std::to_string(nextId_);
    XmlNode rootNode = serialize_node(*root());
    // Replace the child references emitted by the generic helper with nested
    // nodes, keeping the file self-contained and preserving sibling order.
    std::function<void(XmlNode&, const SceneNode&)> nest = [&](XmlNode& target, const SceneNode& source) {
        target.children.erase(std::remove_if(target.children.begin(), target.children.end(),
            [](const XmlNode& item) { return item.name == "children"; }), target.children.end());
        if (source.children.empty()) return;
        XmlNode children;
        children.name = "children";
        for (SceneNodeId childId : source.children) {
            XmlNode child = serialize_node(*node(childId));
            nest(child, *node(childId));
            children.children.push_back(std::move(child));
        }
        target.children.push_back(std::move(children));
    };
    nest(rootNode, *root());
    documentNode.children.push_back(std::move(rootNode));
    XmlDocument document;
    document.set_root(std::move(documentNode));
    return document.serialize();
}

bool SceneModel::deserialize_xml(std::string_view xml, std::string* error) {
    if (xml.empty() || xml.size() > 4u * 1024u * 1024u) { set_error(error, "scene XML is empty or too large"); return false; }
    XmlDocument document;
    if (!document.parse(std::string(xml), error) || !document.root()) return false;
    const XmlNode& rootElement = *document.root();
    if (rootElement.name != "scene-model" || rootElement.attribute("schema") != kSceneModelSchema ||
        rootElement.attribute("version") != std::to_string(kSceneModelSchemaVersion)) {
        set_error(error, "unsupported scene model XML schema/version"); return false;
    }
    const XmlNode* rootRecord = rootElement.child("node");
    if (!rootRecord) { set_error(error, "scene model root node is missing"); return false; }
    SceneNodeId parsedRoot = InvalidSceneNodeId;
    if (!parse_unsigned(rootElement.attribute("root"), parsedRoot)) { set_error(error, "scene model root id is invalid"); return false; }

    std::map<SceneNodeId, SceneNode> parsedNodes;
    // Parse nested records in one pass and establish parent/child links from
    // the nesting itself instead of trusting serialized parent attributes.
    std::function<bool(const XmlNode&, SceneNodeId)> parse_nested = [&](const XmlNode& nodeElement, SceneNodeId parent) {
        SceneNodeId currentId = InvalidSceneNodeId;
        if (!parse_unsigned(nodeElement.attribute("id"), currentId)) {
            set_error(error, "scene node id is invalid"); return false;
        }
        if (!parse_scene_node(nodeElement, parent, parsedNodes, error)) return false;
        const auto* children = nodeElement.child("children");
        if (!children) return true;
        for (const auto& child : children->children) {
            if (child.name != "node") { set_error(error, "scene children must contain node elements"); return false; }
            if (!parse_nested(child, currentId)) return false;
            SceneNodeId childId = InvalidSceneNodeId;
            if (!parse_unsigned(child.attribute("id"), childId)) { set_error(error, "scene child id is invalid"); return false; }
            parsedNodes[currentId].children.push_back(childId);
        }
        return true;
    };
    if (!parse_nested(*rootRecord, InvalidSceneNodeId)) return false;
    if (parsedNodes.find(parsedRoot) == parsedNodes.end()) { set_error(error, "scene model root record is missing"); return false; }
    if (parsedNodes[parsedRoot].parent != InvalidSceneNodeId) { set_error(error, "scene model root has a parent"); return false; }
    for (const auto& [id, current] : parsedNodes) {
        for (SceneNodeId child : current.children) {
            const auto found = parsedNodes.find(child);
            if (found == parsedNodes.end() || found->second.parent != id) { set_error(error, "scene model child relationship is invalid"); return false; }
        }
    }

    SceneNodeId parsedNext = InvalidSceneNodeId;
    if (!parse_unsigned(rootElement.attribute("nextId"), parsedNext)) parsedNext = 1;
    for (const auto& [id, unused] : parsedNodes) parsedNext = std::max(parsedNext, id + 1);
    SceneModel candidate;
    candidate.nodes_ = std::move(parsedNodes);
    candidate.rootId_ = parsedRoot;
    candidate.nextId_ = parsedNext;
    candidate.selection_.clear();
    if (!candidate.valid(error)) return false;
    nodes_ = std::move(candidate.nodes_);
    rootId_ = candidate.rootId_;
    nextId_ = candidate.nextId_;
    selection_.clear();
    if (error) error->clear();
    return true;
}

} // namespace shinkou::uikit

#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace shinkou::uikit {

using SceneNodeId = std::uint64_t;
inline constexpr SceneNodeId InvalidSceneNodeId = 0;
inline constexpr std::uint32_t kSceneModelSchemaVersion = 1;
inline constexpr std::string_view kSceneModelSchema = "shinkou-uikit-scene";
inline constexpr std::size_t SceneAppendIndex = static_cast<std::size_t>(-1);

struct SceneColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    friend bool operator==(const SceneColor& left, const SceneColor& right) {
        return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
    }
};

struct SceneVector2 {
    float x = 0.0f;
    float y = 0.0f;
    friend bool operator==(const SceneVector2& left, const SceneVector2& right) {
        return left.x == right.x && left.y == right.y;
    }
};

struct SceneVector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    friend bool operator==(const SceneVector3& left, const SceneVector3& right) {
        return left.x == right.x && left.y == right.y && left.z == right.z;
    }
};

struct SceneVector4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
    friend bool operator==(const SceneVector4& left, const SceneVector4& right) {
        return left.x == right.x && left.y == right.y && left.z == right.z && left.w == right.w;
    }
};

enum class ScenePropertyType : std::uint8_t {
    Null,
    Boolean,
    Integer,
    Number,
    String,
    MultilineText,
    Color,
    Vector2,
    Vector3,
    Vector4,
    Enum,
    Asset,
    NodeReference
};

using ScenePropertyValue = std::variant<std::monostate, bool, std::int64_t, double, std::string,
                                        SceneColor, SceneVector2, SceneVector3, SceneVector4>;

const char* scene_property_type_name(ScenePropertyType type) noexcept;
bool scene_property_type_from_name(std::string_view name, ScenePropertyType& type) noexcept;
bool scene_property_value_matches(ScenePropertyType type, const ScenePropertyValue& value) noexcept;

struct SceneProperty {
    ScenePropertyType type = ScenePropertyType::String;
    ScenePropertyValue value = std::string{};

    bool valid(std::string* error = nullptr) const;
};

struct InspectorField {
    std::string id;
    std::string label;
    std::string tooltip;
    ScenePropertyType type = ScenePropertyType::String;
    ScenePropertyValue value = std::string{};
    std::optional<ScenePropertyValue> defaultValue;
    std::vector<std::string> enumOptions;
    double minimum = 0.0;
    double maximum = 0.0;
    double step = 0.0;
    std::size_t maxLength = 0;
    bool hasRange = false;
    bool required = false;
    bool readOnly = false;

    bool valid(std::string* error = nullptr) const;
    bool validate_value(const ScenePropertyValue& candidate, std::string* error = nullptr) const;
};

struct InspectorSection {
    std::string id;
    std::string title;
    std::vector<InspectorField> fields;
    bool expanded = true;

    bool valid(std::string* error = nullptr) const;
};

struct SceneNode {
    SceneNodeId id = InvalidSceneNodeId;
    SceneNodeId parent = InvalidSceneNodeId;
    std::string name;
    std::string type = "Node";
    bool visible = true;
    bool enabled = true;
    bool locked = false;
    std::vector<SceneNodeId> children;
    std::map<std::string, SceneProperty, std::less<>> properties;
    std::vector<InspectorSection> inspector;

    bool valid(std::string* error = nullptr) const;
};

enum class SelectionMode : std::uint8_t { Replace, Add, Toggle, Remove };

struct SceneSearchOptions {
    bool caseSensitive = false;
    bool includeAncestors = true;
    bool searchType = true;
    bool searchProperties = true;
};

class SceneModel final {
public:
    SceneModel();

    SceneNodeId root_id() const noexcept;
    SceneNode* root() noexcept;
    const SceneNode* root() const noexcept;
    SceneNode* node(SceneNodeId id) noexcept;
    const SceneNode* node(SceneNodeId id) const noexcept;
    bool contains(SceneNodeId id) const noexcept;
    std::size_t size() const noexcept;

    SceneNodeId add_node(SceneNodeId parent, std::string name, std::string type = "Node");
    bool insert_node(SceneNode node, SceneNodeId parent, std::size_t index = SceneAppendIndex,
                     std::string* error = nullptr);
    bool remove_node(SceneNodeId id, std::string* error = nullptr);
    bool rename_node(SceneNodeId id, std::string name, std::string* error = nullptr);
    bool set_node_type(SceneNodeId id, std::string type, std::string* error = nullptr);
    bool move_node(SceneNodeId id, SceneNodeId newParent, std::size_t index = SceneAppendIndex,
                   std::string* error = nullptr);
    bool reorder_node(SceneNodeId id, std::size_t index, std::string* error = nullptr);

    bool select(SceneNodeId id, SelectionMode mode = SelectionMode::Replace);
    bool set_selection(const std::vector<SceneNodeId>& ids);
    void clear_selection() noexcept;
    const std::vector<SceneNodeId>& selection() const noexcept;
    bool is_selected(SceneNodeId id) const noexcept;

    std::vector<SceneNodeId> flatten() const;
    std::vector<SceneNodeId> search(std::string_view query,
                                    const SceneSearchOptions& options = {}) const;

    bool add_inspector_section(SceneNodeId nodeId, InspectorSection section,
                               std::string* error = nullptr);
    bool remove_inspector_section(SceneNodeId nodeId, std::string_view sectionId,
                                  std::string* error = nullptr);
    InspectorSection* inspector_section(SceneNodeId nodeId, std::string_view sectionId) noexcept;
    const InspectorSection* inspector_section(SceneNodeId nodeId,
                                              std::string_view sectionId) const noexcept;
    bool add_inspector_field(SceneNodeId nodeId, std::string_view sectionId,
                             InspectorField field, std::string* error = nullptr);
    bool remove_inspector_field(SceneNodeId nodeId, std::string_view sectionId,
                                std::string_view fieldId, std::string* error = nullptr);
    InspectorField* inspector_field(SceneNodeId nodeId, std::string_view sectionId,
                                    std::string_view fieldId) noexcept;
    const InspectorField* inspector_field(SceneNodeId nodeId, std::string_view sectionId,
                                          std::string_view fieldId) const noexcept;
    bool set_inspector_value(SceneNodeId nodeId, std::string_view sectionId,
                             std::string_view fieldId, ScenePropertyValue value,
                             std::string* error = nullptr);

    bool valid(std::string* error = nullptr) const;
    std::string serialize_xml(bool pretty = true) const;
    bool deserialize_xml(std::string_view xml, std::string* error = nullptr);

private:
    std::map<SceneNodeId, SceneNode> nodes_;
    std::vector<SceneNodeId> selection_;
    SceneNodeId rootId_ = InvalidSceneNodeId;
    SceneNodeId nextId_ = 1;

    bool is_descendant(SceneNodeId candidate, SceneNodeId ancestor) const noexcept;
    void collect(SceneNodeId id, std::vector<SceneNodeId>& result) const;
    void remove_from_selection(SceneNodeId id) noexcept;
};

} // namespace shinkou::uikit

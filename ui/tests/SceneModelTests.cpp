#include "shinkou/uikit/SceneModel.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace shinkou::uikit;

namespace {

InspectorField number_field() {
    InspectorField field;
    field.id = "opacity";
    field.label = "Opacity";
    field.type = ScenePropertyType::Number;
    field.value = 1.0;
    field.hasRange = true;
    field.minimum = 0.0;
    field.maximum = 1.0;
    field.step = 0.05;
    return field;
}

} // namespace

int main() {
    SceneModel model;
    const SceneNodeId root = model.root_id();
    const SceneNodeId camera = model.add_node(root, "Camera", "Camera3D");
    const SceneNodeId lights = model.add_node(root, "Lights", "Node");
    const SceneNodeId key = model.add_node(lights, "Key Light", "DirectionalLight");
    const SceneNodeId fill = model.add_node(lights, "Fill Light", "PointLight");
    assert(camera != InvalidSceneNodeId && lights != InvalidSceneNodeId);
    assert(key != InvalidSceneNodeId && fill != InvalidSceneNodeId);
    assert(model.root()->children == std::vector<SceneNodeId>({camera, lights}));
    assert(model.node(lights)->children == std::vector<SceneNodeId>({key, fill}));

    assert(model.rename_node(camera, "Main Camera"));
    assert(model.set_node_type(camera, "PerspectiveCamera"));
    assert(model.reorder_node(camera, 1));
    assert(model.root()->children == std::vector<SceneNodeId>({lights, camera}));
    assert(model.move_node(fill, root, 0));
    assert(model.root()->children == std::vector<SceneNodeId>({fill, lights, camera}));
    assert(model.node(lights)->children == std::vector<SceneNodeId>({key}));
    assert(!model.move_node(lights, key)); // would introduce a cycle

    assert(model.select(lights));
    assert(model.select(key, SelectionMode::Add));
    assert(model.is_selected(lights) && model.is_selected(key));
    assert(model.select(lights, SelectionMode::Toggle));
    assert(!model.is_selected(lights) && model.is_selected(key));
    assert(model.set_selection({camera, key, camera}));
    assert(model.selection() == std::vector<SceneNodeId>({camera, key}));
    assert(!model.set_selection({999999}));

    model.node(lights)->properties["layer"] = {ScenePropertyType::Integer, std::int64_t{2}};
    model.node(key)->properties["color"] = {ScenePropertyType::Color, SceneColor{1, 0.5f, 0.25f, 1}};
    auto filtered = model.search("light");
    assert(filtered == std::vector<SceneNodeId>({root, fill, lights, key}));
    filtered = model.search("perspective", SceneSearchOptions{false, false, true, false});
    assert(filtered == std::vector<SceneNodeId>({camera}));
    filtered = model.search("2", SceneSearchOptions{false, false, false, true});
    assert(filtered == std::vector<SceneNodeId>({lights}));

    InspectorSection transform;
    transform.id = "transform";
    transform.title = "Transform";
    transform.fields.push_back(number_field());
    InspectorField enumField;
    enumField.id = "mode";
    enumField.label = "Mode";
    enumField.type = ScenePropertyType::Enum;
    enumField.enumOptions = {"Local", "World"};
    enumField.value = std::string("Local");
    transform.fields.push_back(enumField);
    assert(model.add_inspector_section(camera, transform));
    assert(!model.add_inspector_section(camera, transform));
    assert(model.set_inspector_value(camera, "transform", "opacity", 0.5));
    std::string error;
    assert(!model.set_inspector_value(camera, "transform", "opacity", 2.0, &error));
    assert(!error.empty());
    assert(model.set_inspector_value(camera, "transform", "mode", std::string("World")));
    assert(!model.set_inspector_value(camera, "transform", "mode", std::string("Screen")));
    assert(model.inspector_field(camera, "transform", "mode")->value == ScenePropertyValue{std::string("World")});

    InspectorField invalid = number_field();
    invalid.value = std::string("not a number");
    assert(!invalid.valid(&error));
    InspectorField required;
    required.id = "title";
    required.label = "Title";
    required.type = ScenePropertyType::String;
    required.required = true;
    required.value = std::string{};
    assert(!required.valid(&error));

    assert(model.valid(&error));
    const std::string xml = model.serialize_xml();
    assert(!xml.empty());
    assert(xml.find("shinkou-uikit-scene") != std::string::npos);
    assert(xml.find("PerspectiveCamera") != std::string::npos);
    assert(xml.find("opacity") != std::string::npos);

    SceneModel restored;
    assert(restored.deserialize_xml(xml, &error));
    assert(restored.valid(&error));
    assert(restored.size() == model.size());
    assert(restored.root()->children == model.root()->children);
    assert(restored.node(lights)->children == model.node(lights)->children);
    assert(restored.node(lights)->properties.at("layer").value == model.node(lights)->properties.at("layer").value);
    assert(std::get<double>(restored.inspector_field(camera, "transform", "opacity")->value) == 0.5);
    assert(std::get<std::string>(restored.inspector_field(camera, "transform", "mode")->value) == "World");
    assert(restored.selection().empty());

    const std::string beforeInvalidLoad = restored.serialize_xml();
    assert(!restored.deserialize_xml("<scene-model schema=\"bad\" version=\"1\"/>", &error));
    assert(restored.serialize_xml() == beforeInvalidLoad);
    assert(model.remove_node(lights));
    assert(!model.contains(key) && !model.contains(lights));
    assert(model.valid(&error));

    std::cout << "SceneModel hierarchy, multi-selection, search, inspector validation and XML round-trip passed\n";
    return 0;
}

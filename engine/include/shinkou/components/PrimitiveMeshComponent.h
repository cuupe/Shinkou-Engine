#pragma once

#include "shinkou/GameObject.h"

namespace shinkou::components {

// Scene data only: GPU resources belong to the renderer and never enter undo
// snapshots or scene documents.
class PrimitiveMeshComponent final : public Component {
    std::string shape_{"Cube"};
    math::Vec3 color_{0.32f, 0.56f, 0.82f};

  public:
    std::string_view type_name() const noexcept override {
        return "PrimitiveMesh";
    }
    const std::string &shape() const noexcept {
        return shape_;
    }
    math::Vec3 color() const noexcept {
        return color_;
    }
    bool set_shape(std::string shape) {
        if (shape != "Cube" && shape != "Sphere" && shape != "Plane")
            return false;
        shape_ = std::move(shape);
        return true;
    }
    bool set_color(math::Vec3 color) noexcept {
        if (!math::IsFinite(color.x) || !math::IsFinite(color.y) || !math::IsFinite(color.z) ||
            color.x < 0 || color.x > 1 || color.y < 0 || color.y > 1 || color.z < 0 || color.z > 1)
            return false;
        color_ = color;
        return true;
    }

  protected:
    void define_properties(PropertyBuilder &builder) override {
        builder.add_custom(
            "shape", PropertyType::String, [this] { return PropertyValue{shape_}; },
            [this](const PropertyValue &v) {
                const auto *p = std::get_if<std::string>(&v);
                return p && set_shape(*p);
            },
            PropertyFlags::Serialized);
        builder.add_custom(
            "color", PropertyType::Vec3, [this] { return PropertyValue{color_}; },
            [this](const PropertyValue &v) {
                const auto *p = std::get_if<math::Vec3>(&v);
                return p && set_color(*p);
            },
            PropertyFlags::Serialized, 0, 1, 0.01);
    }
};
} // namespace shinkou::components

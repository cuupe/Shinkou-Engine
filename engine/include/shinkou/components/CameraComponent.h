#pragma once
#include "shinkou/GameObject.h"

namespace shinkou::components {
// Serializable scene camera. Editor navigation owns a different camera, so
// looking around the scene never edits a game's camera or its Transform.
class CameraComponent final : public Component {
  public:
    float fieldOfView{60}, nearPlane{.05f}, farPlane{1000}, orthographicSize{10};
    bool orthographic{false}, active{true};
    std::int64_t priority{0};
    std::string_view type_name() const noexcept override {
        return "Camera";
    }

  protected:
    void define_properties(PropertyBuilder &builder) override {
        const auto number = [&](const char *name, float &field, float low, float high) {
            auto *target = &field;
            builder.add_custom(
                name, PropertyType::Number, [target] { return PropertyValue{double(*target)}; },
                [target, low, high](const PropertyValue &value) {
                    const auto *n = std::get_if<double>(&value);
                    if (!n || !std::isfinite(*n) || *n < low || *n > high)
                        return false;
                    *target = static_cast<float>(*n);
                    return true;
                },
                PropertyFlags::Serialized, low, high, .1);
        };
        number("fieldOfView", fieldOfView, 5, 150);
        number("nearPlane", nearPlane, .001f, 10);
        number("farPlane", farPlane, 10.001f, 100000);
        number("orthographicSize", orthographicSize, .01f, 100000);
        builder.add("orthographic", &orthographic);
        builder.add("active", &active);
        builder.add("priority", &priority);
    }
};
} // namespace shinkou::components

#include "shinkou/reflection/EngineReflection.h"

namespace shinkou::reflection {

bool register_engine_types(TypeRegistry& registry, std::string* error) {
    std::vector<TypeDescriptor> types;
    types.reserve(6);
    types.push_back(std::move(TypeBuilder<Component>("shinkou.Component", TypeKind::Object, TypeFlags::Abstract).take()));
    types.push_back(std::move(TypeBuilder<components::TransformComponent>("shinkou.components.Transform", TypeKind::Object,
        TypeFlags::Component | TypeFlags::Serializable).base<Component>()
        .field("local", &components::TransformComponent::local).take()));
    types.push_back(std::move(TypeBuilder<components::TagComponent>("shinkou.components.Tag", TypeKind::Object,
        TypeFlags::Component | TypeFlags::Serializable).base<Component>().take()));
    types.push_back(std::move(TypeBuilder<components::LifetimeComponent>("shinkou.components.Lifetime", TypeKind::Object,
        TypeFlags::Component | TypeFlags::Serializable).base<Component>()
        .field("remaining", &components::LifetimeComponent::remaining)
        .field("destroyWhenExpired", &components::LifetimeComponent::destroyWhenExpired).take()));
    types.push_back(std::move(TypeBuilder<render::LightType>("shinkou.render.LightType", TypeKind::Enum)
        .enum_value("Directional", static_cast<std::int64_t>(render::LightType::Directional))
        .enum_value("Point", static_cast<std::int64_t>(render::LightType::Point))
        .enum_value("Spot", static_cast<std::int64_t>(render::LightType::Spot)).take()));
    types.push_back(std::move(TypeBuilder<render::TransformComponent>("shinkou.render.TransformComponent")
        .field("local", &render::TransformComponent::local).field("visible", &render::TransformComponent::visible).take()));
    return registry.register_module("shinkou.engine", 1, std::move(types), error);
}

} // namespace shinkou::reflection

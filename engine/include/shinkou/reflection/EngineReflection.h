#pragma once

#include "shinkou/GameObject.h"
#include "shinkou/reflection/Reflection.h"
#include "shinkou/render/RenderScene.h"

namespace shinkou::reflection {

SHINKOU_REFLECT_TYPE(::shinkou::Component, "shinkou.Component");
SHINKOU_REFLECT_TYPE(::shinkou::components::TransformComponent, "shinkou.components.Transform");
SHINKOU_REFLECT_TYPE(::shinkou::components::TagComponent, "shinkou.components.Tag");
SHINKOU_REFLECT_TYPE(::shinkou::components::LifetimeComponent, "shinkou.components.Lifetime");
SHINKOU_REFLECT_TYPE(::shinkou::render::LightType, "shinkou.render.LightType");
SHINKOU_REFLECT_TYPE(::shinkou::render::TransformComponent, "shinkou.render.TransformComponent");

bool register_engine_types(TypeRegistry& registry, std::string* error = nullptr);

} // namespace shinkou::reflection

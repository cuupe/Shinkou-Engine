#pragma once

#include "shinkou/Math.h"

namespace shinkou::ecs {

// Runtime world matrix, stored in chunk memory next to the other POD data.
//
// Entities backed by a GameObject receive one automatically when they are
// promoted to ECS storage, and `World` refreshes it every frame in parallel by
// hierarchy depth.  Render extraction reads this array directly instead of
// walking the GameObject tree.
struct LocalToWorld {
    math::Mat4 matrix{math::Mat4::Identity()};
};

} // namespace shinkou::ecs

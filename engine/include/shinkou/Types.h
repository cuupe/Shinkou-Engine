#pragma once

#include "shinkou/core/Handle.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace shinkou {
using EntityId = std::uint32_t;
using Generation = std::uint32_t;
using ObjectId = std::uint64_t;
using ComponentId = std::uint64_t;
using FrameIndex = std::uint64_t;
using Seconds = float;

enum class LifecycleState { Constructing, Alive, Inactive, DestroyRequested, Destroyed };

// Selects where a GameObject's Component instances are stored.  Regular is
// intentionally the default: it keeps the object-oriented scene path cheap
// for editor and gameplay objects while allowing data-heavy objects to opt in
// to EnTT storage at creation time.
enum class ObjectStorage : std::uint8_t { Regular, Ecs };

struct EntityTag;
using Entity = core::GenerationalHandle<EntityTag, EntityId, Generation, false>;
using EntityHash = core::HandleHash<Entity>;
} // namespace shinkou

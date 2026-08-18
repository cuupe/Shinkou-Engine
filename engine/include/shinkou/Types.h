#pragma once

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

enum class LifecycleState {
    Constructing,
    Alive,
    Inactive,
    DestroyRequested,
    Destroyed
};

struct Entity {
    EntityId id{0};
    Generation generation{0};

    explicit operator bool() const noexcept { return id != 0; }
    friend bool operator==(Entity a, Entity b) noexcept {
        return a.id == b.id && a.generation == b.generation;
    }
    friend bool operator!=(Entity a, Entity b) noexcept { return !(a == b); }
};

struct EntityHash {
    std::size_t operator()(Entity value) const noexcept {
        const auto id = static_cast<std::size_t>(value.id);
        const auto generation = static_cast<std::size_t>(value.generation);
        return id ^ (generation + static_cast<std::size_t>(0x9e3779b9u) + (id << 6u) + (id >> 2u));
    }
};
}

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <typeinfo>

namespace shinkou::ecs {

// DOTS parity: a component type is a dense index plus a small static
// descriptor.  Everything the chunk store needs to place, construct, copy and
// destroy a component type is available without RTTI at runtime.
using TypeId = std::uint32_t;
inline constexpr TypeId kInvalidTypeId = 0;

// Entity chunks are fixed 16 KiB blocks, matching Unity's Entities default.
// A chunk owns the entity array plus one tightly packed array per component
// type (struct-of-arrays), so iterating one component is a linear scan and the
// arrays of the same archetype are adjacent in memory.
inline constexpr std::uint32_t kChunkSize = 16u * 1024u;

struct ComponentTypeInfo {
    TypeId id{kInvalidTypeId};
    const char *name{"<unregistered>"};
    std::uint32_t size{0};
    std::uint32_t alignment{1};
    // Tag components occupy no chunk storage; they only participate in the
    // archetype signature.
    bool tag{false};
    bool triviallyDestructible{false};
    void (*construct)(void *destination, std::uint32_t count){nullptr};
    void (*destruct)(void *destination, std::uint32_t count){nullptr};
    void (*copy)(void *destination, const void *source, std::uint32_t count){nullptr};
};

namespace detail {

std::atomic<std::uint32_t> &next_type_id() noexcept;
void register_type_info(const ComponentTypeInfo *info) noexcept;
const ComponentTypeInfo *find_type_info(TypeId id) noexcept;

template <class T> void construct_impl(void *destination, std::uint32_t count) {
    if constexpr (!std::is_trivially_default_constructible_v<T>) {
        auto *values = static_cast<T *>(destination);
        for (std::uint32_t index = 0; index < count; ++index)
            ::new (values + index) T();
    }
}

template <class T> void destruct_impl(void *destination, std::uint32_t count) {
    if constexpr (!std::is_trivially_destructible_v<T>) {
        auto *values = static_cast<T *>(destination);
        for (std::uint32_t index = 0; index < count; ++index)
            values[index].~T();
    }
}

template <class T> void copy_impl(void *destination, const void *source, std::uint32_t count) {
    if constexpr (std::is_trivially_copyable_v<T>) {
        std::memcpy(destination, source, sizeof(T) * static_cast<std::size_t>(count));
    } else {
        auto *target = static_cast<T *>(destination);
        const auto *values = static_cast<const T *>(source);
        for (std::uint32_t index = 0; index < count; ++index)
            ::new (target + index) T(values[index]);
    }
}

template <class T> struct TypeInfoHolder {
    ComponentTypeInfo info;

    TypeInfoHolder() {
        info.id = next_type_id().fetch_add(1, std::memory_order_relaxed);
        info.name = typeid(T).name();
        info.size = static_cast<std::uint32_t>(sizeof(T));
        info.alignment = static_cast<std::uint32_t>(alignof(T));
        info.tag = std::is_empty_v<T>;
        info.triviallyDestructible = std::is_trivially_destructible_v<T>;
        info.construct = &construct_impl<T>;
        info.destruct = &destruct_impl<T>;
        info.copy = &copy_impl<T>;
        register_type_info(&info);
    }
};

} // namespace detail

// Registers T on first use.  The returned descriptor lives for the process
// lifetime, so component arrays can cache it.
template <class T> const ComponentTypeInfo &component_type_info() {
    static const detail::TypeInfoHolder<T> holder;
    return holder.info;
}

template <class T> TypeId type_id() {
    return component_type_info<T>().id;
}

inline const ComponentTypeInfo *type_info(TypeId id) noexcept {
    return detail::find_type_info(id);
}

} // namespace shinkou::ecs

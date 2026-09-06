#pragma once

#include "shinkou/Math.h"
#include "shinkou/Types.h"
#include <cassert>
#include <cmath>
#include <functional>
#include <memory>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace shinkou {
class GameObject;
class World;
class Component;

using PropertyValue = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double,
                                   std::string, math::Vec2, math::Vec3, math::Quat, math::Transform>;

enum class PropertyType {
    Unknown,
    Bool,
    Integer,
    UnsignedInteger,
    Number,
    String,
    Vec2,
    Vec3,
    Quaternion,
    Transform
};

enum class PropertyFlags : std::uint32_t {
    None = 0,
    ReadOnly = 1u << 0u,
    Hidden = 1u << 1u,
    RuntimeOnly = 1u << 2u,
    Serialized = 1u << 3u
};

inline PropertyFlags operator|(PropertyFlags lhs, PropertyFlags rhs) noexcept {
    return static_cast<PropertyFlags>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}
inline bool has_flag(PropertyFlags value, PropertyFlags flag) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

struct PropertyDescriptor {
    std::string name;
    std::string displayName;
    PropertyType type{PropertyType::Unknown};
    PropertyFlags flags{PropertyFlags::None};
    double minimum{0.0};
    double maximum{0.0};
    double step{0.0};
    std::function<PropertyValue()> get;
    std::function<bool(const PropertyValue&)> set;

    bool editable() const noexcept { return !has_flag(flags, PropertyFlags::ReadOnly) && static_cast<bool>(set); }
};

template<class T>
struct property_traits { static constexpr PropertyType type = PropertyType::Unknown; };
template<> struct property_traits<bool> { static constexpr PropertyType type = PropertyType::Bool; };
template<> struct property_traits<std::int32_t> { static constexpr PropertyType type = PropertyType::Integer; };
template<> struct property_traits<std::int64_t> { static constexpr PropertyType type = PropertyType::Integer; };
template<> struct property_traits<std::uint32_t> { static constexpr PropertyType type = PropertyType::UnsignedInteger; };
template<> struct property_traits<std::uint64_t> { static constexpr PropertyType type = PropertyType::UnsignedInteger; };
template<> struct property_traits<float> { static constexpr PropertyType type = PropertyType::Number; };
template<> struct property_traits<double> { static constexpr PropertyType type = PropertyType::Number; };
template<> struct property_traits<std::string> { static constexpr PropertyType type = PropertyType::String; };
template<> struct property_traits<math::Vec2> { static constexpr PropertyType type = PropertyType::Vec2; };
template<> struct property_traits<math::Vec3> { static constexpr PropertyType type = PropertyType::Vec3; };
template<> struct property_traits<math::Quat> { static constexpr PropertyType type = PropertyType::Quaternion; };
template<> struct property_traits<math::Transform> { static constexpr PropertyType type = PropertyType::Transform; };

template<class T>
PropertyValue make_property_value(const T& value) {
    if constexpr (std::is_same_v<T, bool>) return value;
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) return static_cast<std::int64_t>(value);
    else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) return static_cast<std::uint64_t>(value);
    else if constexpr (std::is_floating_point_v<T>) return static_cast<double>(value);
    else return value;
}

template<class T>
bool assign_property_value(T& target, const PropertyValue& value) {
    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::string> ||
                  std::is_same_v<T, math::Vec2> || std::is_same_v<T, math::Vec3> ||
                  std::is_same_v<T, math::Quat> || std::is_same_v<T, math::Transform>) {
        if (const auto* exact = std::get_if<T>(&value)) {
            target = *exact;
            return true;
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        if (const auto* number = std::get_if<double>(&value)) {
            target = static_cast<T>(*number);
            return true;
        }
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        if (const auto* number = std::get_if<std::int64_t>(&value)) {
            target = static_cast<T>(*number);
            return true;
        }
    } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
        if (const auto* number = std::get_if<std::uint64_t>(&value)) {
            target = static_cast<T>(*number);
            return true;
        }
    }
    return false;
}

class PropertyBuilder {
    std::vector<PropertyDescriptor> properties_;

public:
    template<class T>
    void add(std::string name, T* value, PropertyFlags flags = PropertyFlags::Serialized,
             double minimum = 0.0, double maximum = 0.0, double step = 0.0,
             std::string displayName = {}) {
        if (!value) return;
        PropertyDescriptor descriptor;
        descriptor.name = std::move(name);
        descriptor.displayName = displayName.empty() ? descriptor.name : std::move(displayName);
        descriptor.type = property_traits<T>::type;
        descriptor.flags = flags;
        descriptor.minimum = minimum;
        descriptor.maximum = maximum;
        descriptor.step = step;
        descriptor.get = [value] { return make_property_value(*value); };
        if (!has_flag(flags, PropertyFlags::ReadOnly)) {
            descriptor.set = [value, minimum, maximum](const PropertyValue& next) {
                if (maximum > minimum) {
                    double number = 0.0;
                    if (const auto* floating = std::get_if<double>(&next)) number = *floating;
                    else if (const auto* integer = std::get_if<std::int64_t>(&next)) number = static_cast<double>(*integer);
                    else if (const auto* unsignedInteger = std::get_if<std::uint64_t>(&next)) number = static_cast<double>(*unsignedInteger);
                    else return false;
                    if (!std::isfinite(number) || number < minimum || number > maximum) return false;
                }
                return assign_property_value(*value, next);
            };
        }
        properties_.push_back(std::move(descriptor));
    }

    void add_custom(std::string name, PropertyType type, std::function<PropertyValue()> getter,
                    std::function<bool(const PropertyValue&)> setter,
                    PropertyFlags flags = PropertyFlags::Serialized,
                    double minimum = 0.0, double maximum = 0.0, double step = 0.0,
                    std::string displayName = {}) {
        PropertyDescriptor descriptor;
        descriptor.name = std::move(name);
        descriptor.displayName = displayName.empty() ? descriptor.name : std::move(displayName);
        descriptor.type = type;
        descriptor.flags = flags;
        descriptor.minimum = minimum;
        descriptor.maximum = maximum;
        descriptor.step = step;
        descriptor.get = std::move(getter);
        descriptor.set = has_flag(flags, PropertyFlags::ReadOnly) ? nullptr : std::move(setter);
        properties_.push_back(std::move(descriptor));
    }

    void add_read_only(std::string name, PropertyType type, std::function<PropertyValue()> getter,
                       PropertyFlags flags = PropertyFlags::RuntimeOnly, std::string displayName = {}) {
        PropertyDescriptor descriptor;
        descriptor.name = std::move(name);
        descriptor.displayName = displayName.empty() ? descriptor.name : std::move(displayName);
        descriptor.type = type;
        descriptor.flags = flags | PropertyFlags::ReadOnly;
        descriptor.get = std::move(getter);
        properties_.push_back(std::move(descriptor));
    }

    std::vector<PropertyDescriptor> take() { return std::move(properties_); }
};

struct GameObjectHandle {
    World* world{nullptr};
    ObjectId id{0};
    GameObject* get() const noexcept;
    bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }
};

struct ComponentHandle {
    World* world{nullptr};
    ComponentId id{0};
    Component* get() const noexcept;
    bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }
};

class Component {
    GameObject* owner_{nullptr};
    ComponentId id_{0};
    std::string registeredName_;
    bool enabled_{true};
    LifecycleState state_{LifecycleState::Constructing};

    friend class GameObject;
    friend class World;

    void notify_active_state(bool active) noexcept;

public:
    virtual ~Component() = default;

    GameObject& game_object() noexcept;
    const GameObject& game_object() const noexcept;
    GameObject* try_game_object() noexcept { return owner_; }
    const GameObject* try_game_object() const noexcept { return owner_; }

    bool enabled() const noexcept { return enabled_; }
    void set_enabled(bool enabled) noexcept;
    ComponentId id() const noexcept { return id_; }
    ComponentHandle handle() noexcept;
    LifecycleState lifecycle_state() const noexcept { return state_; }
    virtual std::string_view type_name() const noexcept { return "Component"; }
    std::string_view registered_type_name() const noexcept {
        return registeredName_.empty() ? type_name() : std::string_view(registeredName_);
    }
    std::vector<PropertyDescriptor> properties();
    bool set_property(std::string_view name, const PropertyValue& value);

protected:
    virtual void on_attach() {}
    virtual void on_create() {}
    virtual void on_detach() {}
    virtual void on_enable() {}
    virtual void on_disable() {}
    virtual void on_destroy_requested() {}
    virtual void on_destroy() {}
    virtual void on_update(Seconds) {}
    virtual void define_properties(PropertyBuilder&) {}

private:
    void notify_created() noexcept;
    void notify_destroy_requested() noexcept;
    void notify_destroyed() noexcept;
};

namespace components {
class TransformComponent final : public Component {
    math::Mat4 worldMatrix_{math::Mat4::Identity()};
    bool dirty_{true};

    friend class ::shinkou::GameObject;
    void update_world(const math::Mat4& parentWorld) noexcept;

public:
    math::Transform local{};

    const math::Mat4& world_matrix() const noexcept { return worldMatrix_; }
    std::string_view type_name() const noexcept override { return "Transform"; }
    void mark_dirty() noexcept { dirty_ = true; }
    void set_position(math::Vec3 position) noexcept { local.position = position; mark_dirty(); }
    void set_rotation(math::Quat rotation) noexcept { local.rotation = rotation; mark_dirty(); }
    void set_scale(math::Vec3 scale) noexcept { local.scale = scale; mark_dirty(); }

protected:
    void define_properties(PropertyBuilder& builder) override {
        builder.add_custom("position", PropertyType::Vec3,
            [this] { return PropertyValue{local.position}; },
            [this](const PropertyValue& value) {
                const auto* next = std::get_if<math::Vec3>(&value);
                if (!next) return false;
                set_position(*next);
                return true;
            });
        builder.add_custom("rotation", PropertyType::Quaternion,
            [this] { return PropertyValue{local.rotation}; },
            [this](const PropertyValue& value) {
                const auto* next = std::get_if<math::Quat>(&value);
                if (!next) return false;
                set_rotation(*next);
                return true;
            });
        builder.add_custom("scale", PropertyType::Vec3,
            [this] { return PropertyValue{local.scale}; },
            [this](const PropertyValue& value) {
                const auto* next = std::get_if<math::Vec3>(&value);
                if (!next) return false;
                set_scale(*next);
                return true;
            });
    }
};

class TagComponent final : public Component {
    std::vector<std::string> tags_;

public:
    std::string_view type_name() const noexcept override { return "Tag"; }
    bool has(std::string_view tag) const noexcept;
    void add(std::string tag);
    bool remove(std::string_view tag) noexcept;
    const std::vector<std::string>& tags() const noexcept { return tags_; }
};

class LifetimeComponent final : public Component {
public:
    Seconds remaining{-1.0f};
    bool destroyWhenExpired{true};
    std::string_view type_name() const noexcept override { return "Lifetime"; }

protected:
    void define_properties(PropertyBuilder& builder) override {
        builder.add("remaining", &remaining, PropertyFlags::Serialized, -1.0, 3600.0, 0.01);
        builder.add("destroyWhenExpired", &destroyWhenExpired);
    }
    void on_update(Seconds dt) override;
};
}

class GameObject {
public:
    struct LifecycleCallbacks {
        std::function<void(GameObject&)> created;
        std::function<void(GameObject&)> destroyRequested;
        std::function<void(GameObject&)> destroyed;
    };

private:
    World* world_{nullptr};
    ObjectId id_{0};
    std::string name_;
    GameObject* parent_{nullptr};
    bool activeSelf_{true};
    bool activeInHierarchy_{false};
    bool destroyRequested_{false};
    LifecycleState state_{LifecycleState::Constructing};
    LifecycleCallbacks lifecycleCallbacks_{};
    std::vector<std::unique_ptr<Component>> components_;
    std::unordered_map<std::type_index, Component*> componentLookup_;
    std::vector<std::unique_ptr<GameObject>> children_;
    Entity ecsEntity_{};

    friend class World;
    friend class Component;

    GameObject(World& world, ObjectId id, std::string name);
    void refresh_active_state(bool parentActive) noexcept;
    void update_recursive(Seconds dt);
    void dispose() noexcept;
    void request_destroy_recursive() noexcept;
    void update_world_transform() noexcept;
    void attach_component(Component& component, std::string_view registeredTypeName);
    void remove_component(Component& component) noexcept;
    void initialize() noexcept;

public:
    GameObject(const GameObject&) = delete;
    GameObject& operator=(const GameObject&) = delete;
    ~GameObject() = default;

    ObjectId id() const noexcept { return id_; }
    GameObjectHandle handle() noexcept { return {world_, id_}; }
    std::string_view name() const noexcept { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }
    LifecycleState lifecycle_state() const noexcept { return state_; }
    void set_lifecycle_callbacks(LifecycleCallbacks callbacks) { lifecycleCallbacks_ = std::move(callbacks); }
    std::vector<PropertyDescriptor> properties();
    bool set_property(std::string_view name, const PropertyValue& value);

    World& world() noexcept { return *world_; }
    const World& world() const noexcept { return *world_; }
    GameObject* parent() noexcept { return parent_; }
    const GameObject* parent() const noexcept { return parent_; }
    bool active_self() const noexcept { return activeSelf_; }
    bool active_in_hierarchy() const noexcept { return activeInHierarchy_; }
    bool destroy_requested() const noexcept { return destroyRequested_; }
    void set_active(bool active) noexcept;
    void destroy() noexcept;
    Component* add_component(std::string_view registeredTypeName);
    Component* component_at(std::size_t index) noexcept;
    const Component* component_at(std::size_t index) const noexcept;
    std::size_t component_count() const noexcept;

    template<class Fn>
    void each_component(Fn&& fn) {
        for (auto& component : components_) if (component) fn(*component);
    }

    template<class Fn>
    void each_component(Fn&& fn) const {
        for (const auto& component : components_) if (component) fn(*component);
    }

    GameObject& create_child(std::string name = {});
    void set_parent(GameObject* parent);
    const std::vector<std::unique_ptr<GameObject>>& children() const noexcept { return children_; }

    template<class T, class... Args>
    T* add_component(Args&&... args) {
        static_assert(std::is_base_of_v<Component, T>, "T must derive from shinkou::Component");
        const auto key = std::type_index(typeid(T));
        if (componentLookup_.find(key) != componentLookup_.end()) return nullptr;
        auto component = std::make_unique<T>(std::forward<Args>(args)...);
        auto* result = component.get();
        components_.push_back(std::move(component));
        componentLookup_.emplace(key, result);
        attach_component(*result, result->type_name());
        return result;
    }

    template<class T, class... Args>
    T& get_or_add_component(Args&&... args) {
        if (auto* existing = get_component<T>()) return *existing;
        auto* created = add_component<T>(std::forward<Args>(args)...);
        assert(created);
        return *created;
    }

    template<class T>
    T* get_component() noexcept {
        static_assert(std::is_base_of_v<Component, T>, "T must derive from shinkou::Component");
        const auto it = componentLookup_.find(std::type_index(typeid(T)));
        return it == componentLookup_.end() ? nullptr : static_cast<T*>(it->second);
    }

    template<class T>
    const T* get_component() const noexcept {
        static_assert(std::is_base_of_v<Component, T>, "T must derive from shinkou::Component");
        const auto it = componentLookup_.find(std::type_index(typeid(T)));
        return it == componentLookup_.end() ? nullptr : static_cast<const T*>(it->second);
    }

    template<class T>
    bool has_component() const noexcept { return get_component<T>() != nullptr; }

    template<class T>
    bool remove_component() noexcept {
        const auto it = componentLookup_.find(std::type_index(typeid(T)));
        if (it == componentLookup_.end()) return false;
        remove_component(*it->second);
        return true;
    }

    bool has_ecs_entity() const noexcept { return static_cast<bool>(ecsEntity_); }
    Entity ecs_entity() const noexcept { return ecsEntity_; }
    Entity enable_ecs();
    void disable_ecs() noexcept;

    template<class T, class... Args>
    T& add_ecs_component(Args&&... args);

    template<class T>
    T* get_ecs_component() noexcept;

    template<class T>
    const T* get_ecs_component() const noexcept;

    template<class T>
    void remove_ecs_component() noexcept;
};
}

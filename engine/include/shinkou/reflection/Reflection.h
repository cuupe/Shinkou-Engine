#pragma once

#include "shinkou/Math.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace shinkou::reflection {

using TypeId = std::uint64_t;
using MemberId = std::uint64_t;
using CastFn = void* (*)(void*) noexcept;
using ConstCastFn = const void* (*)(const void*) noexcept;

constexpr TypeId fnv1a(std::string_view value) noexcept {
    TypeId hash = 14695981039346656037ull;
    for (const auto character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

template<class T>
struct TypeName { static constexpr std::string_view value{}; };

#define SHINKOU_REFLECT_TYPE(TYPE, NAME) \
    template<> struct TypeName<TYPE> { static constexpr std::string_view value{NAME}; }

SHINKOU_REFLECT_TYPE(bool, "bool");
SHINKOU_REFLECT_TYPE(std::int8_t, "int8");
SHINKOU_REFLECT_TYPE(std::int16_t, "int16");
SHINKOU_REFLECT_TYPE(std::int32_t, "int32");
SHINKOU_REFLECT_TYPE(std::int64_t, "int64");
SHINKOU_REFLECT_TYPE(std::uint8_t, "uint8");
SHINKOU_REFLECT_TYPE(std::uint16_t, "uint16");
SHINKOU_REFLECT_TYPE(std::uint32_t, "uint32");
SHINKOU_REFLECT_TYPE(std::uint64_t, "uint64");
SHINKOU_REFLECT_TYPE(float, "float");
SHINKOU_REFLECT_TYPE(double, "double");
SHINKOU_REFLECT_TYPE(void, "void");
SHINKOU_REFLECT_TYPE(std::string, "string");
SHINKOU_REFLECT_TYPE(math::Vec2, "shinkou.math.Vec2");
SHINKOU_REFLECT_TYPE(math::Vec3, "shinkou.math.Vec3");
SHINKOU_REFLECT_TYPE(math::Quat, "shinkou.math.Quat");
SHINKOU_REFLECT_TYPE(math::Transform, "shinkou.math.Transform");

template<class T>
struct remove_cvref { using type = std::remove_cv_t<std::remove_reference_t<T>>; };
template<class T>
using remove_cvref_t = typename remove_cvref<T>::type;

template<class T>
constexpr TypeId type_id() noexcept {
    using U = remove_cvref_t<T>;
    return TypeName<U>::value.empty() ? 0 : fnv1a(TypeName<U>::value);
}

enum class TypeKind : std::uint8_t { Object, Struct, Enum, Primitive, Opaque };
enum class TypeFlags : std::uint32_t {
    None = 0,
    Abstract = 1u << 0u,
    Serializable = 1u << 1u,
    Component = 1u << 2u,
};
inline TypeFlags operator|(TypeFlags lhs, TypeFlags rhs) noexcept {
    return static_cast<TypeFlags>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}
inline bool has_flag(TypeFlags value, TypeFlags flag) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

enum class FieldFlags : std::uint32_t {
    None = 0,
    ReadOnly = 1u << 0u,
    Transient = 1u << 1u,
    Hidden = 1u << 2u,
    EditorOnly = 1u << 3u,
    Required = 1u << 4u,
};
inline FieldFlags operator|(FieldFlags lhs, FieldFlags rhs) noexcept {
    return static_cast<FieldFlags>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}
inline bool has_flag(FieldFlags value, FieldFlags flag) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

enum class MethodFlags : std::uint32_t { None = 0, Const = 1u << 0u, Static = 1u << 1u, EditorOnly = 1u << 2u };
inline MethodFlags operator|(MethodFlags lhs, MethodFlags rhs) noexcept {
    return static_cast<MethodFlags>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}
inline bool has_flag(MethodFlags value, MethodFlags flag) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

struct Attribute { std::string key; std::string value; };

struct EnumValueDescriptor {
    std::string name;
    std::int64_t value{0};
    std::vector<Attribute> attributes;
};

struct ContainerDescriptor {
    TypeId elementType{0};
    TypeId keyType{0};
    bool associative{false};
    std::size_t (*size)(const void*) noexcept{nullptr};
    bool (*resize)(void*, std::size_t) noexcept{nullptr};
    const void* (*element)(const void*, std::size_t) noexcept{nullptr};
    void* (*mutableElement)(void*, std::size_t) noexcept{nullptr};
};

struct BaseDescriptor {
    TypeId type{0};
    CastFn cast{nullptr};
    ConstCastFn constCast{nullptr};
};

class TypeRegistry;

struct FieldDescriptor {
    std::string name;
    std::string displayName;
    TypeId type{0};
    TypeId ownerType{0};
    FieldFlags flags{FieldFlags::None};
    std::vector<Attribute> attributes;
    std::function<void*(void*)> address;
    std::function<const void*(const void*)> constAddress;

    void* address_of(void* object) const noexcept { return address ? address(object) : nullptr; }
    const void* address_of(const void* object) const noexcept { return constAddress ? constAddress(object) : nullptr; }
    void* address_of(const TypeRegistry& registry, TypeId derivedType, void* object) const noexcept;
    const void* address_of(const TypeRegistry& registry, TypeId derivedType, const void* object) const noexcept;
    bool writable() const noexcept { return !has_flag(flags, FieldFlags::ReadOnly) && static_cast<bool>(address); }
};

struct Argument {
    TypeId type{0};
    const void* data{nullptr};
};

enum class InvokeError : std::uint8_t {
    None,
    InvalidObject,
    InvalidResult,
    ArgumentCount,
    ArgumentType,
    MethodFailure,
};

struct InvokeResult {
    InvokeError error{InvokeError::None};
    explicit operator bool() const noexcept { return error == InvokeError::None; }
};

using InvokeFn = InvokeResult (*)(const void* object, const Argument* arguments, std::size_t argumentCount,
                                  void* result) noexcept;

struct MethodDescriptor {
    std::string name;
    std::string displayName;
    TypeId ownerType{0};
    TypeId returnType{0};
    MethodFlags flags{MethodFlags::None};
    std::vector<TypeId> argumentTypes;
    std::vector<Attribute> attributes;
    InvokeFn invoke{nullptr};

    InvokeResult call(const void* object, const Argument* arguments, std::size_t argumentCount,
                      void* result = nullptr) const noexcept {
        return invoke ? invoke(object, arguments, argumentCount, result) : InvokeResult{InvokeError::MethodFailure};
    }
    InvokeResult call_on(const TypeRegistry& registry, TypeId derivedType, const void* object,
                         const Argument* arguments, std::size_t argumentCount, void* result = nullptr) const noexcept;
};

using ConstructFn = bool (*)(void*) noexcept;
using DestroyFn = void (*)(void*) noexcept;
using CopyFn = bool (*)(void*, const void*) noexcept;
using MoveFn = bool (*)(void*, void*) noexcept;

struct TypeDescriptor {
    TypeId id{0};
    std::string name;
    std::string displayName;
    std::size_t size{0};
    std::size_t alignment{0};
    TypeKind kind{TypeKind::Opaque};
    TypeFlags flags{TypeFlags::None};
    TypeId baseType{0};
    TypeId underlyingType{0};
    CastFn castToBase{nullptr};
    ConstCastFn constCastToBase{nullptr};
    std::vector<BaseDescriptor> bases;
    ConstructFn construct{nullptr};
    DestroyFn destroy{nullptr};
    CopyFn copy{nullptr};
    MoveFn move{nullptr};
    std::vector<FieldDescriptor> fields;
    std::vector<MethodDescriptor> methods;
    std::vector<EnumValueDescriptor> enumValues;
    std::vector<Attribute> attributes;
    std::unique_ptr<ContainerDescriptor> container;
    std::unordered_map<MemberId, std::size_t> fieldIndex;
    std::unordered_map<MemberId, std::size_t> methodIndex;
    std::unordered_map<MemberId, std::size_t> enumIndex;
    std::unordered_map<std::int64_t, std::size_t> enumValueIndex;

    const FieldDescriptor* find_field(std::string_view fieldName) const noexcept;
    const MethodDescriptor* find_method(std::string_view methodName) const noexcept;
    const EnumValueDescriptor* find_enum_value(std::string_view valueName) const noexcept;
    const EnumValueDescriptor* find_enum_value(std::int64_t enumValue) const noexcept;
    void rebuild_indexes();
};

class ReflectedInstance {
    std::shared_ptr<const TypeDescriptor> owner_;
    const TypeDescriptor* descriptor_{nullptr};
    void* storage_{nullptr};

    ReflectedInstance(const TypeDescriptor* descriptor, void* storage) noexcept
        : descriptor_(descriptor), storage_(storage) {}
    ReflectedInstance(std::shared_ptr<const TypeDescriptor> owner, void* storage) noexcept
        : owner_(std::move(owner)), descriptor_(owner_.get()), storage_(storage) {}
    void release() noexcept;
    friend class TypeRegistry;

public:
    ReflectedInstance() = default;
    ~ReflectedInstance() { release(); }
    ReflectedInstance(const ReflectedInstance&) = delete;
    ReflectedInstance& operator=(const ReflectedInstance&) = delete;
    ReflectedInstance(ReflectedInstance&& other) noexcept
        : owner_(std::move(other.owner_)), descriptor_(other.descriptor_), storage_(other.storage_) {
        other.descriptor_ = nullptr;
        other.storage_ = nullptr;
    }
    ReflectedInstance& operator=(ReflectedInstance&& other) noexcept {
        if (this != &other) {
            release();
            owner_ = std::move(other.owner_);
            descriptor_ = other.descriptor_;
            storage_ = other.storage_;
            other.descriptor_ = nullptr;
            other.storage_ = nullptr;
        }
        return *this;
    }

    static ReflectedInstance create(const TypeDescriptor& descriptor) noexcept;
    const TypeDescriptor* type() const noexcept { return descriptor_; }
    void* data() noexcept { return storage_; }
    const void* data() const noexcept { return storage_; }
    explicit operator bool() const noexcept { return descriptor_ != nullptr && storage_ != nullptr; }
};

template<class T>
struct TypeOperations {
    static bool construct(void* memory) noexcept {
        if (!memory) return false;
        try { new (memory) T(); return true; } catch (...) { return false; }
    }
    static void destroy(void* memory) noexcept { if (memory) static_cast<T*>(memory)->~T(); }
    static bool copy(void* destination, const void* source) noexcept {
        if (!destination || !source) return false;
        try { new (destination) T(*static_cast<const T*>(source)); return true; } catch (...) { return false; }
    }
    static bool move(void* destination, void* source) noexcept {
        if (!destination || !source) return false;
        try { new (destination) T(std::move(*static_cast<T*>(source))); return true; } catch (...) { return false; }
    }
};

template<class T>
struct VectorContainerOperations {
    static std::size_t size(const void* value) noexcept {
        return value ? static_cast<const std::vector<T>*>(value)->size() : 0;
    }
    static bool resize(void* value, std::size_t count) noexcept {
        if (!value) return false;
        try { static_cast<std::vector<T>*>(value)->resize(count); return true; } catch (...) { return false; }
    }
    static const void* element(const void* value, std::size_t index) noexcept {
        const auto* vector = static_cast<const std::vector<T>*>(value);
        return vector && index < vector->size() ? &(*vector)[index] : nullptr;
    }
    static void* mutable_element(void* value, std::size_t index) noexcept {
        auto* vector = static_cast<std::vector<T>*>(value);
        return vector && index < vector->size() ? &(*vector)[index] : nullptr;
    }
};

template<class T>
struct MethodTraits;
template<class R, class C, class... Args>
struct MethodTraits<R (C::*)(Args...)> { using class_type = C; using return_type = R; using arguments = std::tuple<Args...>; static constexpr bool isConst = false; };
template<class R, class C, class... Args>
struct MethodTraits<R (C::*)(Args...) const> { using class_type = C; using return_type = R; using arguments = std::tuple<Args...>; static constexpr bool isConst = true; };
template<class R, class C, class... Args>
struct MethodTraits<R (C::*)(Args...) noexcept> : MethodTraits<R (C::*)(Args...)> {};
template<class R, class C, class... Args>
struct MethodTraits<R (C::*)(Args...) const noexcept> : MethodTraits<R (C::*)(Args...) const> {};

template<auto Method>
struct MethodInvoker {
    using Traits = MethodTraits<decltype(Method)>;
    using Class = typename Traits::class_type;
    using Return = typename Traits::return_type;
    using Arguments = typename Traits::arguments;

    template<class Parameter>
    static decltype(auto) argument_value(const Argument& argument) noexcept {
        using Value = remove_cvref_t<Parameter>;
        if constexpr (std::is_lvalue_reference_v<Parameter> && !std::is_const_v<std::remove_reference_t<Parameter>>) {
            return *const_cast<Value*>(static_cast<const Value*>(argument.data));
        } else if constexpr (std::is_rvalue_reference_v<Parameter>) {
            return std::move(*const_cast<Value*>(static_cast<const Value*>(argument.data)));
        } else {
            return *static_cast<const Value*>(argument.data);
        }
    }

    template<std::size_t... Indices>
    static InvokeResult call_impl(const void* object, const Argument* arguments, std::index_sequence<Indices...>, void* result) noexcept {
        try {
            if (!object) return {InvokeError::InvalidObject};
            constexpr auto count = sizeof...(Indices);
            if (count > 0 && !arguments) return {InvokeError::ArgumentType};
            const bool typesMatch = ((arguments[Indices].data != nullptr &&
                arguments[Indices].type == type_id<remove_cvref_t<std::tuple_element_t<Indices, Arguments>>>()) && ...);
            if (!typesMatch) return {InvokeError::ArgumentType};
            if constexpr (Traits::isConst) {
                const auto* instance = static_cast<const Class*>(object);
                if constexpr (std::is_void_v<Return>) {
                    std::invoke(Method, instance, argument_value<std::tuple_element_t<Indices, Arguments>>(arguments[Indices])...);
                } else {
                    if (!result) return {InvokeError::InvalidResult};
                    *static_cast<remove_cvref_t<Return>*>(result) = std::invoke(Method, instance,
                        argument_value<std::tuple_element_t<Indices, Arguments>>(arguments[Indices])...);
                }
            } else {
                auto* instance = const_cast<Class*>(static_cast<const Class*>(object));
                if constexpr (std::is_void_v<Return>) {
                    std::invoke(Method, instance, argument_value<std::tuple_element_t<Indices, Arguments>>(arguments[Indices])...);
                } else {
                    if (!result) return {InvokeError::InvalidResult};
                    *static_cast<remove_cvref_t<Return>*>(result) = std::invoke(Method, instance,
                        argument_value<std::tuple_element_t<Indices, Arguments>>(arguments[Indices])...);
                }
            }
            return {};
        } catch (...) {
            return {InvokeError::MethodFailure};
        }
    }

    static InvokeResult invoke(const void* object, const Argument* arguments, std::size_t argumentCount, void* result) noexcept {
        constexpr auto count = std::tuple_size_v<Arguments>;
        if (argumentCount != count) return {InvokeError::ArgumentCount};
        return call_impl(object, arguments, std::make_index_sequence<count>{}, result);
    }
};

template<class MethodDescriptorType, class Arguments, std::size_t... Indices>
void append_argument_types(MethodDescriptorType& method, std::index_sequence<Indices...>) {
    (method.argumentTypes.push_back(type_id<remove_cvref_t<std::tuple_element_t<Indices, Arguments>>>()), ...);
}

class TypeBuilderBase {
protected:
    TypeDescriptor descriptor_;
    explicit TypeBuilderBase(std::string name, TypeKind kind, TypeFlags flags) {
        descriptor_.id = fnv1a(name);
        descriptor_.name = std::move(name);
        descriptor_.displayName = descriptor_.name;
        descriptor_.kind = kind;
        descriptor_.flags = flags;
    }

    void add_attribute(std::vector<Attribute>& target, std::string key, std::string value) {
        target.push_back({std::move(key), std::move(value)});
    }
public:
    TypeBuilderBase(const TypeBuilderBase&) = delete;
    TypeBuilderBase& operator=(const TypeBuilderBase&) = delete;
};

template<class T>
class TypeBuilder final : public TypeBuilderBase {
public:
    explicit TypeBuilder(std::string name, TypeKind kind = TypeKind::Struct, TypeFlags flags = TypeFlags::None)
        : TypeBuilderBase(std::move(name), kind, flags) {
        descriptor_.size = sizeof(T);
        descriptor_.alignment = alignof(T);
        descriptor_.id = type_id<T>() != 0 ? type_id<T>() : fnv1a(descriptor_.name);
        if constexpr (std::is_enum_v<T>) descriptor_.underlyingType = type_id<std::underlying_type_t<T>>();
        descriptor_.construct = !has_flag(flags, TypeFlags::Abstract) && std::is_default_constructible_v<T>
            ? &TypeOperations<T>::construct : nullptr;
        descriptor_.destroy = &TypeOperations<T>::destroy;
        descriptor_.copy = std::is_copy_constructible_v<T> ? &TypeOperations<T>::copy : nullptr;
        descriptor_.move = std::is_move_constructible_v<T> ? &TypeOperations<T>::move : nullptr;
    }

    template<class Base>
    TypeBuilder& base() noexcept {
        const auto baseId = type_id<Base>();
        const auto cast = [](void* object) noexcept -> void* {
            return object ? static_cast<Base*>(static_cast<T*>(object)) : nullptr;
        };
        const auto constCast = [](const void* object) noexcept -> const void* {
            return object ? static_cast<const Base*>(static_cast<const T*>(object)) : nullptr;
        };
        if (descriptor_.bases.empty()) {
            descriptor_.baseType = baseId;
            descriptor_.castToBase = cast;
            descriptor_.constCastToBase = constCast;
        }
        descriptor_.bases.push_back({baseId, cast, constCast});
        return *this;
    }

    TypeBuilder& display_name(std::string value) { descriptor_.displayName = std::move(value); return *this; }
    TypeBuilder& attribute(std::string key, std::string value) {
        add_attribute(descriptor_.attributes, std::move(key), std::move(value)); return *this;
    }

    template<class M, class U = T, std::enable_if_t<std::is_class_v<U>, int> = 0>
    TypeBuilder& field(std::string name, M U::*member, FieldFlags flags = FieldFlags::None,
                       std::string displayName = {}) {
        FieldDescriptor field;
        field.name = std::move(name);
        field.displayName = displayName.empty() ? field.name : std::move(displayName);
        field.type = type_id<M>();
        field.ownerType = descriptor_.id;
        field.flags = flags;
        field.address = [member](void* object) -> void* {
            return object ? &(static_cast<U*>(object)->*member) : nullptr;
        };
        field.constAddress = [member](const void* object) -> const void* {
            return object ? &(static_cast<const U*>(object)->*member) : nullptr;
        };
        descriptor_.fields.push_back(std::move(field));
        return *this;
    }

    TypeBuilder& field_attribute(std::size_t index, std::string key, std::string value) {
        if (index < descriptor_.fields.size()) add_attribute(descriptor_.fields[index].attributes, std::move(key), std::move(value));
        return *this;
    }

    template<auto Method>
    TypeBuilder& method(std::string name, MethodFlags flags = MethodFlags::None, std::string displayName = {}) {
        using Traits = MethodTraits<decltype(Method)>;
        using Return = typename Traits::return_type;
        using Arguments = typename Traits::arguments;
        MethodDescriptor method;
        method.name = std::move(name);
        method.displayName = displayName.empty() ? method.name : std::move(displayName);
        method.ownerType = descriptor_.id;
        method.returnType = type_id<Return>();
        method.flags = flags | (Traits::isConst ? MethodFlags::Const : MethodFlags::None);
        method.invoke = &MethodInvoker<Method>::invoke;
        method.argumentTypes.reserve(std::tuple_size_v<Arguments>);
        append_argument_types<MethodDescriptor, Arguments>(method, std::make_index_sequence<std::tuple_size_v<Arguments>>{});
        descriptor_.methods.push_back(std::move(method));
        return *this;
    }

    TypeBuilder& enum_value(std::string name, std::int64_t value) {
        descriptor_.enumValues.push_back({std::move(name), value, {}}); return *this;
    }
    TypeBuilder& enum_value_attribute(std::size_t index, std::string key, std::string value) {
        if (index < descriptor_.enumValues.size()) add_attribute(descriptor_.enumValues[index].attributes, std::move(key), std::move(value));
        return *this;
    }
    template<class Element>
    TypeBuilder& vector_container() {
        descriptor_.container = std::make_unique<ContainerDescriptor>();
        descriptor_.container->elementType = type_id<Element>();
        descriptor_.container->size = &VectorContainerOperations<Element>::size;
        descriptor_.container->resize = &VectorContainerOperations<Element>::resize;
        descriptor_.container->element = &VectorContainerOperations<Element>::element;
        descriptor_.container->mutableElement = &VectorContainerOperations<Element>::mutable_element;
        return *this;
    }
    TypeDescriptor take() & { return std::move(descriptor_); }
    TypeDescriptor take() && { return std::move(descriptor_); }
};

template<class Element>
TypeDescriptor make_vector_descriptor(std::string name, TypeFlags flags = TypeFlags::Serializable) {
    return std::move(TypeBuilder<std::vector<Element>>(std::move(name), TypeKind::Struct, flags)
        .template vector_container<Element>()).take();
}

class TypeRegistry {
    struct ModuleInfo { std::string name; std::uint32_t version{0}; };
    std::unordered_map<TypeId, std::shared_ptr<const TypeDescriptor>> types_;
    std::unordered_map<std::string, TypeId> names_;
    mutable std::shared_mutex mutex_;
    bool frozen_{false};
    std::vector<ModuleInfo> modules_;

    bool validate_descriptor_unlocked(const TypeDescriptor& descriptor, std::string* error) const;
    bool register_type_unlocked(TypeDescriptor descriptor, std::string* error);

public:
    TypeRegistry();
    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;
    TypeRegistry(TypeRegistry&& other) noexcept;
    TypeRegistry& operator=(TypeRegistry&& other) noexcept;

    bool register_type(TypeDescriptor descriptor, std::string* error = nullptr);
    bool register_module(std::string name, std::uint32_t version, std::vector<TypeDescriptor> descriptors,
                         std::string* error = nullptr);
    bool freeze(std::string* error = nullptr);
    bool frozen() const noexcept;
    const TypeDescriptor* find(TypeId id) const noexcept;
    const TypeDescriptor* find(std::string_view name) const noexcept;
    const FieldDescriptor* find_field(TypeId id, std::string_view name) const noexcept;
    const MethodDescriptor* find_method(TypeId id, std::string_view name) const noexcept;
    InvokeResult invoke(TypeId derivedType, std::string_view name, const void* object,
                        const Argument* arguments, std::size_t argumentCount, void* result = nullptr) const noexcept;
    ReflectedInstance create_instance(TypeId id) const noexcept;
    void* cast_object(TypeId derived, TypeId target, void* object) const noexcept;
    const void* cast_object(TypeId derived, TypeId target, const void* object) const noexcept;
    std::size_t size() const noexcept;
    std::size_t module_count() const noexcept;

    template<class Fn>
    bool for_each_field(TypeId id, Fn&& fn) const {
        std::shared_lock lock(mutex_);
        const auto it = types_.find(id);
        if (it == types_.end()) return false;
        std::function<void(const TypeDescriptor*)> visit = [&](const TypeDescriptor* current) {
            if (!current) return;
            for (const auto& base : current->bases) {
                const auto baseIt = types_.find(base.type);
                if (baseIt != types_.end()) visit(baseIt->second.get());
            }
            for (const auto& field : current->fields) fn(*current, field);
        };
        visit(it->second.get());
        return true;
    }
};

} // namespace shinkou::reflection

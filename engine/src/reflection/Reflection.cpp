#include "shinkou/reflection/Reflection.h"

#include <sstream>

namespace shinkou::reflection {

void ReflectedInstance::release() noexcept {
    if (!storage_) return;
    if (descriptor_ && descriptor_->destroy) descriptor_->destroy(storage_);
    if (descriptor_) {
        ::operator delete(storage_, std::align_val_t(descriptor_->alignment));
    } else {
        ::operator delete(storage_);
    }
    descriptor_ = nullptr;
    storage_ = nullptr;
}

void* FieldDescriptor::address_of(const TypeRegistry& registry, TypeId derivedType, void* object) const noexcept {
    return address_of(registry.cast_object(derivedType, ownerType, object));
}

const void* FieldDescriptor::address_of(const TypeRegistry& registry, TypeId derivedType, const void* object) const noexcept {
    return address_of(registry.cast_object(derivedType, ownerType, object));
}

InvokeResult MethodDescriptor::call_on(const TypeRegistry& registry, TypeId derivedType, const void* object,
                                       const Argument* arguments, std::size_t argumentCount, void* result) const noexcept {
    return call(registry.cast_object(derivedType, ownerType, object), arguments, argumentCount, result);
}

ReflectedInstance ReflectedInstance::create(const TypeDescriptor& descriptor) noexcept {
    if (!descriptor.construct || descriptor.size == 0 || descriptor.alignment == 0) return {};
    void* storage = nullptr;
    try {
        storage = ::operator new(descriptor.size, std::align_val_t(descriptor.alignment));
    } catch (...) {
        return {};
    }
    if (!descriptor.construct(storage)) {
        ::operator delete(storage, std::align_val_t(descriptor.alignment));
        return {};
    }
    return ReflectedInstance(&descriptor, storage);
}

const FieldDescriptor* TypeDescriptor::find_field(std::string_view fieldName) const noexcept {
    const auto it = fieldIndex.find(fnv1a(fieldName));
    if (it == fieldIndex.end() || it->second >= fields.size() || fields[it->second].name != fieldName) return nullptr;
    return &fields[it->second];
}

const MethodDescriptor* TypeDescriptor::find_method(std::string_view methodName) const noexcept {
    const auto it = methodIndex.find(fnv1a(methodName));
    if (it == methodIndex.end() || it->second >= methods.size() || methods[it->second].name != methodName) return nullptr;
    return &methods[it->second];
}

const EnumValueDescriptor* TypeDescriptor::find_enum_value(std::string_view valueName) const noexcept {
    const auto it = enumIndex.find(fnv1a(valueName));
    if (it == enumIndex.end() || it->second >= enumValues.size() || enumValues[it->second].name != valueName) return nullptr;
    return &enumValues[it->second];
}

const EnumValueDescriptor* TypeDescriptor::find_enum_value(std::int64_t enumValue) const noexcept {
    const auto index = enumValueIndex.find(enumValue);
    return index == enumValueIndex.end() || index->second >= enumValues.size() ? nullptr : &enumValues[index->second];
}

void TypeDescriptor::rebuild_indexes() {
    fieldIndex.clear();
    methodIndex.clear();
    enumIndex.clear();
    enumValueIndex.clear();
    fieldIndex.reserve(fields.size());
    methodIndex.reserve(methods.size());
    enumIndex.reserve(enumValues.size());
    enumValueIndex.reserve(enumValues.size());
    for (std::size_t index = 0; index < fields.size(); ++index) fieldIndex.emplace(fnv1a(fields[index].name), index);
    for (std::size_t index = 0; index < methods.size(); ++index) methodIndex.emplace(fnv1a(methods[index].name), index);
    for (std::size_t index = 0; index < enumValues.size(); ++index) enumIndex.emplace(fnv1a(enumValues[index].name), index);
    for (std::size_t index = 0; index < enumValues.size(); ++index) enumValueIndex.emplace(enumValues[index].value, index);
}

TypeRegistry::TypeRegistry() {
    auto addPrimitive = [this](TypeId id, std::string name, std::size_t size, std::size_t alignment) {
        TypeDescriptor descriptor;
        descriptor.id = id;
        descriptor.name = name;
        descriptor.displayName = descriptor.name;
        descriptor.size = size;
        descriptor.alignment = alignment;
        descriptor.kind = TypeKind::Primitive;
        auto owned = std::make_shared<const TypeDescriptor>(std::move(descriptor));
        types_.emplace(id, owned);
        names_.emplace(std::move(name), id);
    };

    addPrimitive(type_id<void>(), "void", 0, 1);
    addPrimitive(type_id<bool>(), "bool", sizeof(bool), alignof(bool));
    addPrimitive(type_id<std::int8_t>(), "int8", sizeof(std::int8_t), alignof(std::int8_t));
    addPrimitive(type_id<std::int16_t>(), "int16", sizeof(std::int16_t), alignof(std::int16_t));
    addPrimitive(type_id<std::int32_t>(), "int32", sizeof(std::int32_t), alignof(std::int32_t));
    addPrimitive(type_id<std::int64_t>(), "int64", sizeof(std::int64_t), alignof(std::int64_t));
    addPrimitive(type_id<std::uint8_t>(), "uint8", sizeof(std::uint8_t), alignof(std::uint8_t));
    addPrimitive(type_id<std::uint16_t>(), "uint16", sizeof(std::uint16_t), alignof(std::uint16_t));
    addPrimitive(type_id<std::uint32_t>(), "uint32", sizeof(std::uint32_t), alignof(std::uint32_t));
    addPrimitive(type_id<std::uint64_t>(), "uint64", sizeof(std::uint64_t), alignof(std::uint64_t));
    addPrimitive(type_id<float>(), "float", sizeof(float), alignof(float));
    addPrimitive(type_id<double>(), "double", sizeof(double), alignof(double));

    std::string ignored;
    register_type(TypeBuilder<std::string>("string").take(), &ignored);
    register_type(TypeBuilder<math::Vec2>("shinkou.math.Vec2")
        .field("x", &math::Vec2::x).field("y", &math::Vec2::y).take(), &ignored);
    register_type(TypeBuilder<math::Vec3>("shinkou.math.Vec3")
        .field("x", &math::Vec3::x).field("y", &math::Vec3::y).field("z", &math::Vec3::z).take(), &ignored);
    register_type(TypeBuilder<math::Quat>("shinkou.math.Quat")
        .field("x", &math::Quat::x).field("y", &math::Quat::y).field("z", &math::Quat::z).field("w", &math::Quat::w).take(), &ignored);
    register_type(TypeBuilder<math::Transform>("shinkou.math.Transform")
        .field("position", &math::Transform::position).field("rotation", &math::Transform::rotation)
        .field("scale", &math::Transform::scale).take(), &ignored);
}

TypeRegistry::TypeRegistry(TypeRegistry&& other) noexcept {
    std::unique_lock lock(other.mutex_);
    types_ = std::move(other.types_);
    names_ = std::move(other.names_);
    frozen_ = other.frozen_;
    modules_ = std::move(other.modules_);
    other.frozen_ = false;
}

TypeRegistry& TypeRegistry::operator=(TypeRegistry&& other) noexcept {
    if (this == &other) return *this;
    std::scoped_lock lock(mutex_, other.mutex_);
    types_ = std::move(other.types_);
    names_ = std::move(other.names_);
    frozen_ = other.frozen_;
    modules_ = std::move(other.modules_);
    other.frozen_ = false;
    return *this;
}

bool TypeRegistry::validate_descriptor_unlocked(const TypeDescriptor& descriptor, std::string* error) const {
    if (descriptor.id == 0 || descriptor.name.empty()) {
        if (error) *error = "type id and name are required";
        return false;
    }
    if (descriptor.kind != TypeKind::Primitive && (descriptor.size == 0 || descriptor.alignment == 0)) {
        if (error) *error = "non-primitive type must have size and alignment: " + descriptor.name;
        return false;
    }
    std::unordered_map<std::string_view, bool> fieldNames;
    for (const auto& field : descriptor.fields) {
        if (field.name.empty() || field.type == 0 || field.ownerType != descriptor.id || !field.address || !field.constAddress) {
            if (error) *error = "invalid field in type: " + descriptor.name;
            return false;
        }
        if (!fieldNames.emplace(field.name, true).second) {
            if (error) *error = "duplicate field: " + field.name;
            return false;
        }
    }
    if (descriptor.fieldIndex.size() != descriptor.fields.size() ||
        descriptor.methodIndex.size() != descriptor.methods.size() ||
        descriptor.enumIndex.size() != descriptor.enumValues.size() ||
        descriptor.enumValueIndex.size() != descriptor.enumValues.size()) {
        if (error) *error = "member name hash collision in type: " + descriptor.name;
        return false;
    }
    for (const auto& base : descriptor.bases) {
        if (base.type == 0 || !base.cast || !base.constCast) {
            if (error) *error = "invalid base descriptor in type: " + descriptor.name;
            return false;
        }
    }
    std::unordered_map<TypeId, bool> baseIds;
    for (const auto& base : descriptor.bases) if (!baseIds.emplace(base.type, true).second) {
        if (error) *error = "duplicate base type in type: " + descriptor.name;
        return false;
    }
    if (!descriptor.bases.empty() && descriptor.baseType != descriptor.bases.front().type) {
        if (error) *error = "legacy baseType does not match bases in type: " + descriptor.name;
        return false;
    }
    std::unordered_map<std::string_view, bool> methodNames;
    for (const auto& method : descriptor.methods) {
        if (method.name.empty() || method.ownerType != descriptor.id || !method.invoke) {
            if (error) *error = "invalid method in type: " + descriptor.name;
            return false;
        }
        if (!methodNames.emplace(method.name, true).second) {
            if (error) *error = "duplicate method: " + method.name;
            return false;
        }
        if (method.returnType == 0) {
            if (error) *error = "method return type is unknown: " + method.name;
            return false;
        }
        for (const auto argumentType : method.argumentTypes) if (argumentType == 0) {
            if (error) *error = "method argument type is unknown: " + method.name;
            return false;
        }
    }
    return true;
}

bool TypeRegistry::register_type_unlocked(TypeDescriptor descriptor, std::string* error) {
    if (types_.find(descriptor.id) != types_.end() || names_.find(descriptor.name) != names_.end()) {
        if (error) *error = "duplicate type id or name: " + descriptor.name;
        return false;
    }
    descriptor.rebuild_indexes();
    if (!validate_descriptor_unlocked(descriptor, error)) return false;
    const auto id = descriptor.id;
    const auto name = descriptor.name;
    auto owned = std::make_shared<const TypeDescriptor>(std::move(descriptor));
    types_.emplace(id, std::move(owned));
    names_.emplace(name, id);
    return true;
}

bool TypeRegistry::register_type(TypeDescriptor descriptor, std::string* error) {
    std::unique_lock lock(mutex_);
    if (frozen_) {
        if (error) *error = "type registry is frozen";
        return false;
    }
    return register_type_unlocked(std::move(descriptor), error);
}

bool TypeRegistry::register_module(std::string name, std::uint32_t version,
                                   std::vector<TypeDescriptor> descriptors, std::string* error) {
    std::unique_lock lock(mutex_);
    if (frozen_) {
        if (error) *error = "type registry is frozen";
        return false;
    }
    if (name.empty() || version == 0) {
        if (error) *error = "module name and positive version are required";
        return false;
    }
    for (const auto& module : modules_) if (module.name == name) {
        if (error) *error = "module already registered: " + name;
        return false;
    }
    std::unordered_map<TypeId, bool> ids;
    std::unordered_map<std::string, bool> names;
    for (const auto& descriptor : descriptors) {
        if (!ids.emplace(descriptor.id, true).second || types_.find(descriptor.id) != types_.end() ||
            !names.emplace(descriptor.name, true).second || names_.find(descriptor.name) != names_.end()) {
            if (error) *error = "duplicate type in module: " + descriptor.name;
            return false;
        }
    }
    std::vector<TypeId> added;
    for (auto& descriptor : descriptors) {
        const auto id = descriptor.id;
        if (!register_type_unlocked(std::move(descriptor), error)) {
            for (const auto addedId : added) {
                const auto it = types_.find(addedId);
                if (it != types_.end()) { names_.erase(it->second->name); types_.erase(it); }
            }
            return false;
        }
        added.push_back(id);
    }
    modules_.push_back({std::move(name), version});
    return true;
}

bool TypeRegistry::freeze(std::string* error) {
    std::unique_lock lock(mutex_);
    if (frozen_) return true;
    std::function<bool(TypeId, std::vector<TypeId>&)> validateInheritance =
        [&](TypeId id, std::vector<TypeId>& path) -> bool {
            const auto it = types_.find(id);
            if (it == types_.end()) return false;
            if (std::find(path.begin(), path.end(), id) != path.end()) {
                if (error) *error = "inheritance cycle: " + it->second->name;
                return false;
            }
            path.push_back(id);
            const auto& descriptor = *it->second;
            std::vector<BaseDescriptor> legacyBases = descriptor.bases;
            if (legacyBases.empty() && descriptor.baseType != 0) legacyBases.push_back({descriptor.baseType, descriptor.castToBase, descriptor.constCastToBase});
            for (const auto& base : legacyBases) {
                const auto baseIt = types_.find(base.type);
                if (baseIt == types_.end()) {
                    if (error) *error = "base type is not registered: " + descriptor.name;
                    path.pop_back();
                    return false;
                }
                if (!base.cast || !base.constCast || !validateInheritance(base.type, path)) { path.pop_back(); return false; }
            }
            path.pop_back();
            return true;
        };
    for (const auto& [id, typeHolder] : types_) {
        const auto& type = *typeHolder;
        std::vector<TypeId> path;
        if (!validateInheritance(id, path)) return false;
        for (const auto& field : type.fields) {
            if (types_.find(field.type) == types_.end()) {
                if (error) *error = "field type is not registered: " + field.name;
                return false;
            }
        }
        for (const auto& method : type.methods) {
            if (method.returnType != 0 && types_.find(method.returnType) == types_.end()) {
                if (error) *error = "method return type is not registered: " + method.name;
                return false;
            }
            for (const auto argumentType : method.argumentTypes) {
                if (types_.find(argumentType) == types_.end()) {
                    if (error) *error = "method argument type is not registered: " + method.name;
                    return false;
                }
            }
        }
        if (type.container) {
            if (type.container->elementType == 0 || types_.find(type.container->elementType) == types_.end() ||
                !type.container->size || !type.container->resize || !type.container->element ||
                !type.container->mutableElement) {
                if (error) *error = "invalid container descriptor: " + type.name;
                return false;
            }
            if (type.container->associative &&
                (type.container->keyType == 0 || types_.find(type.container->keyType) == types_.end())) {
                if (error) *error = "invalid associative container key: " + type.name;
                return false;
            }
        }
    }
    frozen_ = true;
    return true;
}

bool TypeRegistry::frozen() const noexcept {
    std::shared_lock lock(mutex_);
    return frozen_;
}

const TypeDescriptor* TypeRegistry::find(TypeId id) const noexcept {
    std::shared_lock lock(mutex_);
    const auto it = types_.find(id);
    return it == types_.end() ? nullptr : it->second.get();
}

const TypeDescriptor* TypeRegistry::find(std::string_view name) const noexcept {
    std::shared_lock lock(mutex_);
    const auto type = types_.find(fnv1a(name));
    return type == types_.end() || type->second->name != name ? nullptr : type->second.get();
}

const FieldDescriptor* TypeRegistry::find_field(TypeId id, std::string_view name) const noexcept {
    std::shared_lock lock(mutex_);
    std::function<const FieldDescriptor*(TypeId)> visit = [&](TypeId currentId) -> const FieldDescriptor* {
        const auto it = types_.find(currentId);
        if (it == types_.end()) return nullptr;
        if (const auto* field = it->second->find_field(name)) return field;
        for (const auto& base : it->second->bases) if (const auto* field = visit(base.type)) return field;
        if (it->second->bases.empty() && it->second->baseType != 0) return visit(it->second->baseType);
        return nullptr;
    };
    return visit(id);
}

const MethodDescriptor* TypeRegistry::find_method(TypeId id, std::string_view name) const noexcept {
    std::shared_lock lock(mutex_);
    std::function<const MethodDescriptor*(TypeId)> visit = [&](TypeId currentId) -> const MethodDescriptor* {
        const auto it = types_.find(currentId);
        if (it == types_.end()) return nullptr;
        if (const auto* method = it->second->find_method(name)) return method;
        for (const auto& base : it->second->bases) if (const auto* method = visit(base.type)) return method;
        if (it->second->bases.empty() && it->second->baseType != 0) return visit(it->second->baseType);
        return nullptr;
    };
    return visit(id);
}

InvokeResult TypeRegistry::invoke(TypeId derivedType, std::string_view name, const void* object,
                                  const Argument* arguments, std::size_t argumentCount, void* result) const noexcept {
    const auto* method = find_method(derivedType, name);
    return method ? method->call_on(*this, derivedType, object, arguments, argumentCount, result)
                  : InvokeResult{InvokeError::MethodFailure};
}

ReflectedInstance TypeRegistry::create_instance(TypeId id) const noexcept {
    std::shared_ptr<const TypeDescriptor> owner;
    {
        std::shared_lock lock(mutex_);
        const auto it = types_.find(id);
        if (it == types_.end()) return {};
        owner = it->second;
    }
    const auto* descriptor = owner.get();
    if (!descriptor || !descriptor->construct || descriptor->size == 0 || descriptor->alignment == 0) return {};
    void* storage = nullptr;
    try { storage = ::operator new(descriptor->size, std::align_val_t(descriptor->alignment)); }
    catch (...) { return {}; }
    if (!descriptor->construct(storage)) {
        ::operator delete(storage, std::align_val_t(descriptor->alignment));
        return {};
    }
    return ReflectedInstance(std::move(owner), storage);
}

void* TypeRegistry::cast_object(TypeId derived, TypeId target, void* object) const noexcept {
    if (!object) return nullptr;
    if (derived == target) return object;
    const auto* descriptor = find(derived);
    if (!descriptor) return nullptr;
    for (const auto& base : descriptor->bases) {
        void* baseObject = base.cast(object);
        if (void* result = cast_object(base.type, target, baseObject)) return result;
    }
    if (descriptor->bases.empty() && descriptor->baseType != 0 && descriptor->castToBase) {
        return cast_object(descriptor->baseType, target, descriptor->castToBase(object));
    }
    return nullptr;
}

const void* TypeRegistry::cast_object(TypeId derived, TypeId target, const void* object) const noexcept {
    if (!object) return nullptr;
    if (derived == target) return object;
    const auto* descriptor = find(derived);
    if (!descriptor) return nullptr;
    for (const auto& base : descriptor->bases) {
        const void* baseObject = base.constCast(object);
        if (const void* result = cast_object(base.type, target, baseObject)) return result;
    }
    if (descriptor->bases.empty() && descriptor->baseType != 0 && descriptor->constCastToBase) {
        return cast_object(descriptor->baseType, target, descriptor->constCastToBase(object));
    }
    return nullptr;
}

std::size_t TypeRegistry::size() const noexcept {
    std::shared_lock lock(mutex_);
    return types_.size();
}

std::size_t TypeRegistry::module_count() const noexcept {
    std::shared_lock lock(mutex_);
    return modules_.size();
}

} // namespace shinkou::reflection

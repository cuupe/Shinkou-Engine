#include "shinkou/data/Json.h"
#include "shinkou/reflection/Serialization.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace shinkou::reflection {
namespace {
using JsonNode = data::Value;

SerializationResult failure(SerializationError error, std::string message) {
    return {error, 0, std::move(message)};
}

bool is_integer_type(std::string_view name) {
    return name == "int8" || name == "int16" || name == "int32" || name == "int64" ||
           name == "uint8" || name == "uint16" || name == "uint32" || name == "uint64";
}
bool is_unsigned_type(std::string_view name) {
    return name == "uint8" || name == "uint16" || name == "uint32" || name == "uint64";
}

std::int64_t read_enum_signed(const void *object, std::size_t size) {
    if (size == 1) {
        std::int8_t value{};
        std::memcpy(&value, object, 1);
        return value;
    }
    if (size == 2) {
        std::int16_t value{};
        std::memcpy(&value, object, 2);
        return value;
    }
    if (size == 4) {
        std::int32_t value{};
        std::memcpy(&value, object, 4);
        return value;
    }
    std::int64_t value{};
    std::memcpy(&value, object, std::min(size, sizeof(value)));
    return value;
}
std::uint64_t read_enum_unsigned(const void *object, std::size_t size) {
    if (size == 1) {
        std::uint8_t value{};
        std::memcpy(&value, object, 1);
        return value;
    }
    if (size == 2) {
        std::uint16_t value{};
        std::memcpy(&value, object, 2);
        return value;
    }
    if (size == 4) {
        std::uint32_t value{};
        std::memcpy(&value, object, 4);
        return value;
    }
    std::uint64_t value{};
    std::memcpy(&value, object, std::min(size, sizeof(value)));
    return value;
}
void write_enum_signed(void *object, std::size_t size, std::int64_t value) {
    if (size == 1) {
        const auto v = static_cast<std::int8_t>(value);
        std::memcpy(object, &v, 1);
    } else if (size == 2) {
        const auto v = static_cast<std::int16_t>(value);
        std::memcpy(object, &v, 2);
    } else if (size == 4) {
        const auto v = static_cast<std::int32_t>(value);
        std::memcpy(object, &v, 4);
    } else
        std::memcpy(object, &value, std::min(size, sizeof(value)));
}
void write_enum_unsigned(void *object, std::size_t size, std::uint64_t value) {
    if (size == 1) {
        const auto v = static_cast<std::uint8_t>(value);
        std::memcpy(object, &v, 1);
    } else if (size == 2) {
        const auto v = static_cast<std::uint16_t>(value);
        std::memcpy(object, &v, 2);
    } else if (size == 4) {
        const auto v = static_cast<std::uint32_t>(value);
        std::memcpy(object, &v, 4);
    } else
        std::memcpy(object, &value, std::min(size, sizeof(value)));
}

SerializationResult read_value(const TypeRegistry& registry, TypeId typeId, const JsonNode& node,
                               void* object, const JsonOptions& options, std::size_t depth);
SerializationResult write_value(const TypeRegistry& registry, TypeId typeId, const void* object,
                                data::Value& output, const JsonOptions& options,
                                std::size_t depth, std::size_t& nodes) {
    if (depth > options.maxDepth || depth > data::maxDocumentDepth) return failure(SerializationError::DepthExceeded, "maximum serialization depth exceeded");
    if (++nodes > options.maxNodes) return failure(SerializationError::LimitExceeded, "serialization node limit exceeded");
    const auto* type = registry.find(typeId);
    if (!type) return failure(SerializationError::InvalidType, "type is not registered");
    if (!object) return failure(SerializationError::NullObject, "object is null");
    if (type->name == "void") { output = {}; return {}; }
    if (type->name == "string") { output = *static_cast<const std::string*>(object); return {}; }
    if (type->name == "bool") { output = *static_cast<const bool*>(object); return {}; }
    if (is_integer_type(type->name)) {
        if (is_unsigned_type(type->name)) output = read_enum_unsigned(object, type->size);
        else output = read_enum_signed(object, type->size);
        return {};
    }
    if (type->name == "float" || type->name == "double") {
        const double value = type->name == "float" ? static_cast<double>(*static_cast<const float*>(object)) : *static_cast<const double*>(object);
        if (!std::isfinite(value)) return failure(SerializationError::RangeError, "non-finite number cannot be serialized");
        output = value; return {};
    }
    if (type->kind == TypeKind::Enum) {
        const auto* underlying = registry.find(type->underlyingType);
        if (underlying && is_unsigned_type(underlying->name)) {
            const auto value = read_enum_unsigned(object, type->size);
            const auto* named = value <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ? type->find_enum_value(static_cast<std::int64_t>(value)) : nullptr;
            output = named ? data::Value{named->name} : data::Value{value};
        } else {
            const auto value = read_enum_signed(object, type->size);
            const auto* named = type->find_enum_value(value);
            output = named ? data::Value{named->name} : data::Value{value};
        }
        return {};
    }
    if (type->container) {
        const auto count = type->container->size(object);
        if (count > options.maxNodes - (std::min)(options.maxNodes, nodes))
            return failure(SerializationError::LimitExceeded, "serialization container limit exceeded");
        output = data::Value::array();
        output.array_items().reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            data::Value child;
            auto result = write_value(registry, type->container->elementType, type->container->element(object,index), child, options, depth+1, nodes);
            if (!result) return result;
            output.array_items().push_back(std::move(child));
        }
        return {};
    }
    if (type->fields.empty() && type->bases.empty() && !type->baseType)
        return failure(SerializationError::UnsupportedType, "opaque type cannot be serialized: " + type->name);
    output = data::Value::object();
    auto write_fields = [&](const TypeDescriptor& owner, const void* ownerObject) -> SerializationResult {
        for (const auto& field : owner.fields) {
            if (!options.includeTransient && has_flag(field.flags, FieldFlags::Transient)) continue;
            const auto* fieldObject = field.address_of(ownerObject);
            if (!fieldObject) return failure(SerializationError::NullObject, "field address is null: " + field.name);
            data::Value child;
            auto result = write_value(registry, field.type, fieldObject, child, options, depth+1, nodes);
            if (!result) return result;
            if (!output.object_items().emplace(field.name,std::move(child)).second)
                return failure(SerializationError::InvalidType, "ambiguous reflected field: " + field.name);
        }
        return {};
    };
    std::vector<TypeId> visiting;
    std::function<SerializationResult(const TypeDescriptor*)> chain = [&](const TypeDescriptor* current) -> SerializationResult {
        if (visiting.size() > options.maxDepth || visiting.size() > data::maxDocumentDepth || std::find(visiting.begin(),visiting.end(),current->id) != visiting.end())
            return failure(SerializationError::DepthExceeded, "cyclic or excessive inheritance depth");
        visiting.push_back(current->id);
        auto bases = current->bases;
        if (bases.empty() && current->baseType) bases.push_back({current->baseType,current->castToBase,current->constCastToBase});
        for (const auto& base : bases) {
            const auto* descriptor = registry.find(base.type);
            if (!descriptor) return failure(SerializationError::InvalidType, "invalid inheritance base");
            auto result = chain(descriptor);
            if (!result) return result;
        }
        const void* ownerObject = registry.cast_object(type->id,current->id,object);
        if (!ownerObject) return failure(SerializationError::InvalidType, "invalid inheritance cast");
        auto result = write_fields(*current,ownerObject);
        visiting.pop_back();
        return result;
    };
    return chain(type);
}

bool number_to_signed(const JsonNode &node, std::int64_t &value) {
    return node.to_int64(value);
}
bool number_to_unsigned(const JsonNode &node, std::uint64_t &value) {
    return node.to_uint64(value);
}

SerializationResult read_value(const TypeRegistry &registry, TypeId typeId, const JsonNode &node,
                               void *object, const JsonOptions &options, std::size_t depth) {
    if (depth > options.maxDepth || depth > data::maxDocumentDepth)
        return failure(SerializationError::DepthExceeded, "maximum deserialization depth exceeded");
    const auto *type = registry.find(typeId);
    if (!type || !object)
        return failure(!type ? SerializationError::InvalidType : SerializationError::NullObject,
                       "invalid destination");
    if (type->name == "string") {
        if (node.kind() != JsonNode::Kind::String)
            return failure(SerializationError::TypeMismatch, "expected string");
        *static_cast<std::string *>(object) = node.string_or();
        return {};
    }
    if (type->name == "bool") {
        if (node.kind() != JsonNode::Kind::Boolean)
            return failure(SerializationError::TypeMismatch, "expected boolean");
        *static_cast<bool *>(object) = node.bool_or();
        return {};
    }
    if (is_integer_type(type->name)) {
        if (is_unsigned_type(type->name)) {
            std::uint64_t value = 0;
            if (!number_to_unsigned(node, value))
                return failure(SerializationError::RangeError, "expected unsigned integer");
            if ((type->name == "uint8" && value > std::numeric_limits<std::uint8_t>::max()) ||
                (type->name == "uint16" && value > std::numeric_limits<std::uint16_t>::max()) ||
                (type->name == "uint32" && value > std::numeric_limits<std::uint32_t>::max()))
                return failure(SerializationError::RangeError, "unsigned integer out of range");
            if (type->name == "uint8")
                *static_cast<std::uint8_t *>(object) = static_cast<std::uint8_t>(value);
            else if (type->name == "uint16")
                *static_cast<std::uint16_t *>(object) = static_cast<std::uint16_t>(value);
            else if (type->name == "uint32")
                *static_cast<std::uint32_t *>(object) = static_cast<std::uint32_t>(value);
            else
                *static_cast<std::uint64_t *>(object) = value;
        } else {
            std::int64_t value = 0;
            if (!number_to_signed(node, value))
                return failure(SerializationError::RangeError, "expected signed integer");
            if ((type->name == "int8" && (value < std::numeric_limits<std::int8_t>::min() ||
                                          value > std::numeric_limits<std::int8_t>::max())) ||
                (type->name == "int16" && (value < std::numeric_limits<std::int16_t>::min() ||
                                           value > std::numeric_limits<std::int16_t>::max())) ||
                (type->name == "int32" && (value < std::numeric_limits<std::int32_t>::min() ||
                                           value > std::numeric_limits<std::int32_t>::max())))
                return failure(SerializationError::RangeError, "signed integer out of range");
            if (type->name == "int8")
                *static_cast<std::int8_t *>(object) = static_cast<std::int8_t>(value);
            else if (type->name == "int16")
                *static_cast<std::int16_t *>(object) = static_cast<std::int16_t>(value);
            else if (type->name == "int32")
                *static_cast<std::int32_t *>(object) = static_cast<std::int32_t>(value);
            else
                *static_cast<std::int64_t *>(object) = value;
        }
        return {};
    }
    if (type->name == "float" || type->name == "double") {
        if (node.kind() != JsonNode::Kind::Number)
            return failure(SerializationError::TypeMismatch, "expected number");
        if (!std::isfinite(node.number_or()) || (type->name == "float" &&
            (node.number_or() < -(std::numeric_limits<float>::max)() || node.number_or() > (std::numeric_limits<float>::max)())))
            return failure(SerializationError::RangeError, "floating point value out of range");
        if (type->name == "float")
            *static_cast<float *>(object) = static_cast<float>(node.number_or());
        else
            *static_cast<double *>(object) = node.number_or();
        return {};
    }
    if (type->kind == TypeKind::Enum) {
        if (type->size != 1 && type->size != 2 && type->size != 4 && type->size != 8)
            return failure(SerializationError::UnsupportedType,"unsupported enum representation");
        const auto* underlying = registry.find(type->underlyingType);
        const bool unsignedValue = underlying && is_unsigned_type(underlying->name);
        if (node.kind() == JsonNode::Kind::String) {
            const auto* found = type->find_enum_value(node.string_or());
            if (!found) return failure(SerializationError::TypeMismatch,"unknown enum value");
            if (unsignedValue) write_enum_unsigned(object,type->size,static_cast<std::uint64_t>(found->value));
            else write_enum_signed(object,type->size,found->value);
            return {};
        }
        if (unsignedValue) {
            std::uint64_t value = 0;
            if (!node.to_uint64(value) || (type->size < 8 && value >= (std::uint64_t{1} << (type->size*8))))
                return failure(SerializationError::RangeError,"enum value out of range");
            write_enum_unsigned(object,type->size,value);
        } else {
            std::int64_t value = 0;
            if (!node.to_int64(value)) return failure(SerializationError::RangeError,"expected enum integer");
            if (type->size < 8) {
                const auto bound = std::int64_t{1} << (type->size*8-1);
                if (value < -bound || value >= bound) return failure(SerializationError::RangeError,"enum value out of range");
            }
            write_enum_signed(object,type->size,value);
        }
        return {};
    }
    if (type->container) {
        if (node.kind() != JsonNode::Kind::Array)
            return failure(SerializationError::TypeMismatch, "expected array");
        if (!type->container->resize(object, node.array_items().size()))
            return failure(SerializationError::ContainerFailure, "container resize failed");
        for (std::size_t index = 0; index < node.array_items().size(); ++index) {
            auto *element = type->container->mutableElement(object, index);
            auto result = read_value(registry, type->container->elementType, node.array_items()[index],
                                     element, options, depth + 1);
            if (!result)
                return result;
        }
        return {};
    }
    bool hasFields = !type->fields.empty();
    if (!hasFields && (!type->bases.empty() || type->baseType != 0))
        hasFields = true;
    if (node.kind() != JsonNode::Kind::Object || !hasFields)
        return failure(SerializationError::TypeMismatch, "expected reflected object");
    std::vector<bool> seen;
    std::vector<std::pair<const FieldDescriptor *, void *>> fields;
    std::function<void(const TypeDescriptor *)> collect_fields =
        [&](const TypeDescriptor *current) {
            std::vector<BaseDescriptor> bases = current->bases;
            if (bases.empty() && current->baseType != 0)
                bases.push_back({current->baseType, current->castToBase, current->constCastToBase});
            for (const auto &base : bases)
                if (const auto *baseType = registry.find(base.type))
                    collect_fields(baseType);
            void *ownerObject = registry.cast_object(type->id, current->id, object);
            for (const auto &field : current->fields)
                fields.emplace_back(&field, field.address_of(ownerObject));
        };
    collect_fields(type);
    seen.assign(fields.size(), false);
    for (const auto &[key, value] : node.object_items()) {
        std::size_t index = 0;
        for (; index < fields.size() && fields[index].first->name != key; ++index) {
        }
        if (index == fields.size()) {
            if (options.rejectUnknownFields)
                return failure(SerializationError::UnknownField, "unknown field: " + key);
            continue;
        }
        auto result = read_value(registry, fields[index].first->type, value, fields[index].second,
                                 options, depth + 1);
        if (!result)
            return result;
        seen[index] = true;
    }
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (has_flag(fields[index].first->flags, FieldFlags::Required) && !seen[index])
            return failure(SerializationError::MissingField,
                           "required field is missing: " + fields[index].first->name);
    }
    return {};
}

} // namespace

SerializationResult serialize_value(const TypeRegistry& registry, TypeId type, const void* object,
                                    data::Value& output, const JsonOptions& options) {
    data::Value temporary;
    std::size_t nodes = 0;
    try {
        auto result = write_value(registry,type,object,temporary,options,0,nodes);
        if (result) output = std::move(temporary);
        return result;
    } catch (const std::exception& error) { return failure(SerializationError::ContainerFailure,error.what()); }
}
SerializationResult deserialize_value(const TypeRegistry& registry, TypeId type, const data::Value& value,
                                      void* object, const JsonOptions& options) {
    return read_value(registry,type,value,object,options,0);
}
SerializationResult serialize_json(const TypeRegistry& registry, TypeId type, const void* object,
                                   std::string& output, const JsonOptions& options) {
    data::Value value;
    auto result = serialize_value(registry,type,object,value,options);
    if (!result) return result;
    auto encoded = data::write_json(value,{options.pretty,options.maxDepth,options.maxBytes});
    if (!encoded) return {encoded.error().code == core::ErrorCode::LimitExceeded ? SerializationError::LimitExceeded : SerializationError::InvalidJson, encoded.error().offset, encoded.error().message};
    output = std::move(encoded).value();
    return {};
}
SerializationResult deserialize_json(const TypeRegistry& registry, TypeId type,
                                     std::string_view input, void* object,
                                     const JsonOptions& options) {
    data::JsonLimits limits;
    limits.maxDepth = options.maxDepth;
    limits.maxInputBytes = limits.maxStringBytes = options.maxBytes;
    limits.maxNodes = options.maxNodes;
    auto parsed = data::parse_json(input,limits);
    if (!parsed) {
        const auto& error = parsed.error();
        const auto code = error.code == core::ErrorCode::OutOfRange ? SerializationError::RangeError
            : error.code == core::ErrorCode::LimitExceeded ? (error.message.find("depth") != std::string::npos ? SerializationError::DepthExceeded : SerializationError::LimitExceeded) : SerializationError::InvalidJson;
        return {code,error.offset,error.message};
    }
    return deserialize_value(registry,type,parsed.value(),object,options);
}
} // namespace shinkou::reflection

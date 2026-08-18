#include "shinkou/reflection/Serialization.h"

#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace shinkou::reflection {
namespace {

struct JsonNode {
    enum class Kind : std::uint8_t { Null, Boolean, Number, String, Array, Object };
    Kind kind{Kind::Null};
    bool boolean{false};
    double number{0.0};
    bool integer{false};
    std::int64_t integerValue{0};
    bool unsignedInteger{false};
    std::uint64_t unsignedIntegerValue{0};
    std::string string;
    std::vector<JsonNode> array;
    std::vector<std::pair<std::string, JsonNode>> object;

    const JsonNode* find(std::string_view name) const noexcept {
        for (const auto& [key, value] : object) if (key == name) return &value;
        return nullptr;
    }
};

struct JsonParser {
    std::string_view input;
    std::size_t position{0};
    SerializationResult result{};

    explicit JsonParser(std::string_view source) : input(source) {}

    void fail(SerializationError error, std::string message) {
        if (result.error == SerializationError::None) {
            result.error = error;
            result.offset = position;
            result.message = std::move(message);
        }
    }
    void whitespace() noexcept { while (position < input.size() && std::isspace(static_cast<unsigned char>(input[position]))) ++position; }
    bool consume(char expected) {
        whitespace();
        if (position >= input.size() || input[position] != expected) { fail(SerializationError::InvalidJson, "unexpected token"); return false; }
        ++position;
        return true;
    }
    static void append_utf8(std::string& output, unsigned value) {
        if (value <= 0x7f) output.push_back(static_cast<char>(value));
        else if (value <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (value >> 6)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xe0 | (value >> 12)));
            output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        }
    }
    bool string_value(std::string& output) {
        if (!consume('"')) return false;
        while (position < input.size()) {
            const auto character = static_cast<unsigned char>(input[position++]);
            if (character == '"') return true;
            if (character < 0x20) { fail(SerializationError::InvalidJson, "control character in string"); return false; }
            if (character != '\\') { output.push_back(static_cast<char>(character)); continue; }
            if (position >= input.size()) break;
            const char escape = input[position++];
            switch (escape) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                if (position + 4 > input.size()) { fail(SerializationError::InvalidJson, "short unicode escape"); return false; }
                unsigned value = 0;
                for (int index = 0; index < 4; ++index) {
                    const char digit = input[position++];
                    value <<= 4;
                    if (digit >= '0' && digit <= '9') value += static_cast<unsigned>(digit - '0');
                    else if (digit >= 'a' && digit <= 'f') value += static_cast<unsigned>(digit - 'a' + 10);
                    else if (digit >= 'A' && digit <= 'F') value += static_cast<unsigned>(digit - 'A' + 10);
                    else { fail(SerializationError::InvalidJson, "invalid unicode escape"); return false; }
                }
                append_utf8(output, value);
                break;
            }
            default: fail(SerializationError::InvalidJson, "invalid string escape"); return false;
            }
        }
        fail(SerializationError::InvalidJson, "unterminated string");
        return false;
    }
    JsonNode value() {
        whitespace();
        JsonNode node;
        if (position >= input.size()) { fail(SerializationError::InvalidJson, "missing value"); return node; }
        if (input.compare(position, 4, "null") == 0) { position += 4; return node; }
        if (input.compare(position, 4, "true") == 0) { position += 4; node.kind = JsonNode::Kind::Boolean; node.boolean = true; return node; }
        if (input.compare(position, 5, "false") == 0) { position += 5; node.kind = JsonNode::Kind::Boolean; node.boolean = false; return node; }
        if (input[position] == '"') { node.kind = JsonNode::Kind::String; string_value(node.string); return node; }
        if (input[position] == '[') return array_value();
        if (input[position] == '{') return object_value();
        return number_value();
    }
    JsonNode number_value() {
        JsonNode node;
        const auto start = position;
        const bool negative = position < input.size() && input[position] == '-';
        if (negative) ++position;
        const auto integerStart = position;
        while (position < input.size() && std::isdigit(static_cast<unsigned char>(input[position]))) ++position;
        if (integerStart == position) { fail(SerializationError::InvalidJson, "invalid number"); return node; }
        if (position - integerStart > 1 && input[integerStart] == '0') {
            fail(SerializationError::InvalidJson, "leading zero in number");
            return node;
        }
        bool integer = true;
        if (position < input.size() && input[position] == '.') {
            integer = false;
            ++position;
            const auto fractionStart = position;
            while (position < input.size() && std::isdigit(static_cast<unsigned char>(input[position]))) ++position;
            if (fractionStart == position) { fail(SerializationError::InvalidJson, "missing fraction digits"); return node; }
        }
        if (position < input.size() && (input[position] == 'e' || input[position] == 'E')) { integer = false; ++position; if (position < input.size() && (input[position] == '+' || input[position] == '-')) ++position; while (position < input.size() && std::isdigit(static_cast<unsigned char>(input[position]))) ++position; }
        if (!integer && position > 0 && (input[position - 1] == 'e' || input[position - 1] == 'E' || input[position - 1] == '+' || input[position - 1] == '-')) {
            fail(SerializationError::InvalidJson, "missing exponent digits");
            return node;
        }
        if (start == position) { fail(SerializationError::InvalidJson, "invalid number"); return node; }
        const std::string token(input.substr(start, position - start));
        char* end = nullptr;
        node.number = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(node.number)) { fail(SerializationError::InvalidJson, "invalid number"); return node; }
        node.kind = JsonNode::Kind::Number;
        node.integer = integer;
        if (integer) {
            try {
                if (negative) node.integerValue = std::stoll(token);
                else {
                    node.unsignedIntegerValue = std::stoull(token);
                    node.unsignedInteger = node.unsignedIntegerValue > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
                    if (!node.unsignedInteger) node.integerValue = static_cast<std::int64_t>(node.unsignedIntegerValue);
                }
            } catch (...) {
                node.integer = false;
            }
        }
        return node;
    }
    JsonNode array_value() {
        JsonNode node; node.kind = JsonNode::Kind::Array;
        if (!consume('[')) return node;
        whitespace();
        if (position < input.size() && input[position] == ']') { ++position; return node; }
        for (;;) {
            node.array.push_back(value());
            if (result.error != SerializationError::None) return node;
            whitespace();
            if (position < input.size() && input[position] == ']') { ++position; return node; }
            if (!consume(',')) return node;
        }
    }
    JsonNode object_value() {
        JsonNode node; node.kind = JsonNode::Kind::Object;
        if (!consume('{')) return node;
        whitespace();
        if (position < input.size() && input[position] == '}') { ++position; return node; }
        for (;;) {
            std::string key;
            if (!string_value(key)) return node;
            for (const auto& existing : node.object) if (existing.first == key) {
                fail(SerializationError::InvalidJson, "duplicate object key");
                return node;
            }
            if (!consume(':')) return node;
            node.object.emplace_back(std::move(key), value());
            if (result.error != SerializationError::None) return node;
            whitespace();
            if (position < input.size() && input[position] == '}') { ++position; return node; }
            if (!consume(',')) return node;
        }
    }
    JsonNode parse() {
        JsonNode node = value();
        whitespace();
        if (result.error == SerializationError::None && position != input.size()) fail(SerializationError::InvalidJson, "trailing data");
        return node;
    }
};

void indent(std::string& output, const JsonOptions& options, std::size_t depth) {
    if (!options.pretty) return;
    output.push_back('\n');
    output.append(depth * 2, ' ');
}
void write_string(std::string& output, std::string_view value) {
    output.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20) { char buffer[8]{}; std::snprintf(buffer, sizeof(buffer), "\\u%04x", character); output += buffer; }
            else output.push_back(static_cast<char>(character));
        }
    }
    output.push_back('"');
}

SerializationResult failure(SerializationError error, std::string message) { return {error, 0, std::move(message)}; }

bool is_integer_type(std::string_view name) { return name == "int8" || name == "int16" || name == "int32" || name == "int64" || name == "uint8" || name == "uint16" || name == "uint32" || name == "uint64"; }
bool is_unsigned_type(std::string_view name) { return name == "uint8" || name == "uint16" || name == "uint32" || name == "uint64"; }

std::int64_t read_enum_signed(const void* object, std::size_t size) {
    if (size == 1) { std::int8_t value{}; std::memcpy(&value, object, 1); return value; }
    if (size == 2) { std::int16_t value{}; std::memcpy(&value, object, 2); return value; }
    if (size == 4) { std::int32_t value{}; std::memcpy(&value, object, 4); return value; }
    std::int64_t value{}; std::memcpy(&value, object, std::min(size, sizeof(value))); return value;
}
std::uint64_t read_enum_unsigned(const void* object, std::size_t size) {
    if (size == 1) { std::uint8_t value{}; std::memcpy(&value, object, 1); return value; }
    if (size == 2) { std::uint16_t value{}; std::memcpy(&value, object, 2); return value; }
    if (size == 4) { std::uint32_t value{}; std::memcpy(&value, object, 4); return value; }
    std::uint64_t value{}; std::memcpy(&value, object, std::min(size, sizeof(value))); return value;
}
void write_enum_signed(void* object, std::size_t size, std::int64_t value) {
    if (size == 1) { const auto v = static_cast<std::int8_t>(value); std::memcpy(object, &v, 1); }
    else if (size == 2) { const auto v = static_cast<std::int16_t>(value); std::memcpy(object, &v, 2); }
    else if (size == 4) { const auto v = static_cast<std::int32_t>(value); std::memcpy(object, &v, 4); }
    else std::memcpy(object, &value, std::min(size, sizeof(value)));
}
void write_enum_unsigned(void* object, std::size_t size, std::uint64_t value) {
    if (size == 1) { const auto v = static_cast<std::uint8_t>(value); std::memcpy(object, &v, 1); }
    else if (size == 2) { const auto v = static_cast<std::uint16_t>(value); std::memcpy(object, &v, 2); }
    else if (size == 4) { const auto v = static_cast<std::uint32_t>(value); std::memcpy(object, &v, 4); }
    else std::memcpy(object, &value, std::min(size, sizeof(value)));
}

SerializationResult write_value(const TypeRegistry& registry, TypeId typeId, const void* object,
                                std::string& output, const JsonOptions& options, std::size_t depth);
SerializationResult read_value(const TypeRegistry& registry, TypeId typeId, const JsonNode& node,
                               void* object, const JsonOptions& options, std::size_t depth);

SerializationResult write_value(const TypeRegistry& registry, TypeId typeId, const void* object,
                                std::string& output, const JsonOptions& options, std::size_t depth) {
    if (depth > options.maxDepth) return failure(SerializationError::DepthExceeded, "maximum serialization depth exceeded");
    const auto* type = registry.find(typeId);
    if (!type) return failure(SerializationError::InvalidType, "type is not registered");
    if (!object) return failure(SerializationError::NullObject, "object is null");
    if (type->name == "void") { output += "null"; return {}; }
    if (type->name == "string") { write_string(output, *static_cast<const std::string*>(object)); return {}; }
    if (type->name == "bool") { output += *static_cast<const bool*>(object) ? "true" : "false"; return {}; }
    if (is_integer_type(type->name)) {
        char buffer[64]{};
        if (type->name == "uint8") std::snprintf(buffer, sizeof(buffer), "%u", static_cast<unsigned>(*static_cast<const std::uint8_t*>(object)));
        else if (type->name == "uint16") std::snprintf(buffer, sizeof(buffer), "%u", static_cast<unsigned>(*static_cast<const std::uint16_t*>(object)));
        else if (type->name == "uint32") std::snprintf(buffer, sizeof(buffer), "%u", *static_cast<const std::uint32_t*>(object));
        else if (type->name == "uint64") std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(*static_cast<const std::uint64_t*>(object)));
        else if (type->name == "int8") std::snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(*static_cast<const std::int8_t*>(object)));
        else if (type->name == "int16") std::snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(*static_cast<const std::int16_t*>(object)));
        else if (type->name == "int32") std::snprintf(buffer, sizeof(buffer), "%d", *static_cast<const std::int32_t*>(object));
        else std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(*static_cast<const std::int64_t*>(object)));
        output += buffer;
        return {};
    }
    if (type->name == "float" || type->name == "double") {
        char buffer[64]{};
        const double value = type->name == "float" ? static_cast<double>(*static_cast<const float*>(object)) : *static_cast<const double*>(object);
        std::snprintf(buffer, sizeof(buffer), "%.17g", value);
        output += buffer;
        return {};
    }
    if (type->kind == TypeKind::Enum) {
        const auto* underlying = registry.find(type->underlyingType);
        const bool unsignedValue = underlying && is_unsigned_type(underlying->name);
        const auto value = unsignedValue ? static_cast<std::int64_t>(read_enum_unsigned(object, type->size)) : read_enum_signed(object, type->size);
        if (const auto* enumValue = type->find_enum_value(value)) write_string(output, enumValue->name);
        else output += std::to_string(value);
        return {};
    }
    if (type->container) {
        output.push_back('[');
        const auto count = type->container->size(object);
        for (std::size_t index = 0; index < count; ++index) {
            if (index) output.push_back(',');
            indent(output, options, depth + 1);
            const auto* element = type->container->element(object, index);
            auto result = write_value(registry, type->container->elementType, element, output, options, depth + 1);
            if (!result) return result;
        }
        if (count) indent(output, options, depth);
        output.push_back(']');
        return {};
    }
    bool hasFields = !type->fields.empty();
    if (!hasFields && (!type->bases.empty() || type->baseType != 0)) hasFields = true;
    if (!hasFields) return failure(SerializationError::UnsupportedType, "opaque type cannot be serialized: " + type->name);
    output.push_back('{');
    bool first = true;
    auto write_fields = [&](const TypeDescriptor& owner, const void* ownerObject) -> SerializationResult {
        for (const auto& field : owner.fields) {
            if (!options.includeTransient && has_flag(field.flags, FieldFlags::Transient)) continue;
            const auto* fieldObject = field.address_of(ownerObject);
            if (!fieldObject) return failure(SerializationError::NullObject, "field address is null: " + field.name);
            if (!first) output.push_back(',');
            first = false;
            indent(output, options, depth + 1);
            write_string(output, field.name);
            output.push_back(':');
            if (options.pretty) output.push_back(' ');
            auto result = write_value(registry, field.type, fieldObject, output, options, depth + 1);
            if (!result) return result;
        }
        return {};
    };
    std::function<SerializationResult(const TypeDescriptor*)> write_chain = [&](const TypeDescriptor* current) -> SerializationResult {
        std::vector<BaseDescriptor> bases = current->bases;
        if (bases.empty() && current->baseType != 0) bases.push_back({current->baseType, current->castToBase, current->constCastToBase});
        for (const auto& base : bases) {
            const auto* baseType = registry.find(base.type);
            if (!baseType) return failure(SerializationError::InvalidType, "invalid inheritance base");
            auto result = write_chain(baseType);
            if (!result) return result;
        }
        const void* ownerObject = registry.cast_object(type->id, current->id, object);
        if (!ownerObject) return failure(SerializationError::InvalidType, "invalid inheritance cast");
        return write_fields(*current, ownerObject);
    };
    auto chainResult = write_chain(type);
    if (!chainResult) return chainResult;
    if (!first) indent(output, options, depth);
    output.push_back('}');
    return {};
}

bool number_to_signed(const JsonNode& node, std::int64_t& value) {
    if (node.kind != JsonNode::Kind::Number || !std::isfinite(node.number)) return false;
    if (node.unsignedInteger) return false;
    if (node.integer) { value = node.integerValue; return true; }
    if (std::floor(node.number) != node.number || node.number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) || node.number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) return false;
    value = static_cast<std::int64_t>(node.number); return true;
}
bool number_to_unsigned(const JsonNode& node, std::uint64_t& value) {
    if (node.kind == JsonNode::Kind::Number && node.unsignedInteger) {
        value = node.unsignedIntegerValue;
        return true;
    }
    std::int64_t signedValue = 0;
    if (!number_to_signed(node, signedValue) || signedValue < 0) return false;
    value = static_cast<std::uint64_t>(signedValue); return true;
}

SerializationResult read_value(const TypeRegistry& registry, TypeId typeId, const JsonNode& node,
                               void* object, const JsonOptions& options, std::size_t depth) {
    if (depth > options.maxDepth) return failure(SerializationError::DepthExceeded, "maximum deserialization depth exceeded");
    const auto* type = registry.find(typeId);
    if (!type || !object) return failure(!type ? SerializationError::InvalidType : SerializationError::NullObject, "invalid destination");
    if (type->name == "string") { if (node.kind != JsonNode::Kind::String) return failure(SerializationError::TypeMismatch, "expected string"); *static_cast<std::string*>(object) = node.string; return {}; }
    if (type->name == "bool") { if (node.kind != JsonNode::Kind::Boolean) return failure(SerializationError::TypeMismatch, "expected boolean"); *static_cast<bool*>(object) = node.boolean; return {}; }
    if (is_integer_type(type->name)) {
        if (is_unsigned_type(type->name)) {
            std::uint64_t value = 0;
            if (!number_to_unsigned(node, value)) return failure(SerializationError::RangeError, "expected unsigned integer");
            if ((type->name == "uint8" && value > std::numeric_limits<std::uint8_t>::max()) ||
                (type->name == "uint16" && value > std::numeric_limits<std::uint16_t>::max()) ||
                (type->name == "uint32" && value > std::numeric_limits<std::uint32_t>::max()))
                return failure(SerializationError::RangeError, "unsigned integer out of range");
            if (type->name == "uint8") *static_cast<std::uint8_t*>(object) = static_cast<std::uint8_t>(value);
            else if (type->name == "uint16") *static_cast<std::uint16_t*>(object) = static_cast<std::uint16_t>(value);
            else if (type->name == "uint32") *static_cast<std::uint32_t*>(object) = static_cast<std::uint32_t>(value);
            else *static_cast<std::uint64_t*>(object) = value;
        } else {
            std::int64_t value = 0;
            if (!number_to_signed(node, value)) return failure(SerializationError::RangeError, "expected signed integer");
            if ((type->name == "int8" && (value < std::numeric_limits<std::int8_t>::min() || value > std::numeric_limits<std::int8_t>::max())) ||
                (type->name == "int16" && (value < std::numeric_limits<std::int16_t>::min() || value > std::numeric_limits<std::int16_t>::max())) ||
                (type->name == "int32" && (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())) )
                return failure(SerializationError::RangeError, "signed integer out of range");
            if (type->name == "int8") *static_cast<std::int8_t*>(object) = static_cast<std::int8_t>(value);
            else if (type->name == "int16") *static_cast<std::int16_t*>(object) = static_cast<std::int16_t>(value);
            else if (type->name == "int32") *static_cast<std::int32_t*>(object) = static_cast<std::int32_t>(value);
            else *static_cast<std::int64_t*>(object) = value;
        }
        return {};
    }
    if (type->name == "float" || type->name == "double") { if (node.kind != JsonNode::Kind::Number) return failure(SerializationError::TypeMismatch, "expected number"); if (type->name == "float") *static_cast<float*>(object) = static_cast<float>(node.number); else *static_cast<double*>(object) = node.number; return {}; }
    if (type->kind == TypeKind::Enum) {
        std::int64_t value = 0;
        if (node.kind == JsonNode::Kind::String) { const auto* found = type->find_enum_value(node.string); if (!found) return failure(SerializationError::TypeMismatch, "unknown enum value"); value = found->value; }
        else if (!number_to_signed(node, value)) return failure(SerializationError::TypeMismatch, "expected enum name or integer");
        const auto* underlying = registry.find(type->underlyingType);
        if (underlying && is_unsigned_type(underlying->name)) write_enum_unsigned(object, type->size, static_cast<std::uint64_t>(value));
        else write_enum_signed(object, type->size, value);
        return {};
    }
    if (type->container) {
        if (node.kind != JsonNode::Kind::Array) return failure(SerializationError::TypeMismatch, "expected array");
        if (!type->container->resize(object, node.array.size())) return failure(SerializationError::ContainerFailure, "container resize failed");
        for (std::size_t index = 0; index < node.array.size(); ++index) {
            auto* element = type->container->mutableElement(object, index);
            auto result = read_value(registry, type->container->elementType, node.array[index], element, options, depth + 1);
            if (!result) return result;
        }
        return {};
    }
    bool hasFields = !type->fields.empty();
    if (!hasFields && (!type->bases.empty() || type->baseType != 0)) hasFields = true;
    if (node.kind != JsonNode::Kind::Object || !hasFields) return failure(SerializationError::TypeMismatch, "expected reflected object");
    std::vector<bool> seen;
    std::vector<std::pair<const FieldDescriptor*, void*>> fields;
    std::function<void(const TypeDescriptor*)> collect_fields = [&](const TypeDescriptor* current) {
        std::vector<BaseDescriptor> bases = current->bases;
        if (bases.empty() && current->baseType != 0) bases.push_back({current->baseType, current->castToBase, current->constCastToBase});
        for (const auto& base : bases) if (const auto* baseType = registry.find(base.type)) collect_fields(baseType);
        void* ownerObject = registry.cast_object(type->id, current->id, object);
        for (const auto& field : current->fields) fields.emplace_back(&field, field.address_of(ownerObject));
    };
    collect_fields(type);
    seen.assign(fields.size(), false);
    for (const auto& [key, value] : node.object) {
        std::size_t index = 0;
        for (; index < fields.size() && fields[index].first->name != key; ++index) {}
        if (index == fields.size()) { if (options.rejectUnknownFields) return failure(SerializationError::UnknownField, "unknown field: " + key); continue; }
        auto result = read_value(registry, fields[index].first->type, value, fields[index].second, options, depth + 1);
        if (!result) return result;
        seen[index] = true;
    }
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (has_flag(fields[index].first->flags, FieldFlags::Required) && !seen[index])
            return failure(SerializationError::MissingField, "required field is missing: " + fields[index].first->name);
    }
    return {};
}

} // namespace

SerializationResult serialize_json(const TypeRegistry& registry, TypeId type, const void* object,
                                   std::string& output, const JsonOptions& options) {
    output.clear();
    return write_value(registry, type, object, output, options, 0);
}

SerializationResult deserialize_json(const TypeRegistry& registry, TypeId type, std::string_view input,
                                     void* object, const JsonOptions& options) {
    JsonParser parser(input);
    JsonNode node = parser.parse();
    if (!parser.result) return parser.result;
    return read_value(registry, type, node, object, options, 0);
}

} // namespace shinkou::reflection

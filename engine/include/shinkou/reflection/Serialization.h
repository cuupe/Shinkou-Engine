#pragma once

#include "shinkou/reflection/Reflection.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace shinkou::reflection {

struct JsonOptions {
    bool pretty{false};
    bool includeTransient{false};
    bool rejectUnknownFields{false};
    std::size_t maxDepth{64};
};

enum class SerializationError : std::uint8_t {
    None,
    InvalidType,
    NullObject,
    UnsupportedType,
    InvalidJson,
    TypeMismatch,
    RangeError,
    MissingField,
    UnknownField,
    DepthExceeded,
    ContainerFailure,
};

struct SerializationResult {
    SerializationError error{SerializationError::None};
    std::size_t offset{0};
    std::string message;
    explicit operator bool() const noexcept { return error == SerializationError::None; }
};

SerializationResult serialize_json(const TypeRegistry& registry, TypeId type, const void* object,
                                   std::string& output, const JsonOptions& options = {});
SerializationResult deserialize_json(const TypeRegistry& registry, TypeId type, std::string_view input,
                                     void* object, const JsonOptions& options = {});

} // namespace shinkou::reflection

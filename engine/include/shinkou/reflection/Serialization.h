#pragma once

#include "shinkou/reflection/Reflection.h"
#include "shinkou/data/Value.h"
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
    std::size_t maxBytes{16u * 1024u * 1024u};
    std::size_t maxNodes{1000000};
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
    LimitExceeded,
};

struct SerializationResult {
    SerializationError error{SerializationError::None};
    std::size_t offset{0};
    std::string message;
    explicit operator bool() const noexcept {
        return error == SerializationError::None;
    }
};

SerializationResult serialize_json(const TypeRegistry &registry, TypeId type, const void *object,
                                   std::string &output, const JsonOptions &options = {});
SerializationResult deserialize_json(const TypeRegistry &registry, TypeId type,
                                     std::string_view input, void *object,
                                     const JsonOptions &options = {});

// Structured interchange is independent of its wire encoding. Scene,
// preferences and tools can inspect/transform a value tree without JSON text.
SerializationResult serialize_value(const TypeRegistry& registry, TypeId type, const void* object,
                                    data::Value& output, const JsonOptions& options = {});
SerializationResult deserialize_value(const TypeRegistry& registry, TypeId type, const data::Value& value,
                                      void* object, const JsonOptions& options = {});

} // namespace shinkou::reflection

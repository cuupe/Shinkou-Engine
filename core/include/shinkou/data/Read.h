#pragma once
#include "shinkou/core/Result.h"
#include "shinkou/data/Value.h"
#include <limits>
#include <type_traits>

namespace shinkou::data {
inline std::string pointer_token(std::string_view key) {
    std::string output;
    for (char ch : key) {
        if (ch == '~') output += "~0";
        else if (ch == '/') output += "~1";
        else output += ch;
    }
    return output;
}
template<class T> core::Status convert(const Value& value, T& output) {
    using core::Error; using core::ErrorCode;
    if constexpr (std::is_same_v<T,std::string>) {
        if (value.kind() != Value::Kind::String) return Error{ErrorCode::TypeMismatch,0,"expected string"};
        output = value.string_or();
    } else if constexpr (std::is_same_v<T,bool>) {
        if (value.kind() != Value::Kind::Boolean) return Error{ErrorCode::TypeMismatch,0,"expected boolean"};
        output = value.bool_or();
    } else if constexpr (std::is_integral_v<T>) {
        static_assert(sizeof(T) <= sizeof(std::uint64_t));
        if constexpr (std::is_unsigned_v<T>) {
            std::uint64_t number;
            if (!value.to_uint64(number) || number > (std::numeric_limits<T>::max)())
                return Error{ErrorCode::OutOfRange,0,"expected unsigned integer in range"};
            output = static_cast<T>(number);
        } else {
            std::int64_t number;
            if (!value.to_int64(number) || number < (std::numeric_limits<T>::min)() || number > (std::numeric_limits<T>::max)())
                return Error{ErrorCode::OutOfRange,0,"expected signed integer in range"};
            output = static_cast<T>(number);
        }
    } else {
        static_assert(std::is_floating_point_v<T>,"Value conversion supports string, bool and arithmetic types");
        if (value.kind() != Value::Kind::Number) return Error{ErrorCode::TypeMismatch,0,"expected number"};
        const auto number = value.number_or();
        if (!std::isfinite(number) || number < -(std::numeric_limits<T>::max)() || number > (std::numeric_limits<T>::max)())
            return Error{ErrorCode::OutOfRange,0,"floating point value out of range"};
        output = static_cast<T>(number);
    }
    return {};
}
template<class T> core::Status read_field(const Value& object, std::string_view name, T& output, bool required = true) {
    if (object.kind() != Value::Kind::Object) return core::Error{core::ErrorCode::TypeMismatch,0,"expected object"};
    const auto* value = object.find(name);
    if (!value) {
        if (!required) return {};
        auto error = core::Error{core::ErrorCode::NotFound,0,"required field missing"};
        error.path = "/" + pointer_token(name); return error;
    }
    auto result = convert(*value,output);
    if (!result) result.error().path = "/" + pointer_token(name);
    return result;
}
// Stages values in a caller-owned DTO and retains the first diagnostic. Missing
// optional fields preserve defaults; malformed present fields are still errors.
class ObjectReader {
public:
    explicit ObjectReader(const Value& value, std::string path = {}) : value_(value), path_(std::move(path)) {
        if (value.kind() != Value::Kind::Object) error_ = core::Error{core::ErrorCode::TypeMismatch,0,"expected object"};
    }
    template<class T> bool read(std::string_view name, T& output, bool required = false) {
        if (error_) return false;
        auto result = read_field(value_,name,output,required);
        if (!result) { error_ = result.error(); error_.path = path_ + error_.path; }
        return result.ok();
    }
    explicit operator bool() const noexcept { return error_.ok(); }
    const core::Error& error() const noexcept { return error_; }
private:
    const Value& value_;
    std::string path_;
    core::Error error_;
};
}

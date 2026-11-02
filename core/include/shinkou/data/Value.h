#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace shinkou::data {
// One compact value tree for configuration, persistence and interchange. IDs
// remain exact integers; the JSON dependency never appears in public headers.
class Value {
public:
    enum class Kind : std::uint8_t { Null, Boolean, Number, String, Array, Object };
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, std::string, Array, Object>;
    Value() noexcept = default;
    Value(std::nullptr_t) noexcept {}
    Value(bool value) : storage_(value) {}
    template<class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Value(T value) {
        if constexpr (std::is_signed_v<T>) storage_.emplace<std::int64_t>(value);
        else storage_.emplace<std::uint64_t>(value);
    }
    Value(double value) : storage_(value) {}
    Value(float value) : storage_(static_cast<double>(value)) {}
    Value(std::string value) : storage_(std::move(value)) {}
    Value(std::string_view value) : storage_(std::string(value)) {}
    Value(const char* value) : storage_(std::string(value)) {}
    Value(Array value) : storage_(std::move(value)) {}
    Value(Object value) : storage_(std::move(value)) {}
    static Value object() { return Object{}; }
    static Value array() { return Array{}; }
    Kind kind() const noexcept {
        switch (storage_.index()) {
        case 0: return Kind::Null;
        case 1: return Kind::Boolean;
        case 2: case 3: case 4: return Kind::Number;
        case 5: return Kind::String;
        case 6: return Kind::Array;
        default: return Kind::Object;
        }
    }
    bool is_integer() const noexcept { return storage_.index() == 2 || storage_.index() == 3; }
    bool is_unsigned_integer() const noexcept { return storage_.index() == 3; }
    bool bool_or(bool fallback = false) const noexcept {
        const auto* value = std::get_if<bool>(&storage_); return value ? *value : fallback;
    }
    double number_or(double fallback = 0) const noexcept {
        if (const auto* value = std::get_if<double>(&storage_)) return *value;
        if (const auto* value = std::get_if<std::int64_t>(&storage_)) return static_cast<double>(*value);
        if (const auto* value = std::get_if<std::uint64_t>(&storage_)) return static_cast<double>(*value);
        return fallback;
    }
    const std::string& string_or() const noexcept {
        const auto* value = std::get_if<std::string>(&storage_);
        static const std::string empty;
        return value ? *value : empty;
    }
    const Array& array_items() const noexcept {
        const auto* value = std::get_if<Array>(&storage_); static const Array empty; return value ? *value : empty;
    }
    Array& array_items() { return std::get<Array>(storage_); }
    const Object& object_items() const noexcept {
        const auto* value = std::get_if<Object>(&storage_); static const Object empty; return value ? *value : empty;
    }
    Object& object_items() { return std::get<Object>(storage_); }
    const Value* find(std::string_view name) const noexcept {
        if (kind() != Kind::Object) return nullptr;
        const auto& fields = std::get<Object>(storage_);
        const auto found = fields.find(name);
        return found == fields.end() ? nullptr : &found->second;
    }
    bool to_int64(std::int64_t& output) const noexcept {
        if (const auto* value = std::get_if<std::int64_t>(&storage_)) { output = *value; return true; }
        if (const auto* value = std::get_if<std::uint64_t>(&storage_)) {
            if (*value > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) return false;
            output = static_cast<std::int64_t>(*value); return true;
        }
        if (const auto* value = std::get_if<double>(&storage_)) {
            if (!std::isfinite(*value) || std::trunc(*value) != *value || *value < -9223372036854775808.0 || *value >= 9223372036854775808.0) return false;
            output = static_cast<std::int64_t>(*value); return true;
        }
        return false;
    }
    bool to_uint64(std::uint64_t& output) const noexcept {
        if (const auto* value = std::get_if<std::uint64_t>(&storage_)) { output = *value; return true; }
        if (const auto* value = std::get_if<std::int64_t>(&storage_)) {
            if (*value < 0) return false;
            output = static_cast<std::uint64_t>(*value); return true;
        }
        if (const auto* value = std::get_if<double>(&storage_)) {
            if (!std::isfinite(*value) || std::trunc(*value) != *value || *value < 0 || *value >= 18446744073709551616.0) return false;
            output = static_cast<std::uint64_t>(*value); return true;
        }
        return false;
    }
    const Storage& storage() const noexcept { return storage_; }
private:
    Storage storage_{nullptr};
};
}

#include "shinkou/data/Json.h"
#include "shinkou/data/Read.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace shinkou::data {
namespace {
using Json = nlohmann::json;
using core::Error;
using core::ErrorCode;
struct Builder : nlohmann::json_sax<Json> {
    struct Frame { Value* value; std::string key; bool hasKey{false}; };
    JsonLimits limits;
    Value root;
    bool rootWritten{false};
    std::vector<Frame> stack;
    std::size_t nodes{0};
    Error error;
    explicit Builder(JsonLimits value) : limits(value) {}
    std::string location() const {
        std::string path;
        for (std::size_t i = 1; i < stack.size(); ++i) {
            const auto& parent = *stack[i-1].value;
            const auto* child = stack[i].value;
            if (parent.kind() == Value::Kind::Object) {
                for (const auto& [key,value] : parent.object_items()) if (&value == child) {
                    path += "/" + pointer_token(key); break;
                }
            } else if (parent.kind() == Value::Kind::Array) {
                path += "/" + std::to_string(static_cast<std::size_t>(child - parent.array_items().data()));
            }
        }
        if (!stack.empty()) {
            const auto& last = stack.back();
            if (last.hasKey) path += "/" + pointer_token(last.key);
            else if (last.value->kind() == Value::Kind::Array) path += "/" + std::to_string(last.value->array_items().size());
        }
        return path;
    }
    bool fail(ErrorCode code, std::string message) {
        if (error.ok()) { error = Error{code, 0, std::move(message)}; error.path = location(); }
        return false;
    }
    Value* attach(Value value) {
        if (++nodes > limits.maxNodes || stack.size() > limits.maxDepth || stack.size() > maxDocumentDepth) {
            fail(ErrorCode::LimitExceeded, "JSON node/depth limit exceeded"); return nullptr;
        }
        if (stack.empty()) {
            if (rootWritten) { fail(ErrorCode::ParseError, "multiple JSON roots"); return nullptr; }
            root = std::move(value); rootWritten = true; return &root;
        }
        auto& parent = stack.back();
        if (parent.value->kind() == Value::Kind::Array) {
            auto& values = parent.value->array_items();
            if (values.size() >= limits.maxArrayItems) { fail(ErrorCode::LimitExceeded, "JSON array limit exceeded"); return nullptr; }
            values.push_back(std::move(value)); return &values.back();
        }
        if (!parent.hasKey) { fail(ErrorCode::ParseError, "JSON object key missing"); return nullptr; }
        auto& fields = parent.value->object_items();
        if (fields.size() >= limits.maxObjectMembers) { fail(ErrorCode::LimitExceeded, "JSON object member limit exceeded"); return nullptr; }
        auto inserted = fields.emplace(std::move(parent.key), std::move(value));
        parent.hasKey = false;
        if (!inserted.second) { fail(ErrorCode::InvalidData, "duplicate JSON object key"); return nullptr; }
        return &inserted.first->second;
    }
    bool null() override { return attach(Value{}) != nullptr; }
    bool boolean(bool value) override { return attach(Value{value}) != nullptr; }
    bool number_integer(number_integer_t value) override { return attach(Value{value}) != nullptr; }
    bool number_unsigned(number_unsigned_t value) override { return attach(Value{value}) != nullptr; }
    bool number_float(number_float_t value, const string_t& token) override {
        if (token.find_first_of(".eE") == std::string::npos)
            return fail(ErrorCode::OutOfRange, "JSON integer exceeds 64-bit range");
        if (!std::isfinite(value)) return fail(ErrorCode::OutOfRange, "non-finite JSON number");
        return attach(Value{value}) != nullptr;
    }
    bool string(string_t& value) override {
        if (value.size() > limits.maxStringBytes) return fail(ErrorCode::LimitExceeded, "JSON string limit exceeded");
        return attach(Value{std::move(value)}) != nullptr;
    }
    bool binary(binary_t&) override { return fail(ErrorCode::NotSupported, "binary values are not JSON"); }
    bool start_object(std::size_t) override {
        auto* value = attach(Value::object());
        if (!value) return false;
        stack.push_back({value, {}, false}); return true;
    }
    bool key(string_t& value) override {
        if (value.size() > limits.maxStringBytes) return fail(ErrorCode::LimitExceeded, "JSON key limit exceeded");
        auto& frame = stack.back();
        if (frame.value->find(value)) return fail(ErrorCode::InvalidData, "duplicate JSON object key: " + value);
        frame.key = std::move(value); frame.hasKey = true; return true;
    }
    bool end_object() override { stack.pop_back(); return true; }
    bool start_array(std::size_t) override {
        auto* value = attach(Value::array());
        if (!value) return false;
        stack.push_back({value, {}, false}); return true;
    }
    bool end_array() override { stack.pop_back(); return true; }
    bool parse_error(std::size_t position, const std::string&, const Json::exception& exception) override {
        fail(ErrorCode::ParseError, exception.what());
        error.offset = position ? position - 1 : 0; return false;
    }
};
struct Writer {
    JsonWriteOptions options;
    std::string output;
    Error error;
    bool append(std::string_view text) {
        if (text.size() > options.maxOutputBytes - (std::min)(options.maxOutputBytes, output.size())) {
            error = Error{ErrorCode::LimitExceeded, 0, "JSON output limit exceeded"}; return false;
        }
        output.append(text); return true;
    }
    bool line(std::size_t depth) {
        if (!options.pretty) return true;
        if (!append("\n")) return false;
        for (std::size_t i = 0; i < depth; ++i) if (!append("  ")) return false;
        return true;
    }
    bool write(const Value& value, std::size_t depth) {
        if (depth > options.maxDepth || depth > maxDocumentDepth) { error = Error{ErrorCode::LimitExceeded, 0, "JSON write depth exceeded"}; return false; }
        switch (value.kind()) {
        case Value::Kind::Null: return append("null");
        case Value::Kind::Boolean: return append(value.bool_or() ? "true" : "false");
        case Value::Kind::String: return append(quote_json(value.string_or()));
        case Value::Kind::Number: {
            char buffer[64];
            std::to_chars_result result;
            if (const auto* number = std::get_if<std::int64_t>(&value.storage())) result = std::to_chars(buffer, buffer + sizeof(buffer), *number);
            else if (const auto* number = std::get_if<std::uint64_t>(&value.storage())) result = std::to_chars(buffer, buffer + sizeof(buffer), *number);
            else {
                const auto floating = std::get<double>(value.storage());
                if (!std::isfinite(floating)) { error = Error{ErrorCode::InvalidData, 0, "non-finite JSON number"}; return false; }
                result = std::to_chars(buffer, buffer + sizeof(buffer), floating, std::chars_format::general, std::numeric_limits<double>::max_digits10);
            }
            if (result.ec != std::errc{}) { error = Error{ErrorCode::OutOfRange, 0, "JSON number conversion failed"}; return false; }
            return append({buffer, static_cast<std::size_t>(result.ptr - buffer)});
        }
        case Value::Kind::Array: {
            if (!append("[")) return false;
            bool first = true;
            for (const auto& item : value.array_items()) {
                if ((!first && !append(",")) || !line(depth + 1) || !write(item, depth + 1)) return false;
                first = false;
            }
            return (first || line(depth)) && append("]");
        }
        case Value::Kind::Object: {
            if (!append("{")) return false;
            bool first = true;
            for (const auto& [key, item] : value.object_items()) {
                if ((!first && !append(",")) || !line(depth + 1) || !append(quote_json(key)) ||
                    !append(options.pretty ? ": " : ":") || !write(item, depth + 1)) return false;
                first = false;
            }
            return (first || line(depth)) && append("}");
        }
        }
        return false;
    }
};
}
core::Result<Value> parse_json(std::string_view input, const JsonLimits& limits) {
    if (input.size() > limits.maxInputBytes) return Error{ErrorCode::LimitExceeded, 0, "JSON input limit exceeded"};
    try {
        Builder builder(limits);
        if (!Json::sax_parse(input.begin(), input.end(), &builder)) {
            if (builder.error.ok()) builder.error = Error{ErrorCode::ParseError, 0, "invalid JSON"};
            return std::move(builder.error);
        }
        return std::move(builder.root);
    } catch (const std::bad_alloc&) { return Error{ErrorCode::OutOfMemory}; }
    catch (const std::exception& error) { return Error{ErrorCode::ParseError, 0, error.what()}; }
}
core::Status parse_json(std::string_view input, Value& output, const JsonLimits& limits) {
    auto parsed = parse_json(input, limits);
    if (!parsed) return parsed.error();
    output = std::move(parsed).value(); return {};
}
core::Result<std::string> write_json(const Value& value, const JsonWriteOptions& options) {
    try {
        Writer writer{options, {}, {}};
        if (!writer.write(value, 0)) return std::move(writer.error);
        return std::move(writer.output);
    } catch (const std::bad_alloc&) { return Error{ErrorCode::OutOfMemory}; }
    catch (const std::exception& error) { return Error{ErrorCode::InvalidData, 0, error.what()}; }
}
std::string quote_json(std::string_view text) { return Json(std::string(text)).dump(); }
std::string escape_json(std::string_view text) {
    auto quoted = quote_json(text); return quoted.substr(1, quoted.size() - 2);
}
void quote_json(std::ostream& output, std::string_view text) { output << quote_json(text); }
core::Status validate_document(const Value& root, DocumentSchema schema) {
    if (root.kind() != Value::Kind::Object) return Error{ErrorCode::TypeMismatch, 0, "document must be an object"};
    const auto* name = root.find("schema");
    const auto* version = root.find("version");
    std::uint64_t number = 0;
    if (!name || name->kind() != Value::Kind::String || name->string_or() != schema.name)
        return Error{ErrorCode::InvalidData, 0, "unsupported document schema"};
    if (!version || !version->is_integer() || !version->to_uint64(number) || number != schema.version)
        return Error{ErrorCode::InvalidData, 0, "unsupported document version"};
    return {};
}
Value document(DocumentSchema schema) {
    auto value = Value::object();
    value.object_items().emplace("schema", Value{schema.name});
    value.object_items().emplace("version", Value{schema.version}); return value;
}
}

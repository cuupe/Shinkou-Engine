#pragma once
#include "shinkou/core/Result.h"
#include "shinkou/data/Value.h"
#include <cstddef>
#include <ostream>

namespace shinkou::data {
inline constexpr std::size_t maxDocumentDepth = 256;
struct JsonLimits {
    std::size_t maxInputBytes{16u * 1024u * 1024u};
    std::size_t maxDepth{64};
    std::size_t maxNodes{1000000};
    std::size_t maxStringBytes{1u * 1024u * 1024u};
    std::size_t maxArrayItems{500000};
    std::size_t maxObjectMembers{65536};
};
struct JsonWriteOptions {
    bool pretty{false};
    std::size_t maxDepth{64};
    std::size_t maxOutputBytes{16u * 1024u * 1024u};
};
core::Result<Value> parse_json(std::string_view input, const JsonLimits& limits = {});
// Transactional: output changes only after complete successful validation.
core::Status parse_json(std::string_view input, Value& output, const JsonLimits& limits = {});
core::Result<std::string> write_json(const Value& value, const JsonWriteOptions& options = {});
std::string quote_json(std::string_view value);
std::string escape_json(std::string_view value);
void quote_json(std::ostream& output, std::string_view value);

// Shared compatibility facade for existing domain readers. New APIs should use
// Result<Value>; there is still only one parser and one error representation.
class JsonParser {
public:
    explicit JsonParser(std::string_view input, JsonLimits limits = {}) : input_(input), limits_(limits) {}
    bool parse(Value& value, std::string& error) {
        auto result = parse_json(input_, value, limits_);
        error_ = result ? core::Error{} : result.error();
        error = error_.ok() ? std::string{} : error_.describe();
        return result.ok();
    }
    bool parse(Value& value) { std::string ignored; return parse(value, ignored); }
    std::string error() const { return error_.ok() ? std::string{} : error_.describe(); }
    const core::Error& diagnostic() const noexcept { return error_; }
private:
    std::string_view input_;
    JsonLimits limits_;
    core::Error error_;
};
struct DocumentSchema { std::string_view name; std::uint64_t version{1}; };
core::Status validate_document(const Value& root, DocumentSchema schema);
Value document(DocumentSchema schema);
}

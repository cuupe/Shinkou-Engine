#include "shinkou/core/Handle.h"
#include "shinkou/core/Hash.h"
#include "shinkou/core/Path.h"
#include "shinkou/data/Json.h"
#include "shinkou/data/Read.h"
#include <locale>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace shinkou;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void values() {
    const auto parsed = data::parse_json(R"({"a":18446744073709551615,"b":-9223372036854775808,"s":"\uD83D\uDE00","list":[true,null,1.25]})");
    check(parsed.ok(), "valid value tree rejected");
    std::uint64_t u = 0; std::int64_t i = 0;
    check(parsed->find("a")->to_uint64(u) && u == (std::numeric_limits<std::uint64_t>::max)(), "uint64 lost precision");
    check(!parsed->find("a")->to_int64(i), "overflowing signed conversion accepted");
    check(parsed->find("b")->to_int64(i) && i == (std::numeric_limits<std::int64_t>::min)(), "int64 minimum lost");
    check(parsed->find("s")->string_or() == "\xf0\x9f\x98\x80", "surrogate pair decoded incorrectly");
    auto written = data::write_json(*parsed, {true});
    check(written.ok(), "tree writer failed");
    auto again = data::parse_json(*written);
    check(again.ok() && again->find("a")->to_uint64(u) && u == (std::numeric_limits<std::uint64_t>::max)(), "integer round trip failed");
    const char* bad[] = {"", "{} trailing", "[1,]", "{\"x\":1,\"x\":2}", "01", "NaN", "1e9999", "18446744073709551616", "-9223372036854775809", R"("\uD800")", R"("\uDC00")", "[true false]"};
    for (auto text : bad) check(!data::parse_json(text), "invalid JSON accepted");
    const std::string invalidUtf8{'"', static_cast<char>(0xc0), static_cast<char>(0x80), '"'};
    check(!data::parse_json(invalidUtf8), "invalid UTF8 accepted");
    auto before = data::document({"unit", 1});
    check(!data::parse_json("{", before), "invalid JSON did not fail");
    check(data::validate_document(before, {"unit", 1}).ok(), "failed parse mutated caller output");
    check(!data::validate_document(before, {"unit", 2}), "unsupported version accepted");
    std::string control = "a\n\t\r\"\\"; control.push_back('\0');
    auto quoted = data::parse_json(data::quote_json(control));
    check(quoted.ok() && quoted->string_or() == control, "string escaping failed");
    check(!data::write_json(data::Value{(std::numeric_limits<double>::infinity)()}), "nonfinite output accepted");
}
void limits() {
    data::JsonLimits limits;
    limits.maxDepth = 2;
    check(data::parse_json("[[0]]", limits).ok(), "valid depth rejected");
    check(!data::parse_json("[[[0]]]", limits), "depth limit ignored");
    limits = {}; limits.maxNodes = 3;
    check(!data::parse_json("[1,2,3]", limits), "node budget ignored");
    limits = {}; limits.maxObjectMembers = 1;
    check(!data::parse_json("{\"a\":1,\"b\":2}", limits), "object budget ignored");
    limits = {}; limits.maxArrayItems = 1;
    check(!data::parse_json("[1,2]", limits), "array budget ignored");
    limits = {}; limits.maxStringBytes = 1;
    check(!data::parse_json("\"ab\"", limits), "string budget ignored");
    limits = {}; limits.maxInputBytes = 1;
    check(!data::parse_json("[]", limits), "input budget ignored");
    auto failed = data::parse_json("{\"x\":}");
    check(!failed && failed.error().offset > 0, "parse diagnostic has no location");
    check(!data::write_json(data::Value{std::string(100, 'a')}, {false,64,10}), "output budget ignored");
    auto nested = data::parse_json("{\"outer\":{\"value\":}}");
    check(!nested && nested.error().path == "/outer/value", "nested diagnostic path was lost");
    limits = {}; limits.maxDepth = 100000;
    check(!data::parse_json(std::string(257,'[')+"0"+std::string(257,']'),limits), "hard depth limit ignored");
}
void checked_readers() {
    auto value = data::parse_json("{\"count\":18446744073709551615,\"enabled\":true,\"name\":\"value\"}");
    check(value.ok(),"reader fixture failed");
    std::uint64_t count=0;
    check(data::read_field(*value,"count",count).ok() && count == (std::numeric_limits<std::uint64_t>::max)(),"reader lost uint64 precision");
    std::uint32_t narrow=77;
    auto overflow = data::read_field(*value,"count",narrow);
    check(!overflow && narrow == 77 && overflow.error().path == "/count","failed field conversion mutated output");
    bool enabled=false;
    data::ObjectReader reader(*value,"/settings");
    reader.read("enabled",enabled);
    reader.read("name",enabled);
    check(!reader && reader.error().path == "/settings/name" && enabled,"reader did not retain first field error");
    auto missing = data::read_field(*value,"a/b~c",enabled);
    check(!missing && missing.error().path == "/a~1b~0c","JSON pointer escaping failed");
    struct Comma : std::numpunct<char> { char do_decimal_point() const override { return ','; } };
    const auto old = std::locale();
    std::locale::global(std::locale(old,new Comma));
    auto text = data::write_json(data::Value{1.25});
    std::locale::global(old);
    check(text.ok() && *text == "1.25","JSON number depended on process locale");
}
void identities_and_results() {
    struct Tag;
    using Handle = core::GenerationalHandle<Tag>;
    check(!Handle{1,0} && static_cast<bool>(Handle{1,1}), "generation validity broken");
    check(Handle{1,2} != Handle{1,3}, "generations alias");
    using Codec = core::PackedHandleCodec<std::uint32_t, 16>;
    const auto handle = Codec::pack(12, 34);
    check(Codec::index(handle) == 12 && Codec::generation(handle) == 34, "packed identity round trip failed");
    check(Codec::index(0) == Codec::indexMask, "invalid packed index changed");
    check(core::hash_string("hello") == 0xa430d84680aabd0bull, "FNV reference value failed");
    core::Result<std::unique_ptr<int>> result{std::make_unique<int>(42)};
    check(result.ok() && **result == 42, "move-only result failed");
    core::Result<int> error{core::make_error(core::ErrorCode::NotFound, 2, "missing")};
    check(!error && error.error().native_code == 2, "error/native code lost");
    core::Error first;
    core::set_error_if_empty(first, core::ErrorCode::NotFound);
    core::set_error_if_empty(first, core::ErrorCode::Unknown);
    check(first.code == core::ErrorCode::NotFound, "cause was overwritten");
    check(core::Status{}.ok(), "void success failed");
    const std::string unicode = "assets/\xe5\xad\x97\xe4\xbd\x93/\xf0\x9f\x98\x80.ttf";
    check(core::path_to_utf8(core::path_from_utf8(unicode)) == unicode, "UTF8 filesystem boundary lost characters");
}
}
int main() {
    try { values(); limits(); checked_readers(); identities_and_results(); std::cout << "core contracts passed; Value bytes=" << sizeof(data::Value) << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

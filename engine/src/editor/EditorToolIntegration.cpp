#include "shinkou/editor/EditorToolIntegration.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <map>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace shinkou::editor {
namespace {

constexpr std::size_t kMaxProfileJsonBytes = 1u * 1024u * 1024u;
constexpr std::size_t kMaxProfileStringBytes = 16u * 1024u;
constexpr std::size_t kMaxProfileArguments = 128;
constexpr std::size_t kMaxProfileArgumentBytes = 16u * 1024u;
constexpr std::size_t kMaxProfileSetEntries = 32;
constexpr std::size_t kMaxJsonDepth = 16;
constexpr std::size_t kMaxJsonMembers = 64;

struct JsonValue {
    enum class Kind : std::uint8_t { Null, Boolean, Number, String, Array, Object };
    Kind kind{Kind::Null};
    std::int64_t number{0};
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue, std::less<>> object;
};

std::string json_error(std::string message) {
    return "build profile JSON: " + std::move(message);
}

class JsonParser final {
    std::string_view input_;
    std::size_t position_{0};
    std::string error_;

    void whitespace() noexcept {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\r' || input_[position_] == '\n')) ++position_;
    }

    bool fail(std::string message) {
        if (error_.empty()) error_ = json_error(std::move(message));
        return false;
    }

    bool parse_hex4(std::uint32_t& value) {
        if (position_ + 4 > input_.size()) return fail("truncated unicode escape");
        value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const auto character = input_[position_++];
            value <<= 4;
            if (character >= '0' && character <= '9') value += static_cast<std::uint32_t>(character - '0');
            else if (character >= 'a' && character <= 'f') value += static_cast<std::uint32_t>(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F') value += static_cast<std::uint32_t>(character - 'A' + 10);
            else return fail("invalid unicode escape");
        }
        return true;
    }

    static void append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7fu) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ffu) {
            output.push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else if (codepoint <= 0xffffu) {
            output.push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else {
            output.push_back(static_cast<char>(0xf0u | (codepoint >> 18u)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
    }

    bool parse_string(std::string& output) {
        if (position_ >= input_.size() || input_[position_] != '"') return fail("string expected");
        ++position_;
        output.clear();
        output.reserve(std::min<std::size_t>(input_.size() - position_, kMaxProfileStringBytes));
        while (position_ < input_.size()) {
            const auto character = input_[position_++];
            if (character == '"') {
                if (output.size() > kMaxProfileStringBytes) return fail("string is too large");
                return true;
            }
            if (static_cast<unsigned char>(character) < 0x20u) return fail("control character in string");
            if (character != '\\') {
                output.push_back(character);
                if (output.size() > kMaxProfileStringBytes) return fail("string is too large");
                continue;
            }
            if (position_ >= input_.size()) return fail("truncated string escape");
            switch (input_[position_++]) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t codepoint = 0;
                if (!parse_hex4(codepoint)) return false;
                if (codepoint >= 0xd800u && codepoint <= 0xdfffu)
                    return fail("surrogate unicode escapes are not supported");
                append_utf8(output, codepoint);
                break;
            }
            default: return fail("unknown string escape");
            }
            if (output.size() > kMaxProfileStringBytes) return fail("string is too large");
        }
        return fail("unterminated string");
    }

    bool parse_number(JsonValue& output) {
        const auto begin = position_;
        if (input_[position_] == '-') ++position_;
        const auto digits = position_;
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        if (digits == position_) return fail("invalid number");
        if (position_ < input_.size() &&
            (input_[position_] == '.' || input_[position_] == 'e' || input_[position_] == 'E'))
            return fail("profile numbers must be integers");
        std::int64_t value = 0;
        const auto parsed = std::from_chars(input_.data() + begin, input_.data() + position_, value);
        if (parsed.ec != std::errc{} || parsed.ptr != input_.data() + position_)
            return fail("number is out of range");
        output.kind = JsonValue::Kind::Number;
        output.number = value;
        return true;
    }

    bool parse_value(JsonValue& output, std::size_t depth) {
        if (depth > kMaxJsonDepth) return fail("nesting is too deep");
        whitespace();
        if (position_ >= input_.size()) return fail("value expected");
        switch (input_[position_]) {
        case '"': output.kind = JsonValue::Kind::String; return parse_string(output.string);
        case '{': return parse_object(output, depth + 1);
        case '[': return parse_array(output, depth + 1);
        case 't':
            if (input_.substr(position_, 4) == "true") { position_ += 4; output.kind = JsonValue::Kind::Boolean; return true; }
            return fail("invalid literal");
        case 'f':
            if (input_.substr(position_, 5) == "false") { position_ += 5; output.kind = JsonValue::Kind::Boolean; return true; }
            return fail("invalid literal");
        case 'n':
            if (input_.substr(position_, 4) == "null") { position_ += 4; output.kind = JsonValue::Kind::Null; return true; }
            return fail("invalid literal");
        default:
            if (input_[position_] == '-' || (input_[position_] >= '0' && input_[position_] <= '9'))
                return parse_number(output);
            return fail("unknown value");
        }
    }

    bool parse_array(JsonValue& output, std::size_t depth) {
        ++position_;
        output.kind = JsonValue::Kind::Array;
        whitespace();
        if (position_ < input_.size() && input_[position_] == ']') { ++position_; return true; }
        while (position_ < input_.size()) {
            if (output.array.size() >= kMaxProfileArguments) return fail("array has too many entries");
            JsonValue value;
            if (!parse_value(value, depth)) return false;
            output.array.push_back(std::move(value));
            whitespace();
            if (position_ >= input_.size()) return fail("unterminated array");
            if (input_[position_] == ']') { ++position_; return true; }
            if (input_[position_] != ',') return fail("array separator expected");
            ++position_;
            whitespace();
        }
        return fail("unterminated array");
    }

    bool parse_object(JsonValue& output, std::size_t depth) {
        ++position_;
        output.kind = JsonValue::Kind::Object;
        whitespace();
        if (position_ < input_.size() && input_[position_] == '}') { ++position_; return true; }
        while (position_ < input_.size()) {
            if (output.object.size() >= kMaxJsonMembers) return fail("object has too many members");
            std::string key;
            if (!parse_string(key)) return false;
            if (!output.object.emplace(key, JsonValue{}).second) return fail("duplicate object member");
            whitespace();
            if (position_ >= input_.size() || input_[position_] != ':') return fail("object colon expected");
            ++position_;
            JsonValue value;
            if (!parse_value(value, depth)) return false;
            output.object[key] = std::move(value);
            whitespace();
            if (position_ >= input_.size()) return fail("unterminated object");
            if (input_[position_] == '}') { ++position_; return true; }
            if (input_[position_] != ',') return fail("object separator expected");
            ++position_;
            whitespace();
        }
        return fail("unterminated object");
    }

public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool parse(JsonValue& output) {
        if (input_.size() > kMaxProfileJsonBytes) return fail("input is too large");
        if (!parse_value(output, 0)) return false;
        whitespace();
        if (position_ != input_.size()) return fail("trailing data");
        return true;
    }

    const std::string& error() const noexcept { return error_; }
};

const JsonValue* member(const JsonValue& value, std::string_view name) {
    if (value.kind != JsonValue::Kind::Object) return nullptr;
    const auto found = value.object.find(name);
    return found == value.object.end() ? nullptr : &found->second;
}

bool read_string(const JsonValue& object, std::string_view name, std::string& output,
                 bool required, std::string& error) {
    const auto* value = member(object, name);
    if (!value) {
        if (required) { error = json_error("missing string member: " + std::string(name)); return false; }
        output.clear();
        return true;
    }
    if (value->kind != JsonValue::Kind::String || value->string.find('\0') != std::string::npos) {
        error = json_error("invalid string member: " + std::string(name));
        return false;
    }
    output = value->string;
    return true;
}

bool safe_relative_path(std::string_view value, std::filesystem::path& output, bool allowEmpty) {
    if (value.empty()) { output.clear(); return allowEmpty; }
    std::filesystem::path path{std::string(value)};
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    const auto normalized = path.lexically_normal();
    if (normalized.empty() || (normalized == "." && value != ".")) return false;
    const auto generic = normalized.generic_string();
    if (generic == ".." || generic.rfind("../", 0) == 0) return false;
    output = normalized;
    return true;
}

std::string json_escape(std::string_view value) {
    std::string output;
    output.reserve(value.size() + 2);
    output.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (const auto character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20u) {
                output += "\\u00";
                output.push_back(hex[(static_cast<unsigned char>(character) >> 4u) & 0xfu]);
                output.push_back(hex[static_cast<unsigned char>(character) & 0xfu]);
            } else output.push_back(character);
            break;
        }
    }
    output.push_back('"');
    return output;
}

std::string tool_kind_name(EditorToolKind kind) {
    return std::string(EditorBuildSystem::tool_name(kind));
}

bool parse_tool_kind(std::string_view value, EditorToolKind& output) {
    constexpr EditorToolKind kinds[] = {
        EditorToolKind::CMake, EditorToolKind::Ninja, EditorToolKind::MSBuild,
        EditorToolKind::Clang, EditorToolKind::ClangCl, EditorToolKind::GCC,
        EditorToolKind::GXX, EditorToolKind::DotNet,
    };
    for (const auto kind : kinds) {
        if (value == EditorBuildSystem::tool_name(kind)) { output = kind; return true; }
    }
    return false;
}

bool read_arguments(const JsonValue& object, std::string_view name,
                    std::vector<std::string>& output, std::string& error) {
    const auto* value = member(object, name);
    if (!value) { output.clear(); return true; }
    if (value->kind != JsonValue::Kind::Array || value->array.size() > kMaxProfileArguments) {
        error = json_error("invalid argument array: " + std::string(name));
        return false;
    }
    output.clear();
    output.reserve(value->array.size());
    for (const auto& item : value->array) {
        if (item.kind != JsonValue::Kind::String || item.string.find('\0') != std::string::npos ||
            item.string.size() > kMaxProfileArgumentBytes) {
            error = json_error("invalid argument in: " + std::string(name));
            return false;
        }
        output.push_back(item.string);
    }
    return true;
}

bool contains_nul_or_too_large(std::string_view value) {
    return value.find('\0') != std::string_view::npos || value.size() > kMaxProfileStringBytes;
}

std::string path_string(const std::filesystem::path& value) {
    return value.generic_string();
}

std::string profile_json_string(const EditorBuildProfile& profile, std::string* error,
                               bool includeVersion = true) {
    const auto fields = std::array<std::pair<std::string_view, std::string>, 9>{
        std::pair{"id", profile.id}, std::pair{"name", profile.name},
        std::pair{"sourceDirectory", path_string(profile.sourceDirectory)},
        std::pair{"buildDirectory", path_string(profile.buildDirectory)},
        std::pair{"projectFile", path_string(profile.projectFile)},
        std::pair{"generator", profile.generator}, std::pair{"configuration", profile.configuration},
        std::pair{"target", profile.target}, std::pair{"configureTool", tool_kind_name(profile.configureTool)},
    };
    if (profile.id.empty() || profile.name.empty()) {
        if (error) *error = json_error("profile id and name must not be empty");
        return {};
    }
    for (const auto& [name, value] : fields) {
        if (contains_nul_or_too_large(value)) {
            if (error) *error = json_error("profile member is empty, contains NUL, or is too large: " + std::string(name));
            return {};
        }
    }
    if (contains_nul_or_too_large(tool_kind_name(profile.buildTool))) {
        if (error) *error = json_error("invalid build tool");
        return {};
    }
    const auto validate_path = [&](const std::filesystem::path& value, bool allowEmpty, std::string_view name) {
        const auto text = path_string(value);
        std::filesystem::path ignored;
        if (contains_nul_or_too_large(text) || !safe_relative_path(text, ignored, allowEmpty)) {
            if (error) *error = json_error("profile path is not project-relative: " + std::string(name));
            return false;
        }
        return true;
    };
    if (!validate_path(profile.sourceDirectory, false, "sourceDirectory") ||
        !validate_path(profile.buildDirectory, false, "buildDirectory") ||
        !validate_path(profile.projectFile, true, "projectFile")) return {};
    const auto validate_args = [&](const std::vector<std::string>& args, std::string_view name) {
        if (args.size() > kMaxProfileArguments) {
            if (error) *error = json_error("too many arguments: " + std::string(name));
            return false;
        }
        for (const auto& argument : args) {
            if (argument.find('\0') != std::string::npos || argument.size() > kMaxProfileArgumentBytes) {
                if (error) *error = json_error("argument is invalid: " + std::string(name));
                return false;
            }
        }
        return true;
    };
    if (!validate_args(profile.configureArguments, "configureArguments") ||
        !validate_args(profile.buildArguments, "buildArguments")) return {};

    std::string json = "{\n";
    if (includeVersion) json += "  \"version\": 1,\n";
    const auto field = [&json](std::string_view name, std::string_view value, bool comma = true) {
        json += "  "; json += json_escape(name); json += ": "; json += json_escape(value);
        json += comma ? ",\n" : "\n";
    };
    field("id", profile.id);
    field("name", profile.name);
    field("configureTool", tool_kind_name(profile.configureTool));
    field("buildTool", tool_kind_name(profile.buildTool));
    field("sourceDirectory", path_string(profile.sourceDirectory));
    field("buildDirectory", path_string(profile.buildDirectory));
    field("projectFile", path_string(profile.projectFile));
    field("generator", profile.generator);
    field("configuration", profile.configuration);
    field("target", profile.target);
    const auto array = [&json](std::string_view name, const std::vector<std::string>& values, bool comma) {
        json += "  "; json += json_escape(name); json += ": [";
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index != 0) json += ", ";
            json += json_escape(values[index]);
        }
        json += "]";
        json += comma ? ",\n" : "\n";
    };
    array("configureArguments", profile.configureArguments, true);
    array("buildArguments", profile.buildArguments, false);
    json += "}\n";
    return json;
}

std::string ascii_lower(std::string value) {
    for (auto& character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

std::filesystem::path normalize_root(std::filesystem::path root) {
    std::error_code error;
    if (root.empty()) root = std::filesystem::current_path(error);
    const auto absolute = std::filesystem::absolute(root, error);
    return (error ? root : absolute).lexically_normal();
}

bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const auto base = ascii_lower(root.lexically_normal().generic_string());
    const auto value = ascii_lower(candidate.lexically_normal().generic_string());
    return !base.empty() && (value == base || value.rfind(base + '/', 0) == 0);
}

std::filesystem::path resolve_project_path(const std::filesystem::path& root,
                                           const std::filesystem::path& input) {
    if (input.empty()) return {};
    if (input.is_absolute()) return is_within(root, input) ? input.lexically_normal() : std::filesystem::path{};
    const auto candidate = (root / input).lexically_normal();
    return is_within(root, candidate) ? candidate : std::filesystem::path{};
}

char path_separator() noexcept {
#if defined(_WIN32)
    return ';';
#else
    return ':';
#endif
}

std::vector<std::filesystem::path> split_search_path(std::string_view value) {
    std::vector<std::filesystem::path> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(path_separator(), begin);
        const auto part = value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin);
        if (!part.empty()) result.emplace_back(std::string(part));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return result;
}

struct IdeCandidate {
    EditorIdeKind kind;
    const char* name;
    const char* const* executableNames;
    std::size_t executableNameCount;
};

constexpr const char* kVisualStudioNames[] = {"devenv.com", "devenv.exe", "devenv"};
constexpr const char* kRiderNames[] = {"rider64.exe", "rider64", "rider"};
// Do not discover the code.cmd shim: launching it would require going through
// a shell, which would break the explicit executable/argument safety boundary.
constexpr const char* kCodeNames[] = {"code.exe", "Code.exe", "code"};
constexpr const char* kClangdNames[] = {"clangd.exe", "clangd"};
constexpr IdeCandidate kIdeCandidates[] = {
    {EditorIdeKind::VisualStudio, "Visual Studio", kVisualStudioNames, std::size(kVisualStudioNames)},
    {EditorIdeKind::Rider, "Rider", kRiderNames, std::size(kRiderNames)},
    {EditorIdeKind::VisualStudioCode, "VS Code", kCodeNames, std::size(kCodeNames)},
    {EditorIdeKind::Clangd, "clangd", kClangdNames, std::size(kClangdNames)},
};

std::filesystem::path find_ide_executable(const std::vector<std::filesystem::path>& directories,
                                          const IdeCandidate& candidate) {
    for (const auto& directory : directories) {
        if (directory.empty()) continue;
        for (std::size_t index = 0; index < candidate.executableNameCount; ++index) {
            const auto path = (directory / candidate.executableNames[index]).lexically_normal();
            std::error_code error;
            if (std::filesystem::is_regular_file(path, error) && !error) return path;
        }
    }
    return {};
}

const EditorIdeDescriptor* find_ide(const std::vector<EditorIdeDescriptor>& ides, EditorIdeKind kind) {
    const auto found = std::find_if(ides.begin(), ides.end(), [kind](const auto& value) { return value.kind == kind; });
    return found == ides.end() ? nullptr : &*found;
}

#if defined(_WIN32)
std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty() || value.size() > static_cast<std::size_t>(INT_MAX)) return {};
    const auto length = static_cast<int>(value.size());
    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (required <= 0) required = MultiByteToWideChar(CP_ACP, 0, value.data(), length, nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, result.data(), required) <= 0 &&
        MultiByteToWideChar(CP_ACP, 0, value.data(), length, result.data(), required) <= 0) return {};
    return result;
}

std::wstring quote_windows_argument(std::string_view value) {
    const auto wide = utf8_to_wide(value);
    if (wide.empty()) return L"\"\"";
    if (wide.find_first_of(L" \t\"") == std::wstring::npos) return wide;
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : wide) {
        if (character == L'\\') ++slashes;
        else if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'"');
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            result.push_back(character);
            slashes = 0;
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}
#endif

} // namespace

bool EditorBuildProfileStore::serialize(const EditorBuildProfile& profile, std::string& json,
                                        std::string* error) {
    json = profile_json_string(profile, error);
    return !json.empty();
}

EditorBuildProfileLoadResult EditorBuildProfileStore::deserialize(std::string_view json) {
    EditorBuildProfileLoadResult result;
    JsonValue root;
    JsonParser parser(json);
    if (!parser.parse(root)) { result.error = parser.error(); return result; }
    if (root.kind != JsonValue::Kind::Object) { result.error = json_error("root must be an object"); return result; }
    const auto* version = member(root, "version");
    if (!version || version->kind != JsonValue::Kind::Number || version->number != CurrentVersion) {
        result.error = json_error("unsupported or missing version"); return result;
    }
    auto& profile = result.profile;
    std::string sourceDirectory;
    std::string buildDirectory;
    std::string projectFile;
    if (!read_string(root, "id", profile.id, true, result.error) ||
        !read_string(root, "name", profile.name, true, result.error) ||
        !read_string(root, "sourceDirectory", sourceDirectory, true, result.error) ||
        !read_string(root, "buildDirectory", buildDirectory, true, result.error) ||
        !read_string(root, "projectFile", projectFile, false, result.error) ||
        !read_string(root, "generator", profile.generator, false, result.error) ||
        !read_string(root, "configuration", profile.configuration, false, result.error) ||
        !read_string(root, "target", profile.target, false, result.error)) return result;
    std::string configureTool;
    std::string buildTool;
    if (!read_string(root, "configureTool", configureTool, true, result.error) ||
        !read_string(root, "buildTool", buildTool, true, result.error) ||
        !parse_tool_kind(configureTool, profile.configureTool) ||
        !parse_tool_kind(buildTool, profile.buildTool)) {
        result.error = json_error("unknown configure/build tool"); return result;
    }
    if (!safe_relative_path(sourceDirectory, profile.sourceDirectory, false) ||
        !safe_relative_path(buildDirectory, profile.buildDirectory, false) ||
        !safe_relative_path(projectFile, profile.projectFile, true)) {
        result.error = json_error("profile paths must be project-relative"); return result;
    }
    if (!read_arguments(root, "configureArguments", profile.configureArguments, result.error) ||
        !read_arguments(root, "buildArguments", profile.buildArguments, result.error)) return result;
    if (profile.id.empty() || profile.name.empty() || contains_nul_or_too_large(profile.id) ||
        contains_nul_or_too_large(profile.name) || contains_nul_or_too_large(profile.generator) ||
        contains_nul_or_too_large(profile.configuration) || contains_nul_or_too_large(profile.target)) {
        result.error = json_error("profile identity or string is invalid"); return result;
    }
    result.valid = true;
    return result;
}

namespace {

EditorBuildProfileLoadResult parse_profile_value(const JsonValue& root) {
    EditorBuildProfileLoadResult result;
    if (root.kind != JsonValue::Kind::Object) {
        result.error = json_error("profile entry must be an object"); return result;
    }
    if (const auto* version = member(root, "version")) {
        if (version->kind != JsonValue::Kind::Number || version->number != EditorBuildProfileStore::CurrentVersion) {
            result.error = json_error("unsupported profile entry version"); return result;
        }
    }
    auto& profile = result.profile;
    std::string sourceDirectory;
    std::string buildDirectory;
    std::string projectFile;
    if (!read_string(root, "id", profile.id, true, result.error) ||
        !read_string(root, "name", profile.name, true, result.error) ||
        !read_string(root, "sourceDirectory", sourceDirectory, true, result.error) ||
        !read_string(root, "buildDirectory", buildDirectory, true, result.error) ||
        !read_string(root, "projectFile", projectFile, false, result.error) ||
        !read_string(root, "generator", profile.generator, false, result.error) ||
        !read_string(root, "configuration", profile.configuration, false, result.error) ||
        !read_string(root, "target", profile.target, false, result.error)) return result;
    std::string configureTool;
    std::string buildTool;
    if (!read_string(root, "configureTool", configureTool, true, result.error) ||
        !read_string(root, "buildTool", buildTool, true, result.error) ||
        !parse_tool_kind(configureTool, profile.configureTool) ||
        !parse_tool_kind(buildTool, profile.buildTool)) {
        result.error = json_error("unknown configure/build tool"); return result;
    }
    if (!safe_relative_path(sourceDirectory, profile.sourceDirectory, false) ||
        !safe_relative_path(buildDirectory, profile.buildDirectory, false) ||
        !safe_relative_path(projectFile, profile.projectFile, true)) {
        result.error = json_error("profile paths must be project-relative"); return result;
    }
    if (!read_arguments(root, "configureArguments", profile.configureArguments, result.error) ||
        !read_arguments(root, "buildArguments", profile.buildArguments, result.error)) return result;
    if (profile.id.empty() || profile.name.empty() || contains_nul_or_too_large(profile.id) ||
        contains_nul_or_too_large(profile.name) || contains_nul_or_too_large(profile.generator) ||
        contains_nul_or_too_large(profile.configuration) || contains_nul_or_too_large(profile.target)) {
        result.error = json_error("profile identity or string is invalid"); return result;
    }
    result.valid = true;
    return result;
}

} // namespace

EditorBuildProfileSetLoadResult EditorBuildProfileStore::deserialize_set(std::string_view json) {
    EditorBuildProfileSetLoadResult result;
    JsonValue root;
    JsonParser parser(json);
    if (!parser.parse(root)) { result.error = parser.error(); return result; }
    if (root.kind != JsonValue::Kind::Object) { result.error = json_error("root must be an object"); return result; }
    const auto* version = member(root, "version");
    const auto* selectedId = member(root, "selectedId");
    const auto* profiles = member(root, "profiles");
    if (!version || version->kind != JsonValue::Kind::Number || version->number != CurrentSetVersion ||
        !selectedId || selectedId->kind != JsonValue::Kind::String || selectedId->string.empty() ||
        selectedId->string.size() > kMaxProfileStringBytes || selectedId->string.find('\0') != std::string::npos ||
        !profiles || profiles->kind != JsonValue::Kind::Array || profiles->array.empty() ||
        profiles->array.size() > kMaxProfileSetEntries) {
        result.error = json_error("invalid profile set header"); return result;
    }
    result.set.selectedId = selectedId->string;
    for (const auto& value : profiles->array) {
        const auto parsed = parse_profile_value(value);
        if (!parsed.valid) { result.error = parsed.error; return result; }
        if (std::find_if(result.set.profiles.begin(), result.set.profiles.end(),
                         [&](const auto& profile) { return profile.id == parsed.profile.id; }) != result.set.profiles.end()) {
            result.error = json_error("duplicate profile id"); return result;
        }
        result.set.profiles.push_back(parsed.profile);
    }
    const auto selected = std::find_if(result.set.profiles.begin(), result.set.profiles.end(),
                                       [&](const auto& profile) { return profile.id == result.set.selectedId; });
    if (selected == result.set.profiles.end()) {
        result.error = json_error("selected profile id is not present"); return result;
    }
    result.valid = true;
    return result;
}

bool EditorBuildProfileStore::serialize_set(const EditorBuildProfileSet& set, std::string& json,
                                            std::string* error) {
    json.clear();
    if (set.profiles.empty() || set.profiles.size() > kMaxProfileSetEntries || set.selectedId.empty()) {
        if (error) *error = json_error("profile set is empty or too large");
        return false;
    }
    if (set.selectedId.size() > kMaxProfileStringBytes || set.selectedId.find('\0') != std::string::npos) {
        if (error) *error = json_error("selected profile id is invalid");
        return false;
    }
    bool selectedFound = false;
    json = "{\n  \"version\": 2,\n  \"selectedId\": " + json_escape(set.selectedId) + ",\n  \"profiles\": [\n";
    for (std::size_t index = 0; index < set.profiles.size(); ++index) {
        const auto& profile = set.profiles[index];
        if (std::find_if(set.profiles.begin(), set.profiles.begin() + static_cast<std::ptrdiff_t>(index),
                         [&](const auto& previous) { return previous.id == profile.id; }) !=
            set.profiles.begin() + static_cast<std::ptrdiff_t>(index)) {
            if (error) *error = json_error("duplicate profile id");
            json.clear();
            return false;
        }
        if (profile.id == set.selectedId) selectedFound = true;
        std::string profileError;
        const auto value = profile_json_string(profile, &profileError, false);
        if (value.empty()) {
            if (error) *error = std::move(profileError);
            json.clear();
            return false;
        }
        if (index != 0) json += ",\n";
        json += value;
    }
    if (!selectedFound) {
        if (error) *error = json_error("selected profile id is not present");
        json.clear();
        return false;
    }
    json += "\n  ]\n}\n";
    if (json.size() > kMaxProfileJsonBytes) {
        if (error) *error = json_error("serialized profile set is too large");
        json.clear();
        return false;
    }
    return true;
}

std::vector<EditorIdeDescriptor> EditorToolIntegration::discover_ides(
    const std::filesystem::path& projectRoot, std::string_view searchPath) {
    const auto root = normalize_root(projectRoot);
    std::vector<std::filesystem::path> directories;
    if (!root.empty()) {
        directories.emplace_back(root / ".shinkou" / "tools");
        directories.emplace_back(root / "Tools");
    }
    if (searchPath.empty()) {
        if (const char* environment = std::getenv("PATH")) searchPath = environment;
    }
    const auto environmentDirectories = split_search_path(searchPath);
    directories.insert(directories.end(), environmentDirectories.begin(), environmentDirectories.end());
    std::vector<EditorIdeDescriptor> result;
    result.reserve(std::size(kIdeCandidates));
    for (const auto& candidate : kIdeCandidates) {
        const auto executable = find_ide_executable(directories, candidate);
        result.push_back({candidate.kind, candidate.name, executable, !executable.empty()});
    }
    return result;
}

EditorIdeLaunchPlan EditorToolIntegration::plan_ide_launch(
    const std::filesystem::path& projectRoot, const EditorBuildProfile& profile,
    EditorIdeKind kind, std::string_view selectedFile, std::size_t line,
    std::size_t column, std::string_view searchPath) {
    EditorIdeLaunchPlan result;
    result.kind = kind;
    const auto root = normalize_root(projectRoot);
    if (root.empty()) { result.error = "IDE launch project root is not configured"; return result; }
    const auto ides = discover_ides(root, searchPath);
    const auto* descriptor = find_ide(ides, kind);
    if (!descriptor || !descriptor->available) {
        result.error = "IDE is not available: " + std::string(ide_name(kind));
        return result;
    }
    result.executable = descriptor->executable;
    result.workingDirectory = root;
    const auto projectTarget = profile.projectFile.empty() ? root : resolve_project_path(root, profile.projectFile);
    if (projectTarget.empty()) { result.error = "IDE project file is outside the project root"; return result; }
    if (selectedFile.find('\0') != std::string_view::npos) { result.error = "IDE selected file contains NUL"; return result; }
    std::filesystem::path selectedPath;
    if (!selectedFile.empty()) {
        selectedPath = resolve_project_path(root, std::filesystem::path(std::string(selectedFile)));
        if (selectedPath.empty()) { result.error = "IDE selected file is outside the project root"; return result; }
        if (line > 1000000u || column > 1000000u) { result.error = "IDE source location is out of range"; return result; }
    }
    const auto selected = selectedPath.generic_string();
    switch (kind) {
    case EditorIdeKind::VisualStudio:
        result.arguments.push_back(projectTarget.generic_string());
        if (!selected.empty()) {
            result.arguments.push_back("/Command");
            std::string command = "File.OpenFile " + selected;
            if (line != 0) command += " " + std::to_string(line);
            result.arguments.push_back(std::move(command));
        }
        break;
    case EditorIdeKind::Rider:
        result.arguments.push_back(projectTarget.generic_string());
        if (!selected.empty()) {
            result.arguments.push_back("--line");
            result.arguments.push_back(std::to_string(line == 0 ? 1 : line));
            if (column != 0) {
                result.arguments.push_back("--column");
                result.arguments.push_back(std::to_string(column));
            }
            result.arguments.push_back(selected);
        }
        break;
    case EditorIdeKind::VisualStudioCode:
        result.arguments.push_back("--reuse-window");
        if (!selected.empty()) {
            result.arguments.push_back("--goto");
            std::string target = selected;
            if (line != 0) {
                target += ":" + std::to_string(line);
                if (column != 0) target += ":" + std::to_string(column);
            }
            result.arguments.push_back(std::move(target));
        }
        result.arguments.push_back(root.generic_string());
        break;
    case EditorIdeKind::Clangd: {
        const auto buildDirectory = resolve_project_path(root, profile.buildDirectory);
        if (buildDirectory.empty()) { result.error = "IDE build directory is outside the project root"; return result; }
        result.arguments.push_back("--compile-commands-dir=" + buildDirectory.generic_string());
        result.arguments.push_back("--background-index");
        break;
    }
    default:
        result.error = "IDE launch adapter is not implemented";
        return result;
    }
    result.valid = true;
    return result;
}

EditorExternalLaunchResult EditorToolIntegration::launch(const EditorIdeLaunchPlan& plan) {
    EditorExternalLaunchResult result;
    if (!plan.valid || plan.executable.empty() || plan.workingDirectory.empty()) {
        result.state = EditorExternalLaunchState::InvalidPlan;
        result.error = "IDE launch plan is invalid";
        return result;
    }
#if !defined(_WIN32)
    result.state = EditorExternalLaunchState::Unsupported;
    result.error = "external IDE launch is currently supported on Windows only";
    return result;
#else
    std::error_code error;
    if (!std::filesystem::is_regular_file(plan.executable, error) || error) {
        result.state = EditorExternalLaunchState::LaunchFailed;
        result.error = "IDE executable is not a regular file";
        return result;
    }
    const auto application = utf8_to_wide(plan.executable.generic_string());
    auto commandLine = quote_windows_argument(plan.executable.generic_string());
    for (const auto& argument : plan.arguments) {
        if (argument.find('\0') != std::string::npos) {
            result.state = EditorExternalLaunchState::InvalidPlan;
            result.error = "IDE launch argument contains NUL";
            return result;
        }
        commandLine.push_back(L' ');
        commandLine += quote_windows_argument(argument);
    }
    const auto working = utf8_to_wide(plan.workingDirectory.generic_string());
    if (application.empty() || working.empty() || commandLine.size() > static_cast<std::size_t>(INT_MAX)) {
        result.state = EditorExternalLaunchState::LaunchFailed;
        result.error = "IDE launch paths or arguments are invalid";
        return result;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    const auto created = CreateProcessW(application.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
        CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS | CREATE_UNICODE_ENVIRONMENT,
        nullptr, working.c_str(), &startup, &process);
    if (!created) {
        result.state = EditorExternalLaunchState::LaunchFailed;
        result.error = "CreateProcessW failed: " + std::to_string(GetLastError());
        return result;
    }
    result.state = EditorExternalLaunchState::Launched;
    result.processId = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
#endif
}

EditorExternalProcessStatus EditorToolIntegration::query_process(std::uint32_t processId) {
    EditorExternalProcessStatus result;
    result.processId = processId;
    if (processId == 0) {
        result.state = EditorExternalProcessState::NotStarted;
        result.error = "process id is zero";
        return result;
    }
#if !defined(_WIN32)
    result.state = EditorExternalProcessState::Unsupported;
    result.error = "external process tracking is currently supported on Windows only";
    return result;
#else
    const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId);
    if (!handle) {
        const auto code = GetLastError();
        result.state = code == ERROR_INVALID_PARAMETER ? EditorExternalProcessState::NotFound
                                                       : EditorExternalProcessState::QueryFailed;
        result.error = "OpenProcess failed: " + std::to_string(code);
        return result;
    }
    DWORD exitCode = STILL_ACTIVE;
    if (!GetExitCodeProcess(handle, &exitCode)) {
        result.state = EditorExternalProcessState::QueryFailed;
        result.error = "GetExitCodeProcess failed: " + std::to_string(GetLastError());
        CloseHandle(handle);
        return result;
    }
    CloseHandle(handle);
    if (exitCode == STILL_ACTIVE) {
        result.state = EditorExternalProcessState::Running;
        return result;
    }
    result.state = EditorExternalProcessState::Exited;
    result.exitCode = exitCode;
    return result;
#endif
}

std::string_view EditorToolIntegration::ide_name(EditorIdeKind kind) noexcept {
    switch (kind) {
    case EditorIdeKind::VisualStudio: return "Visual Studio";
    case EditorIdeKind::Rider: return "Rider";
    case EditorIdeKind::VisualStudioCode: return "VS Code";
    case EditorIdeKind::Clangd: return "clangd";
    case EditorIdeKind::Unknown: return "Unknown IDE";
    }
    return "Unknown IDE";
}

} // namespace shinkou::editor

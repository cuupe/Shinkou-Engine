#include "shinkou/editor/EditorCompileCommands.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>

namespace shinkou::editor {
namespace {

constexpr std::size_t kMaxInputBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxCommands = 32768;
constexpr std::size_t kMaxArguments = 4096;
constexpr unsigned kMaxJsonDepth = 32;

struct JsonValue {
    enum class Kind { Null, Boolean, Number, String, Object, Array };
    Kind kind{Kind::Null};
    double number{0.0};
    std::string string;
    std::map<std::string, JsonValue, std::less<>> object;
    std::vector<JsonValue> array;
};

class JsonParser final {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool parse(JsonValue& output, std::string& error) {
        if (input_.size() > kMaxInputBytes) return fail(error, "compile_commands JSON exceeds 16 MiB");
        skip_space();
        if (!parse_value(output, error, 0)) return false;
        skip_space();
        return position_ == input_.size() || fail(error, "trailing JSON data");
    }

private:
    std::string_view input_;
    std::size_t position_{0};

    bool fail(std::string& error, std::string message) const {
        error = std::move(message);
        return false;
    }

    void skip_space() noexcept {
        while (position_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[position_]))) ++position_;
    }

    bool consume(char value) noexcept {
        if (position_ >= input_.size() || input_[position_] != value) return false;
        ++position_;
        return true;
    }

    bool parse_value(JsonValue& output, std::string& error, unsigned depth) {
        if (depth > kMaxJsonDepth) return fail(error, "compile_commands JSON nesting is too deep");
        skip_space();
        if (position_ >= input_.size()) return fail(error, "unexpected end of JSON");
        switch (input_[position_]) {
        case '{': return parse_object(output, error, depth + 1);
        case '[': return parse_array(output, error, depth + 1);
        case '"': output.kind = JsonValue::Kind::String; return parse_string(output.string, error);
        case 'n': return parse_literal(output, "null", JsonValue::Kind::Null, error);
        case 't': return parse_literal(output, "true", JsonValue::Kind::Boolean, error);
        case 'f': return parse_literal(output, "false", JsonValue::Kind::Boolean, error);
        default: return parse_number(output, error);
        }
    }

    bool parse_literal(JsonValue& output, std::string_view literal, JsonValue::Kind kind,
                       std::string& error) {
        if (input_.substr(position_, literal.size()) != literal) return fail(error, "invalid JSON literal");
        position_ += literal.size();
        output.kind = kind;
        return true;
    }

    bool parse_number(JsonValue& output, std::string& error) {
        const auto start = position_;
        while (position_ < input_.size() && std::string_view("0123456789+-.eE").find(input_[position_]) != std::string_view::npos)
            ++position_;
        if (start == position_) return fail(error, "invalid JSON value");
        const std::string number(input_.substr(start, position_ - start));
        char* end = nullptr;
        const double value = std::strtod(number.c_str(), &end);
        if (end == number.c_str() || *end != '\0' || !std::isfinite(value)) return fail(error, "invalid JSON number");
        output.kind = JsonValue::Kind::Number;
        output.number = value;
        return true;
    }

    static bool hex_digit(char value, unsigned& output) noexcept {
        if (value >= '0' && value <= '9') { output = static_cast<unsigned>(value - '0'); return true; }
        if (value >= 'a' && value <= 'f') { output = static_cast<unsigned>(value - 'a' + 10); return true; }
        if (value >= 'A' && value <= 'F') { output = static_cast<unsigned>(value - 'A' + 10); return true; }
        return false;
    }

    static void append_utf8(std::string& output, unsigned codepoint) {
        if (codepoint <= 0x7fu) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ffu) {
            output.push_back(static_cast<char>(0xc0u | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else if (codepoint <= 0xffffu) {
            output.push_back(static_cast<char>(0xe0u | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else {
            output.push_back(static_cast<char>(0xf0u | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
    }

    bool parse_string(std::string& output, std::string& error) {
        if (!consume('"')) return fail(error, "expected JSON string");
        output.clear();
        while (position_ < input_.size()) {
            const auto character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') return true;
            if (character < 0x20u) return fail(error, "control character in JSON string");
            if (character != '\\') { output.push_back(static_cast<char>(character)); continue; }
            if (position_ >= input_.size()) return fail(error, "unfinished JSON escape");
            const auto escaped = input_[position_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                unsigned codepoint = 0;
                for (int index = 0; index < 4; ++index) {
                    if (position_ >= input_.size()) return fail(error, "unfinished unicode escape");
                    unsigned digit = 0;
                    if (!hex_digit(input_[position_++], digit)) return fail(error, "invalid unicode escape");
                    codepoint = (codepoint << 4u) | digit;
                }
                append_utf8(output, codepoint);
                break;
            }
            default: return fail(error, "invalid JSON escape");
            }
        }
        return fail(error, "unterminated JSON string");
    }

    bool parse_object(JsonValue& output, std::string& error, unsigned depth) {
        consume('{');
        output.kind = JsonValue::Kind::Object;
        output.object.clear();
        skip_space();
        if (consume('}')) return true;
        while (position_ < input_.size()) {
            skip_space();
            std::string key;
            if (!parse_string(key, error)) return false;
            skip_space();
            if (!consume(':')) return fail(error, "expected ':' in JSON object");
            JsonValue child;
            if (!parse_value(child, error, depth)) return false;
            if (!output.object.emplace(std::move(key), std::move(child)).second)
                return fail(error, "duplicate JSON key");
            skip_space();
            if (consume('}')) return true;
            if (!consume(',')) return fail(error, "expected ',' in JSON object");
        }
        return fail(error, "unterminated JSON object");
    }

    bool parse_array(JsonValue& output, std::string& error, unsigned depth) {
        consume('[');
        output.kind = JsonValue::Kind::Array;
        output.array.clear();
        skip_space();
        if (consume(']')) return true;
        while (position_ < input_.size()) {
            if (output.array.size() >= kMaxCommands) return fail(error, "compile_commands array is too large");
            JsonValue child;
            if (!parse_value(child, error, depth)) return false;
            output.array.push_back(std::move(child));
            skip_space();
            if (consume(']')) return true;
            if (!consume(',')) return fail(error, "expected ',' in JSON array");
            skip_space();
        }
        return fail(error, "unterminated JSON array");
    }
};

const JsonValue* member(const JsonValue& value, std::string_view name) {
    if (value.kind != JsonValue::Kind::Object) return nullptr;
    const auto found = value.object.find(name);
    return found == value.object.end() ? nullptr : &found->second;
}

bool string_member(const JsonValue& value, std::string_view name, std::string& output, bool required,
                   std::string& error) {
    const auto* field = member(value, name);
    if (!field) {
        if (required) error = "compile command is missing '" + std::string(name) + "'";
        return !required;
    }
    if (field->kind != JsonValue::Kind::String) {
        error = "compile command field '" + std::string(name) + "' must be a string";
        return false;
    }
    output = field->string;
    return true;
}

bool path_has_nul(const std::filesystem::path& path) {
    const auto value = path.generic_string();
    return value.find('\0') != std::string::npos;
}

std::filesystem::path normalize_root(const std::filesystem::path& root) {
    std::error_code error;
    auto result = root.empty() ? std::filesystem::current_path(error) : root;
    if (result.empty()) result = ".";
    result = std::filesystem::absolute(result, error).lexically_normal();
    if (error) return {};
    return std::filesystem::weakly_canonical(result, error).lexically_normal();
}

bool inside_root(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto candidate = path.lexically_normal().generic_string();
    const auto base = root.lexically_normal().generic_string();
    return candidate == base || candidate.rfind(base + '/', 0) == 0;
}

bool normalize_command_path(std::string_view raw, const std::filesystem::path& base,
                            const std::filesystem::path& root, std::filesystem::path& output,
                            std::string& error) {
    if (raw.empty()) { error = "compile command path is empty"; return false; }
    std::filesystem::path value{std::string(raw)};
    if (path_has_nul(value)) { error = "compile command path contains NUL"; return false; }
    if (value.is_relative()) value = base / value;
    std::error_code fsError;
    value = std::filesystem::absolute(value, fsError).lexically_normal();
    if (fsError) { error = "cannot normalize compile command path"; return false; }
    value = std::filesystem::weakly_canonical(value, fsError).lexically_normal();
    if (fsError || !inside_root(value, root)) {
        error = "compile command path is outside the project root";
        return false;
    }
    output = std::move(value);
    return true;
}

bool tokenize_command(std::string_view command, std::vector<std::string>& arguments, std::string& error) {
    arguments.clear();
    std::size_t cursor = 0;
    while (cursor < command.size()) {
        while (cursor < command.size() && std::isspace(static_cast<unsigned char>(command[cursor]))) ++cursor;
        if (cursor == command.size()) break;
        std::string argument;
        char quote = 0;
        while (cursor < command.size()) {
            const char character = command[cursor++];
            if (quote != 0) {
                if (character == quote) { quote = 0; continue; }
                if (character == '\\' && cursor < command.size()) {
                    argument.push_back(command[cursor++]);
                    continue;
                }
                argument.push_back(character);
                continue;
            }
            if (character == '\'' || character == '"') { quote = character; continue; }
            if (character == '\\' && cursor < command.size()) {
                argument.push_back(command[cursor++]);
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(character))) break;
            argument.push_back(character);
        }
        if (quote != 0) { error = "compile command has an unterminated quote"; return false; }
        arguments.push_back(std::move(argument));
        if (arguments.size() > kMaxArguments) { error = "compile command has too many arguments"; return false; }
    }
    return !arguments.empty();
}

std::string json_escape(std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20u) {
                output << "\\u00" << std::hex << static_cast<unsigned>(character) << std::dec;
            } else output << static_cast<char>(character);
            break;
        }
    }
    output << '"';
    return output.str();
}

void set_error(std::string* error, std::string value) {
    if (error) *error = std::move(value);
}

} // namespace

EditorCompileCommandsResult EditorCompileCommands::parse(std::string_view json,
                                                         const std::filesystem::path& projectRoot) {
    EditorCompileCommandsResult result;
    JsonValue root;
    if (!JsonParser(json).parse(root, result.error)) return result;
    if (root.kind != JsonValue::Kind::Array) { result.error = "compile_commands root must be an array"; return result; }

    const auto normalizedRoot = normalize_root(projectRoot);
    if (normalizedRoot.empty()) { result.error = "cannot determine project root"; return result; }
    result.commands.reserve(root.array.size());
    for (const auto& item : root.array) {
        if (item.kind != JsonValue::Kind::Object) { result.error = "compile command entry must be an object"; return result; }
        std::string directoryText;
        std::string fileText;
        std::string commandText;
        if (!string_member(item, "directory", directoryText, true, result.error) ||
            !string_member(item, "file", fileText, true, result.error)) return result;

        std::filesystem::path directory;
        if (!normalize_command_path(directoryText, normalizedRoot, normalizedRoot, directory, result.error)) return result;
        std::filesystem::path file;
        if (!normalize_command_path(fileText, directory, normalizedRoot, file, result.error)) return result;

        EditorCompileCommand command;
        command.directory = std::move(directory);
        command.file = std::move(file);
        const auto* arguments = member(item, "arguments");
        const auto* commandField = member(item, "command");
        if (arguments) {
            if (arguments->kind != JsonValue::Kind::Array || arguments->array.empty() || arguments->array.size() > kMaxArguments) {
                result.error = "compile command 'arguments' must be a non-empty array";
                return result;
            }
            for (const auto& argument : arguments->array) {
                if (argument.kind != JsonValue::Kind::String || argument.string.find('\0') != std::string::npos) {
                    result.error = "compile command arguments must be strings without NUL";
                    return result;
                }
                command.arguments.push_back(argument.string);
            }
        } else if (commandField) {
            if (commandField->kind != JsonValue::Kind::String || !tokenize_command(commandField->string, command.arguments, result.error)) {
                if (result.error.empty()) result.error = "compile command 'command' is empty";
                return result;
            }
        } else {
            result.error = "compile command needs 'arguments' or 'command'";
            return result;
        }
        result.commands.push_back(std::move(command));
    }
    result.valid = true;
    return result;
}

bool EditorCompileCommands::serialize(const std::vector<EditorCompileCommand>& commands,
                                      const std::filesystem::path& projectRoot,
                                      std::string& json, std::string* error) {
    json.clear();
    const auto normalizedRoot = normalize_root(projectRoot);
    if (normalizedRoot.empty()) { set_error(error, "cannot determine project root"); return false; }
    if (commands.size() > kMaxCommands) { set_error(error, "too many compile commands"); return false; }

    json = "[\n";
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const auto& command = commands[index];
        if (command.arguments.empty() || command.arguments.size() > kMaxArguments) {
            set_error(error, "compile command arguments are empty or too large");
            json.clear();
            return false;
        }
        std::filesystem::path directory;
        std::filesystem::path file;
        std::string pathError;
        if (!normalize_command_path(command.directory.generic_string(), normalizedRoot, normalizedRoot, directory, pathError) ||
            !normalize_command_path(command.file.generic_string(), directory, normalizedRoot, file, pathError)) {
            set_error(error, pathError);
            json.clear();
            return false;
        }
        json += "  {\n    \"directory\": " + json_escape(directory.generic_string()) +
            ",\n    \"file\": " + json_escape(file.generic_string()) + ",\n    \"arguments\": [";
        for (std::size_t argument = 0; argument < command.arguments.size(); ++argument) {
            if (argument != 0) json += ", ";
            if (command.arguments[argument].find('\0') != std::string::npos) {
                set_error(error, "compile command argument contains NUL");
                json.clear();
                return false;
            }
            json += json_escape(command.arguments[argument]);
        }
        json += "]\n  }";
        if (index + 1 != commands.size()) json += ',';
        json += '\n';
    }
    json += "]\n";
    return true;
}

} // namespace shinkou::editor

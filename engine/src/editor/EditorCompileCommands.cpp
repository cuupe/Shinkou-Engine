#include "shinkou/core/Path.h"
#include "shinkou/data/Json.h"
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
using JsonValue = data::Value;
using JsonParser = data::JsonParser;

constexpr std::size_t kMaxInputBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxCommands = 32768;
constexpr std::size_t kMaxArguments = 4096;
constexpr unsigned kMaxJsonDepth = 32;

const JsonValue *member(const JsonValue &value, std::string_view name) {
    if (value.kind() != JsonValue::Kind::Object)
        return nullptr;
    const auto found = value.object_items().find(name);
    return found == value.object_items().end() ? nullptr : &found->second;
}

bool string_member(const JsonValue &value, std::string_view name, std::string &output,
                   bool required, std::string &error) {
    const auto *field = member(value, name);
    if (!field) {
        if (required)
            error = "compile command is missing '" + std::string(name) + "'";
        return !required;
    }
    if (field->kind() != JsonValue::Kind::String) {
        error = "compile command field '" + std::string(name) + "' must be a string";
        return false;
    }
    output = field->string_or();
    return true;
}

bool path_has_nul(const std::filesystem::path &path) {
    const auto value = core::path_to_utf8(path);
    return value.find('\0') != std::string::npos;
}

std::filesystem::path normalize_root(const std::filesystem::path &root) {
    std::error_code error;
    auto result = root.empty() ? std::filesystem::current_path(error) : root;
    if (result.empty())
        result = ".";
    result = std::filesystem::absolute(result, error).lexically_normal();
    if (error)
        return {};
    return std::filesystem::weakly_canonical(result, error).lexically_normal();
}

bool inside_root(const std::filesystem::path &path, const std::filesystem::path &root) {
    const auto candidate = core::path_to_utf8(path.lexically_normal());
    const auto base = core::path_to_utf8(root.lexically_normal());
    return candidate == base || candidate.rfind(base + '/', 0) == 0;
}

bool normalize_command_path(std::string_view raw, const std::filesystem::path &base,
                            const std::filesystem::path &root, std::filesystem::path &output,
                            std::string &error) {
    if (raw.empty()) {
        error = "compile command path is empty";
        return false;
    }
    auto value = core::path_from_utf8(raw);
    if (path_has_nul(value)) {
        error = "compile command path contains NUL";
        return false;
    }
    if (value.is_relative())
        value = base / value;
    std::error_code fsError;
    value = std::filesystem::absolute(value, fsError).lexically_normal();
    if (fsError) {
        error = "cannot normalize compile command path";
        return false;
    }
    value = std::filesystem::weakly_canonical(value, fsError).lexically_normal();
    if (fsError || !inside_root(value, root)) {
        error = "compile command path is outside the project root";
        return false;
    }
    output = std::move(value);
    return true;
}

bool tokenize_command(std::string_view command, std::vector<std::string> &arguments,
                      std::string &error) {
    arguments.clear();
    std::size_t cursor = 0;
    while (cursor < command.size()) {
        while (cursor < command.size() && std::isspace(static_cast<unsigned char>(command[cursor])))
            ++cursor;
        if (cursor == command.size())
            break;
        std::string argument;
        char quote = 0;
        while (cursor < command.size()) {
            const char character = command[cursor++];
            if (quote != 0) {
                if (character == quote) {
                    quote = 0;
                    continue;
                }
                if (character == '\\' && cursor < command.size()) {
                    argument.push_back(command[cursor++]);
                    continue;
                }
                argument.push_back(character);
                continue;
            }
            if (character == '\'' || character == '"') {
                quote = character;
                continue;
            }
            if (character == '\\' && cursor < command.size()) {
                argument.push_back(command[cursor++]);
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(character)))
                break;
            argument.push_back(character);
        }
        if (quote != 0) {
            error = "compile command has an unterminated quote";
            return false;
        }
        arguments.push_back(std::move(argument));
        if (arguments.size() > kMaxArguments) {
            error = "compile command has too many arguments";
            return false;
        }
    }
    return !arguments.empty();
}

std::string json_escape(std::string_view value) {
    return data::quote_json(value);
}

void set_error(std::string *error, std::string value) {
    if (error)
        *error = std::move(value);
}

} // namespace

EditorCompileCommandsResult EditorCompileCommands::parse(std::string_view json,
                                                         const std::filesystem::path &projectRoot) {
    EditorCompileCommandsResult result;
    JsonValue root;
    if (!JsonParser(json, data::JsonLimits{kMaxInputBytes, kMaxJsonDepth, 1000000, 1024u*1024u, kMaxCommands*128u, 4096}).parse(root, result.error))
        return result;
    if (root.kind() != JsonValue::Kind::Array) {
        result.error = "compile_commands root must be an array";
        return result;
    }

    const auto normalizedRoot = normalize_root(projectRoot);
    if (normalizedRoot.empty()) {
        result.error = "cannot determine project root";
        return result;
    }
    result.commands.reserve(root.array_items().size());
    for (const auto &item : root.array_items()) {
        if (item.kind() != JsonValue::Kind::Object) {
            result.error = "compile command entry must be an object";
            return result;
        }
        std::string directoryText;
        std::string fileText;
        std::string commandText;
        if (!string_member(item, "directory", directoryText, true, result.error) ||
            !string_member(item, "file", fileText, true, result.error))
            return result;

        std::filesystem::path directory;
        if (!normalize_command_path(directoryText, normalizedRoot, normalizedRoot, directory,
                                    result.error))
            return result;
        std::filesystem::path file;
        if (!normalize_command_path(fileText, directory, normalizedRoot, file, result.error))
            return result;

        EditorCompileCommand command;
        command.directory = std::move(directory);
        command.file = std::move(file);
        const auto *arguments = member(item, "arguments");
        const auto *commandField = member(item, "command");
        if (arguments) {
            if (arguments->kind() != JsonValue::Kind::Array || arguments->array_items().empty() ||
                arguments->array_items().size() > kMaxArguments) {
                result.error = "compile command 'arguments' must be a non-empty array";
                return result;
            }
            for (const auto &argument : arguments->array_items()) {
                if (argument.kind() != JsonValue::Kind::String ||
                    argument.string_or().find('\0') != std::string::npos) {
                    result.error = "compile command arguments must be strings without NUL";
                    return result;
                }
                command.arguments.push_back(argument.string_or());
            }
        } else if (commandField) {
            if (commandField->kind() != JsonValue::Kind::String ||
                !tokenize_command(commandField->string_or(), command.arguments, result.error)) {
                if (result.error.empty())
                    result.error = "compile command 'command' is empty";
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

bool EditorCompileCommands::serialize(const std::vector<EditorCompileCommand> &commands,
                                      const std::filesystem::path &projectRoot, std::string &json,
                                      std::string *error) {
    json.clear();
    const auto normalizedRoot = normalize_root(projectRoot);
    if (normalizedRoot.empty()) {
        set_error(error, "cannot determine project root");
        return false;
    }
    if (commands.size() > kMaxCommands) {
        set_error(error, "too many compile commands");
        return false;
    }

    json = "[\n";
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const auto &command = commands[index];
        if (command.arguments.empty() || command.arguments.size() > kMaxArguments) {
            set_error(error, "compile command arguments are empty or too large");
            json.clear();
            return false;
        }
        std::filesystem::path directory;
        std::filesystem::path file;
        std::string pathError;
        if (!normalize_command_path(core::path_to_utf8(command.directory), normalizedRoot,
                                    normalizedRoot, directory, pathError) ||
            !normalize_command_path(core::path_to_utf8(command.file), directory, normalizedRoot, file,
                                    pathError)) {
            set_error(error, pathError);
            json.clear();
            return false;
        }
        json += "  {\n    \"directory\": " + json_escape(core::path_to_utf8(directory)) +
                ",\n    \"file\": " + json_escape(core::path_to_utf8(file)) +
                ",\n    \"arguments\": [";
        for (std::size_t argument = 0; argument < command.arguments.size(); ++argument) {
            if (argument != 0)
                json += ", ";
            if (command.arguments[argument].find('\0') != std::string::npos) {
                set_error(error, "compile command argument contains NUL");
                json.clear();
                return false;
            }
            json += json_escape(command.arguments[argument]);
        }
        json += "]\n  }";
        if (index + 1 != commands.size())
            json += ',';
        json += '\n';
    }
    json += "]\n";
    return true;
}

} // namespace shinkou::editor

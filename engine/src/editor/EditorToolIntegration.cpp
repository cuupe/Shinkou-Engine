#include "shinkou/core/Path.h"
#include "shinkou/data/Json.h"
#include "shinkou/editor/EditorToolIntegration.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
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
using JsonValue = data::Value;
using JsonParser = data::JsonParser;

constexpr std::size_t kMaxProfileJsonBytes = 1u * 1024u * 1024u;
constexpr std::size_t kMaxProfileStringBytes = 16u * 1024u;
constexpr std::size_t kMaxProfileArguments = 128;
constexpr std::size_t kMaxProfileArgumentBytes = 16u * 1024u;
constexpr std::size_t kMaxProfileSetEntries = 32;
constexpr std::size_t kMaxJsonDepth = 16;
constexpr std::size_t kMaxJsonMembers = 64;

std::string json_error(std::string message) {
    return "build profile JSON: " + std::move(message);
}

const JsonValue *member(const JsonValue &value, std::string_view name) {
    if (value.kind() != JsonValue::Kind::Object)
        return nullptr;
    const auto found = value.object_items().find(name);
    return found == value.object_items().end() ? nullptr : &found->second;
}

bool read_string(const JsonValue &object, std::string_view name, std::string &output, bool required,
                 std::string &error) {
    const auto *value = member(object, name);
    if (!value) {
        if (required) {
            error = json_error("missing string member: " + std::string(name));
            return false;
        }
        output.clear();
        return true;
    }
    if (value->kind() != JsonValue::Kind::String || value->string_or().find('\0') != std::string::npos) {
        error = json_error("invalid string member: " + std::string(name));
        return false;
    }
    output = value->string_or();
    return true;
}

bool safe_relative_path(std::string_view value, std::filesystem::path &output, bool allowEmpty) {
    if (value.empty()) {
        output.clear();
        return allowEmpty;
    }
    auto path = core::path_from_utf8(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;
    const auto normalized = path.lexically_normal();
    if (normalized.empty() || (normalized == "." && value != "."))
        return false;
    const auto generic = core::path_to_utf8(normalized);
    if (generic == ".." || generic.rfind("../", 0) == 0)
        return false;
    output = normalized;
    return true;
}

std::string json_escape(std::string_view value) {
    return data::quote_json(value);
}

std::string tool_kind_name(EditorToolKind kind) {
    return std::string(EditorBuildSystem::tool_name(kind));
}

bool parse_tool_kind(std::string_view value, EditorToolKind &output) {
    constexpr EditorToolKind kinds[] = {
        EditorToolKind::CMake, EditorToolKind::Ninja,   EditorToolKind::MSBuild,
        EditorToolKind::Clang, EditorToolKind::ClangCl, EditorToolKind::GCC,
        EditorToolKind::GXX,   EditorToolKind::DotNet,
    };
    for (const auto kind : kinds) {
        if (value == EditorBuildSystem::tool_name(kind)) {
            output = kind;
            return true;
        }
    }
    return false;
}

bool read_arguments(const JsonValue &object, std::string_view name,
                    std::vector<std::string> &output, std::string &error) {
    const auto *value = member(object, name);
    if (!value) {
        output.clear();
        return true;
    }
    if (value->kind() != JsonValue::Kind::Array || value->array_items().size() > kMaxProfileArguments) {
        error = json_error("invalid argument array: " + std::string(name));
        return false;
    }
    output.clear();
    output.reserve(value->array_items().size());
    for (const auto &item : value->array_items()) {
        if (item.kind() != JsonValue::Kind::String || item.string_or().find('\0') != std::string::npos ||
            item.string_or().size() > kMaxProfileArgumentBytes) {
            error = json_error("invalid argument in: " + std::string(name));
            return false;
        }
        output.push_back(item.string_or());
    }
    return true;
}

bool contains_nul_or_too_large(std::string_view value) {
    return value.find('\0') != std::string_view::npos || value.size() > kMaxProfileStringBytes;
}

std::string path_string(const std::filesystem::path &value) {
    return core::path_to_utf8(value);
}

std::string profile_json_string(const EditorBuildProfile &profile, std::string *error,
                                bool includeVersion = true) {
    const auto fields = std::array<std::pair<std::string_view, std::string>, 9>{
        std::pair{"id", profile.id},
        std::pair{"name", profile.name},
        std::pair{"sourceDirectory", path_string(profile.sourceDirectory)},
        std::pair{"buildDirectory", path_string(profile.buildDirectory)},
        std::pair{"projectFile", path_string(profile.projectFile)},
        std::pair{"generator", profile.generator},
        std::pair{"configuration", profile.configuration},
        std::pair{"target", profile.target},
        std::pair{"configureTool", tool_kind_name(profile.configureTool)},
    };
    if (profile.id.empty() || profile.name.empty()) {
        if (error)
            *error = json_error("profile id and name must not be empty");
        return {};
    }
    for (const auto &[name, value] : fields) {
        if (contains_nul_or_too_large(value)) {
            if (error)
                *error = json_error("profile member is empty, contains NUL, or is too large: " +
                                    std::string(name));
            return {};
        }
    }
    if (contains_nul_or_too_large(tool_kind_name(profile.buildTool))) {
        if (error)
            *error = json_error("invalid build tool");
        return {};
    }
    const auto validate_path = [&](const std::filesystem::path &value, bool allowEmpty,
                                   std::string_view name) {
        const auto text = path_string(value);
        std::filesystem::path ignored;
        if (contains_nul_or_too_large(text) || !safe_relative_path(text, ignored, allowEmpty)) {
            if (error)
                *error = json_error("profile path is not project-relative: " + std::string(name));
            return false;
        }
        return true;
    };
    if (!validate_path(profile.sourceDirectory, false, "sourceDirectory") ||
        !validate_path(profile.buildDirectory, false, "buildDirectory") ||
        !validate_path(profile.projectFile, true, "projectFile"))
        return {};
    const auto validate_args = [&](const std::vector<std::string> &args, std::string_view name) {
        if (args.size() > kMaxProfileArguments) {
            if (error)
                *error = json_error("too many arguments: " + std::string(name));
            return false;
        }
        for (const auto &argument : args) {
            if (argument.find('\0') != std::string::npos ||
                argument.size() > kMaxProfileArgumentBytes) {
                if (error)
                    *error = json_error("argument is invalid: " + std::string(name));
                return false;
            }
        }
        return true;
    };
    if (!validate_args(profile.configureArguments, "configureArguments") ||
        !validate_args(profile.buildArguments, "buildArguments"))
        return {};

    std::string json = "{\n";
    if (includeVersion)
        json += "  \"version\": 1,\n";
    const auto field = [&json](std::string_view name, std::string_view value, bool comma = true) {
        json += "  ";
        json += json_escape(name);
        json += ": ";
        json += json_escape(value);
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
    const auto array = [&json](std::string_view name, const std::vector<std::string> &values,
                               bool comma) {
        json += "  ";
        json += json_escape(name);
        json += ": [";
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index != 0)
                json += ", ";
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
    for (auto &character : value)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

std::filesystem::path normalize_root(std::filesystem::path root) {
    std::error_code error;
    if (root.empty())
        root = std::filesystem::current_path(error);
    const auto absolute = std::filesystem::absolute(root, error);
    return (error ? root : absolute).lexically_normal();
}

bool is_within(const std::filesystem::path &root, const std::filesystem::path &candidate) {
    const auto base = ascii_lower(core::path_to_utf8(root.lexically_normal()));
    const auto value = ascii_lower(core::path_to_utf8(candidate.lexically_normal()));
    return !base.empty() && (value == base || value.rfind(base + '/', 0) == 0);
}

std::filesystem::path resolve_project_path(const std::filesystem::path &root,
                                           const std::filesystem::path &input) {
    if (input.empty())
        return {};
    if (input.is_absolute())
        return is_within(root, input) ? input.lexically_normal() : std::filesystem::path{};
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
        const auto part =
            value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin);
        if (!part.empty())
            result.emplace_back(std::string(part));
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return result;
}

struct IdeCandidate {
    EditorIdeKind kind;
    const char *name;
    const char *const *executableNames;
    std::size_t executableNameCount;
};

constexpr const char *kVisualStudioNames[] = {"devenv.com", "devenv.exe", "devenv"};
constexpr const char *kRiderNames[] = {"rider64.exe", "rider64", "rider"};
// Do not discover the code.cmd shim: launching it would require going through
// a shell, which would break the explicit executable/argument safety boundary.
constexpr const char *kCodeNames[] = {"code.exe", "Code.exe", "code"};
constexpr const char *kClangdNames[] = {"clangd.exe", "clangd"};
constexpr IdeCandidate kIdeCandidates[] = {
    {EditorIdeKind::VisualStudio, "Visual Studio", kVisualStudioNames,
     std::size(kVisualStudioNames)},
    {EditorIdeKind::Rider, "Rider", kRiderNames, std::size(kRiderNames)},
    {EditorIdeKind::VisualStudioCode, "VS Code", kCodeNames, std::size(kCodeNames)},
    {EditorIdeKind::Clangd, "clangd", kClangdNames, std::size(kClangdNames)},
};

std::filesystem::path find_ide_executable(const std::vector<std::filesystem::path> &directories,
                                          const IdeCandidate &candidate) {
    for (const auto &directory : directories) {
        if (directory.empty())
            continue;
        for (std::size_t index = 0; index < candidate.executableNameCount; ++index) {
            const auto path = (directory / candidate.executableNames[index]).lexically_normal();
            std::error_code error;
            if (std::filesystem::is_regular_file(path, error) && !error)
                return path;
        }
    }
    return {};
}

const EditorIdeDescriptor *find_ide(const std::vector<EditorIdeDescriptor> &ides,
                                    EditorIdeKind kind) {
    const auto found = std::find_if(ides.begin(), ides.end(),
                                    [kind](const auto &value) { return value.kind == kind; });
    return found == ides.end() ? nullptr : &*found;
}

#if defined(_WIN32)
std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty() || value.size() > static_cast<std::size_t>(INT_MAX))
        return {};
    const auto length = static_cast<int>(value.size());
    int required =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (required <= 0)
        required = MultiByteToWideChar(CP_ACP, 0, value.data(), length, nullptr, 0);
    if (required <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, result.data(),
                            required) <= 0 &&
        MultiByteToWideChar(CP_ACP, 0, value.data(), length, result.data(), required) <= 0)
        return {};
    return result;
}

std::wstring quote_windows_argument(std::string_view value) {
    const auto wide = utf8_to_wide(value);
    if (wide.empty())
        return L"\"\"";
    if (wide.find_first_of(L" \t\"") == std::wstring::npos)
        return wide;
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : wide) {
        if (character == L'\\')
            ++slashes;
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

bool EditorBuildProfileStore::serialize(const EditorBuildProfile &profile, std::string &json,
                                        std::string *error) {
    json = profile_json_string(profile, error);
    return !json.empty();
}

EditorBuildProfileLoadResult EditorBuildProfileStore::deserialize(std::string_view json) {
    EditorBuildProfileLoadResult result;
    JsonValue root;
    JsonParser parser(json, data::JsonLimits{kMaxProfileJsonBytes, kMaxJsonDepth, 16384, kMaxProfileStringBytes, 4096, kMaxJsonMembers});
    if (!parser.parse(root)) {
        result.error = parser.error();
        return result;
    }
    if (root.kind() != JsonValue::Kind::Object) {
        result.error = json_error("root must be an object");
        return result;
    }
    const auto *version = member(root, "version");
    if (!version || version->kind() != JsonValue::Kind::Number || version->number_or() != CurrentVersion) {
        result.error = json_error("unsupported or missing version");
        return result;
    }
    auto &profile = result.profile;
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
        !read_string(root, "target", profile.target, false, result.error))
        return result;
    std::string configureTool;
    std::string buildTool;
    if (!read_string(root, "configureTool", configureTool, true, result.error) ||
        !read_string(root, "buildTool", buildTool, true, result.error) ||
        !parse_tool_kind(configureTool, profile.configureTool) ||
        !parse_tool_kind(buildTool, profile.buildTool)) {
        result.error = json_error("unknown configure/build tool");
        return result;
    }
    if (!safe_relative_path(sourceDirectory, profile.sourceDirectory, false) ||
        !safe_relative_path(buildDirectory, profile.buildDirectory, false) ||
        !safe_relative_path(projectFile, profile.projectFile, true)) {
        result.error = json_error("profile paths must be project-relative");
        return result;
    }
    if (!read_arguments(root, "configureArguments", profile.configureArguments, result.error) ||
        !read_arguments(root, "buildArguments", profile.buildArguments, result.error))
        return result;
    if (profile.id.empty() || profile.name.empty() || contains_nul_or_too_large(profile.id) ||
        contains_nul_or_too_large(profile.name) || contains_nul_or_too_large(profile.generator) ||
        contains_nul_or_too_large(profile.configuration) ||
        contains_nul_or_too_large(profile.target)) {
        result.error = json_error("profile identity or string is invalid");
        return result;
    }
    result.valid = true;
    return result;
}

namespace {

EditorBuildProfileLoadResult parse_profile_value(const JsonValue &root) {
    EditorBuildProfileLoadResult result;
    if (root.kind() != JsonValue::Kind::Object) {
        result.error = json_error("profile entry must be an object");
        return result;
    }
    if (const auto *version = member(root, "version")) {
        if (version->kind() != JsonValue::Kind::Number ||
            version->number_or() != EditorBuildProfileStore::CurrentVersion) {
            result.error = json_error("unsupported profile entry version");
            return result;
        }
    }
    auto &profile = result.profile;
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
        !read_string(root, "target", profile.target, false, result.error))
        return result;
    std::string configureTool;
    std::string buildTool;
    if (!read_string(root, "configureTool", configureTool, true, result.error) ||
        !read_string(root, "buildTool", buildTool, true, result.error) ||
        !parse_tool_kind(configureTool, profile.configureTool) ||
        !parse_tool_kind(buildTool, profile.buildTool)) {
        result.error = json_error("unknown configure/build tool");
        return result;
    }
    if (!safe_relative_path(sourceDirectory, profile.sourceDirectory, false) ||
        !safe_relative_path(buildDirectory, profile.buildDirectory, false) ||
        !safe_relative_path(projectFile, profile.projectFile, true)) {
        result.error = json_error("profile paths must be project-relative");
        return result;
    }
    if (!read_arguments(root, "configureArguments", profile.configureArguments, result.error) ||
        !read_arguments(root, "buildArguments", profile.buildArguments, result.error))
        return result;
    if (profile.id.empty() || profile.name.empty() || contains_nul_or_too_large(profile.id) ||
        contains_nul_or_too_large(profile.name) || contains_nul_or_too_large(profile.generator) ||
        contains_nul_or_too_large(profile.configuration) ||
        contains_nul_or_too_large(profile.target)) {
        result.error = json_error("profile identity or string is invalid");
        return result;
    }
    result.valid = true;
    return result;
}

} // namespace

EditorBuildProfileSetLoadResult EditorBuildProfileStore::deserialize_set(std::string_view json) {
    EditorBuildProfileSetLoadResult result;
    JsonValue root;
    JsonParser parser(json, data::JsonLimits{kMaxProfileJsonBytes, kMaxJsonDepth, 16384, kMaxProfileStringBytes, 4096, kMaxJsonMembers});
    if (!parser.parse(root)) {
        result.error = parser.error();
        return result;
    }
    if (root.kind() != JsonValue::Kind::Object) {
        result.error = json_error("root must be an object");
        return result;
    }
    const auto *version = member(root, "version");
    const auto *selectedId = member(root, "selectedId");
    const auto *profiles = member(root, "profiles");
    if (!version || version->kind() != JsonValue::Kind::Number ||
        version->number_or() != CurrentSetVersion || !selectedId ||
        selectedId->kind() != JsonValue::Kind::String || selectedId->string_or().empty() ||
        selectedId->string_or().size() > kMaxProfileStringBytes ||
        selectedId->string_or().find('\0') != std::string::npos || !profiles ||
        profiles->kind() != JsonValue::Kind::Array || profiles->array_items().empty() ||
        profiles->array_items().size() > kMaxProfileSetEntries) {
        result.error = json_error("invalid profile set header");
        return result;
    }
    result.set.selectedId = selectedId->string_or();
    for (const auto &value : profiles->array_items()) {
        const auto parsed = parse_profile_value(value);
        if (!parsed.valid) {
            result.error = parsed.error;
            return result;
        }
        if (std::find_if(result.set.profiles.begin(), result.set.profiles.end(),
                         [&](const auto &profile) { return profile.id == parsed.profile.id; }) !=
            result.set.profiles.end()) {
            result.error = json_error("duplicate profile id");
            return result;
        }
        result.set.profiles.push_back(parsed.profile);
    }
    const auto selected =
        std::find_if(result.set.profiles.begin(), result.set.profiles.end(),
                     [&](const auto &profile) { return profile.id == result.set.selectedId; });
    if (selected == result.set.profiles.end()) {
        result.error = json_error("selected profile id is not present");
        return result;
    }
    result.valid = true;
    return result;
}

bool EditorBuildProfileStore::serialize_set(const EditorBuildProfileSet &set, std::string &json,
                                            std::string *error) {
    json.clear();
    if (set.profiles.empty() || set.profiles.size() > kMaxProfileSetEntries ||
        set.selectedId.empty()) {
        if (error)
            *error = json_error("profile set is empty or too large");
        return false;
    }
    if (set.selectedId.size() > kMaxProfileStringBytes ||
        set.selectedId.find('\0') != std::string::npos) {
        if (error)
            *error = json_error("selected profile id is invalid");
        return false;
    }
    bool selectedFound = false;
    json = "{\n  \"version\": 2,\n  \"selectedId\": " + json_escape(set.selectedId) +
           ",\n  \"profiles\": [\n";
    for (std::size_t index = 0; index < set.profiles.size(); ++index) {
        const auto &profile = set.profiles[index];
        if (std::find_if(set.profiles.begin(),
                         set.profiles.begin() + static_cast<std::ptrdiff_t>(index),
                         [&](const auto &previous) { return previous.id == profile.id; }) !=
            set.profiles.begin() + static_cast<std::ptrdiff_t>(index)) {
            if (error)
                *error = json_error("duplicate profile id");
            json.clear();
            return false;
        }
        if (profile.id == set.selectedId)
            selectedFound = true;
        std::string profileError;
        const auto value = profile_json_string(profile, &profileError, false);
        if (value.empty()) {
            if (error)
                *error = std::move(profileError);
            json.clear();
            return false;
        }
        if (index != 0)
            json += ",\n";
        json += value;
    }
    if (!selectedFound) {
        if (error)
            *error = json_error("selected profile id is not present");
        json.clear();
        return false;
    }
    json += "\n  ]\n}\n";
    if (json.size() > kMaxProfileJsonBytes) {
        if (error)
            *error = json_error("serialized profile set is too large");
        json.clear();
        return false;
    }
    return true;
}

std::vector<EditorIdeDescriptor>
EditorToolIntegration::discover_ides(const std::filesystem::path &projectRoot,
                                     std::string_view searchPath) {
    const auto root = normalize_root(projectRoot);
    std::vector<std::filesystem::path> directories;
    if (!root.empty()) {
        directories.emplace_back(root / ".shinkou" / "tools");
        directories.emplace_back(root / "Tools");
    }
    if (searchPath.empty()) {
        if (const char *environment = std::getenv("PATH"))
            searchPath = environment;
    }
    const auto environmentDirectories = split_search_path(searchPath);
    directories.insert(directories.end(), environmentDirectories.begin(),
                       environmentDirectories.end());
    std::vector<EditorIdeDescriptor> result;
    result.reserve(std::size(kIdeCandidates));
    for (const auto &candidate : kIdeCandidates) {
        const auto executable = find_ide_executable(directories, candidate);
        result.push_back({candidate.kind, candidate.name, executable, !executable.empty()});
    }
    return result;
}

EditorIdeLaunchPlan EditorToolIntegration::plan_ide_launch(const std::filesystem::path &projectRoot,
                                                           const EditorBuildProfile &profile,
                                                           EditorIdeKind kind,
                                                           std::string_view selectedFile,
                                                           std::size_t line, std::size_t column,
                                                           std::string_view searchPath) {
    EditorIdeLaunchPlan result;
    result.kind = kind;
    const auto root = normalize_root(projectRoot);
    if (root.empty()) {
        result.error = "IDE launch project root is not configured";
        return result;
    }
    const auto ides = discover_ides(root, searchPath);
    const auto *descriptor = find_ide(ides, kind);
    if (!descriptor || !descriptor->available) {
        result.error = "IDE is not available: " + std::string(ide_name(kind));
        return result;
    }
    result.executable = descriptor->executable;
    result.workingDirectory = root;
    const auto projectTarget =
        profile.projectFile.empty() ? root : resolve_project_path(root, profile.projectFile);
    if (projectTarget.empty()) {
        result.error = "IDE project file is outside the project root";
        return result;
    }
    if (selectedFile.find('\0') != std::string_view::npos) {
        result.error = "IDE selected file contains NUL";
        return result;
    }
    std::filesystem::path selectedPath;
    if (!selectedFile.empty()) {
        selectedPath = resolve_project_path(root, core::path_from_utf8(selectedFile));
        if (selectedPath.empty()) {
            result.error = "IDE selected file is outside the project root";
            return result;
        }
        if (line > 1000000u || column > 1000000u) {
            result.error = "IDE source location is out of range";
            return result;
        }
    }
    const auto selected = core::path_to_utf8(selectedPath);
    switch (kind) {
    case EditorIdeKind::VisualStudio:
        result.arguments.push_back(core::path_to_utf8(projectTarget));
        if (!selected.empty()) {
            result.arguments.push_back("/Command");
            std::string command = "File.OpenFile " + selected;
            if (line != 0)
                command += " " + std::to_string(line);
            result.arguments.push_back(std::move(command));
        }
        break;
    case EditorIdeKind::Rider:
        result.arguments.push_back(core::path_to_utf8(projectTarget));
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
                if (column != 0)
                    target += ":" + std::to_string(column);
            }
            result.arguments.push_back(std::move(target));
        }
        result.arguments.push_back(core::path_to_utf8(root));
        break;
    case EditorIdeKind::Clangd: {
        const auto buildDirectory = resolve_project_path(root, profile.buildDirectory);
        if (buildDirectory.empty()) {
            result.error = "IDE build directory is outside the project root";
            return result;
        }
        result.arguments.push_back("--compile-commands-dir=" + core::path_to_utf8(buildDirectory));
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

EditorExternalLaunchResult EditorToolIntegration::launch(const EditorIdeLaunchPlan &plan) {
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
    const auto application = utf8_to_wide(core::path_to_utf8(plan.executable));
    auto commandLine = quote_windows_argument(core::path_to_utf8(plan.executable));
    for (const auto &argument : plan.arguments) {
        if (argument.find('\0') != std::string::npos) {
            result.state = EditorExternalLaunchState::InvalidPlan;
            result.error = "IDE launch argument contains NUL";
            return result;
        }
        commandLine.push_back(L' ');
        commandLine += quote_windows_argument(argument);
    }
    const auto working = utf8_to_wide(core::path_to_utf8(plan.workingDirectory));
    if (application.empty() || working.empty() ||
        commandLine.size() > static_cast<std::size_t>(INT_MAX)) {
        result.state = EditorExternalLaunchState::LaunchFailed;
        result.error = "IDE launch paths or arguments are invalid";
        return result;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    const auto created =
        CreateProcessW(application.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
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
    const auto handle =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId);
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
    case EditorIdeKind::VisualStudio:
        return "Visual Studio";
    case EditorIdeKind::Rider:
        return "Rider";
    case EditorIdeKind::VisualStudioCode:
        return "VS Code";
    case EditorIdeKind::Clangd:
        return "clangd";
    case EditorIdeKind::Unknown:
        return "Unknown IDE";
    }
    return "Unknown IDE";
}

} // namespace shinkou::editor

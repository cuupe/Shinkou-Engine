#include "shinkou/editor/EditorProjectIntegration.h"

#include "shinkou/editor/EditorCompileCommands.h"
#include "shinkou/editor/FileSystem.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <system_error>

namespace shinkou::editor {
namespace {

std::string lower_ascii(std::string value) {
    for (auto& character : value)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

std::filesystem::path normalize_root(const std::filesystem::path& input) {
    std::error_code error;
    auto root = input.empty() ? std::filesystem::current_path(error) : input;
    if (error || root.empty()) return {};
    root = std::filesystem::weakly_canonical(root, error);
    if (error) root = std::filesystem::absolute(input, error);
    if (error || root.empty()) return {};
    return root.lexically_normal();
}

bool inside_root(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const auto base = lower_ascii(root.lexically_normal().generic_string());
    const auto value = lower_ascii(candidate.lexically_normal().generic_string());
    return !base.empty() && (value == base || value.rfind(base + '/', 0) == 0);
}

std::filesystem::path resolve_inside(const std::filesystem::path& root,
                                     const std::filesystem::path& input) {
    if (input.empty()) return {};
    const auto candidate = input.is_absolute() ? input : root / input;
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(candidate, error);
    if (error) return {};
    return inside_root(root, canonical) ? canonical.lexically_normal() : std::filesystem::path{};
}

std::filesystem::path relative_inside(const std::filesystem::path& root,
                                       const std::filesystem::path& input) {
    std::error_code error;
    const auto relative = std::filesystem::relative(input, root, error);
    if (error || relative.empty() || relative.is_absolute()) return {};
    const auto normalized = relative.lexically_normal();
    const auto text = normalized.generic_string();
    if (text == ".." || text.rfind("../", 0) == 0) return {};
    return normalized;
}

EditorProjectFileKind kind_for(const std::filesystem::path& path) noexcept {
    const auto name = lower_ascii(path.filename().generic_string());
    if (name == "cmakelists.txt") return EditorProjectFileKind::CMake;
    if (name == "meson.build") return EditorProjectFileKind::Meson;
    if (name == "cargo.toml") return EditorProjectFileKind::Cargo;
    if (name == "compile_commands.json") return EditorProjectFileKind::CompileCommands;
    if (name == ".clangd") return EditorProjectFileKind::ClangdConfig;
    const auto extension = lower_ascii(path.extension().generic_string());
    if (extension == ".sln" || extension == ".slnx") return EditorProjectFileKind::VisualStudioSolution;
    if (extension == ".vcxproj") return EditorProjectFileKind::VisualStudioProject;
    if (extension == ".csproj" || extension == ".fsproj" || extension == ".vbproj")
        return EditorProjectFileKind::DotNetProject;
    return EditorProjectFileKind::Unknown;
}

int discovery_priority(const EditorProjectFileDescriptor& file) noexcept {
    switch (file.kind) {
    case EditorProjectFileKind::VisualStudioSolution: return 0;
    case EditorProjectFileKind::CMake: return 1;
    case EditorProjectFileKind::VisualStudioProject: return 2;
    case EditorProjectFileKind::DotNetProject: return 3;
    case EditorProjectFileKind::Meson: return 4;
    case EditorProjectFileKind::Cargo: return 5;
    case EditorProjectFileKind::CompileCommands: return 6;
    case EditorProjectFileKind::ClangdConfig: return 7;
    case EditorProjectFileKind::Unknown: return 8;
    }
    return 8;
}

bool skipped_directory(std::string_view name) noexcept {
    return name == ".git" || name == ".shinkou" || name == "node_modules";
}

std::filesystem::path profile_project_file(const std::filesystem::path& root,
                                           const EditorBuildProfile* profile) {
    return profile ? resolve_inside(root, profile->projectFile) : std::filesystem::path{};
}

std::filesystem::path profile_compile_commands(const std::filesystem::path& root,
                                                const EditorBuildProfile* profile) {
    if (!profile || profile->buildDirectory.empty()) return {};
    const auto build = resolve_inside(root, profile->buildDirectory);
    if (build.empty()) return {};
    return build / "compile_commands.json";
}

bool is_regular_project_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

bool same_path(const std::filesystem::path& left, const std::filesystem::path& right) {
    return lower_ascii(left.lexically_normal().generic_string()) ==
        lower_ascii(right.lexically_normal().generic_string());
}

void set_error(std::string* error, std::string value) {
    if (error) *error = std::move(value);
}

std::string yaml_single_quote(std::string_view value) {
    std::string result{"'"};
    result.reserve(value.size() + 2);
    for (const auto character : value) {
        if (character == '\0' || character == '\r' || character == '\n') return {};
        if (character == '\'') result += "''";
        else result.push_back(character);
    }
    result.push_back('\'');
    return result;
}

} // namespace

EditorProjectDiscoveryResult EditorProjectIntegration::discover(
    const std::filesystem::path& projectRoot, const EditorBuildProfile* profile) {
    EditorProjectDiscoveryResult result;
    const auto root = normalize_root(projectRoot);
    if (root.empty()) {
        result.error = "Project root is unavailable";
        return result;
    }
    std::error_code rootError;
    if (!std::filesystem::is_directory(root, rootError) || rootError) {
        result.error = "Project root is not a directory";
        return result;
    }
    if (profile && !profile->buildDirectory.empty() &&
        resolve_inside(root, profile->buildDirectory).empty()) {
        result.error = "Build profile directory is outside the project root";
        return result;
    }

    auto append = [&](const std::filesystem::path& absolute) {
        if (result.files.size() >= MaxFiles || !is_regular_project_file(absolute)) return;
        const auto resolved = resolve_inside(root, absolute);
        const auto relative = relative_inside(root, resolved);
        if (resolved.empty() || relative.empty()) return;
        const auto kind = kind_for(relative);
        if (kind == EditorProjectFileKind::Unknown) return;
        if (std::find_if(result.files.begin(), result.files.end(), [&](const auto& file) {
                return same_path(file.relativePath, relative);
            }) != result.files.end()) return;
        result.files.push_back({kind, relative, false});
    };

    // Prioritize profile/root compile databases before the bounded walk so a
    // large source tree cannot hide the file needed to configure clangd.
    append(profile_compile_commands(root, profile));
    append(root / "compile_commands.json");

    std::error_code error;
    std::size_t scanned = 0;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end && !error && scanned < MaxScanEntries; iterator.increment(error)) {
        ++scanned;
        const auto& entry = *iterator;
        std::error_code entryError;
        if (entry.is_directory(entryError)) {
            if (skipped_directory(lower_ascii(entry.path().filename().generic_string())) ||
                iterator.depth() >= static_cast<int>(MaxScanDepth)) iterator.disable_recursion_pending();
            continue;
        }
        if (!entryError && entry.is_regular_file(entryError)) append(entry.path());
    }

    std::sort(result.files.begin(), result.files.end(), [](const auto& left, const auto& right) {
        const auto leftPriority = discovery_priority(left);
        const auto rightPriority = discovery_priority(right);
        if (leftPriority != rightPriority) return leftPriority < rightPriority;
        return left.relativePath.generic_string() < right.relativePath.generic_string();
    });

    const auto preferred = profile_project_file(root, profile);
    if (!preferred.empty()) {
        const auto preferredRelative = relative_inside(root, preferred);
        const auto found = std::find_if(result.files.begin(), result.files.end(), [&](const auto& file) {
            return same_path(file.relativePath, preferredRelative);
        });
        if (found != result.files.end()) result.recommendedProjectFile = found->relativePath;
    }
    if (result.recommendedProjectFile.empty()) {
        const auto rootSolution = std::find_if(result.files.begin(), result.files.end(), [](const auto& file) {
            return file.kind == EditorProjectFileKind::VisualStudioSolution && file.relativePath.parent_path().empty();
        });
        if (rootSolution != result.files.end()) result.recommendedProjectFile = rootSolution->relativePath;
    }
    if (result.recommendedProjectFile.empty()) {
        const auto rootCmake = std::find_if(result.files.begin(), result.files.end(), [](const auto& file) {
            return file.kind == EditorProjectFileKind::CMake && file.relativePath.parent_path().empty();
        });
        if (rootCmake != result.files.end()) result.recommendedProjectFile = rootCmake->relativePath;
    }
    if (result.recommendedProjectFile.empty()) {
        const auto firstProject = std::find_if(result.files.begin(), result.files.end(), [](const auto& file) {
            return file.kind == EditorProjectFileKind::VisualStudioSolution ||
                file.kind == EditorProjectFileKind::VisualStudioProject ||
                file.kind == EditorProjectFileKind::DotNetProject ||
                file.kind == EditorProjectFileKind::CMake;
        });
        if (firstProject != result.files.end()) result.recommendedProjectFile = firstProject->relativePath;
    }

    const auto preferredDatabase = profile_compile_commands(root, profile);
    if (!preferredDatabase.empty() && is_regular_project_file(preferredDatabase)) {
        result.compileCommandsFile = relative_inside(root, preferredDatabase);
    }
    if (result.compileCommandsFile.empty() && is_regular_project_file(root / "compile_commands.json")) {
        result.compileCommandsFile = "compile_commands.json";
    }
    if (result.compileCommandsFile.empty()) {
        const auto database = std::find_if(result.files.begin(), result.files.end(), [](const auto& file) {
            return file.kind == EditorProjectFileKind::CompileCommands;
        });
        if (database != result.files.end()) result.compileCommandsFile = database->relativePath;
    }
    for (auto& file : result.files) file.recommended = !result.recommendedProjectFile.empty() &&
        same_path(file.relativePath, result.recommendedProjectFile);
    result.valid = true;
    if (scanned >= MaxScanEntries) result.error = "Project discovery reached its bounded scan limit";
    return result;
}

EditorClangdConfigPlan EditorProjectIntegration::plan_clangd_config(
    const std::filesystem::path& projectRoot, const EditorBuildProfile* profile) {
    EditorClangdConfigPlan result;
    const auto root = normalize_root(projectRoot);
    if (root.empty()) { result.error = "Project root is unavailable"; return result; }
    const auto discovery = discover(root, profile);
    if (!discovery.valid) { result.error = discovery.error; return result; }
    if (discovery.compileCommandsFile.empty()) {
        result.error = "No compile_commands.json was discovered";
        return result;
    }
    const auto database = resolve_inside(root, discovery.compileCommandsFile);
    if (database.empty() || !is_regular_project_file(database)) {
        result.error = "Discovered compile_commands.json is not a project file";
        return result;
    }
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(database, sizeError);
    if (sizeError || size > 16u * 1024u * 1024u) {
        result.error = "compile_commands.json is unavailable or exceeds 16 MiB";
        return result;
    }
    std::ifstream file(database, std::ios::binary);
    if (!file) { result.error = "Cannot read compile_commands.json"; return result; }
    std::string json(static_cast<std::size_t>(size), '\0');
    if (size != 0 && !file.read(json.data(), static_cast<std::streamsize>(size))) {
        result.error = "Cannot read compile_commands.json";
        return result;
    }
    const auto parsed = EditorCompileCommands::parse(json, root);
    if (!parsed.valid) {
        result.error = "compile_commands.json is invalid: " + parsed.error;
        return result;
    }
    const auto relativeDatabase = relative_inside(root, database);
    const auto relativeDirectory = relativeDatabase.parent_path().empty() ?
        std::filesystem::path{"."} : relativeDatabase.parent_path();
    const auto quoted = yaml_single_quote(relativeDirectory.generic_string());
    if (quoted.empty()) { result.error = "compile database path contains unsupported YAML characters"; return result; }
    result.compileCommandsFile = relativeDatabase;
    result.content = "# Generated by Shinkou Editor.\n"
                     "CompileFlags:\n"
                     "  CompilationDatabase: " + quoted + "\n";
    result.valid = true;
    return result;
}

bool EditorProjectIntegration::write_clangd_config(
    const std::filesystem::path& projectRoot, const EditorClangdConfigPlan& plan,
    std::string* error) {
    if (!plan.valid || plan.content.empty()) {
        set_error(error, "clangd config plan is invalid");
        return false;
    }
    if (plan.outputPath != std::filesystem::path{".clangd"}) {
        set_error(error, "clangd config output must be the project-root .clangd file");
        return false;
    }
    const auto root = normalize_root(projectRoot);
    if (root.empty()) {
        set_error(error, "project root is unavailable");
        return false;
    }
    FileSystemService fileSystem(root);
    if (!fileSystem.write_text_atomic(plan.outputPath, plan.content, error)) return false;
    return true;
}

std::string_view EditorProjectIntegration::file_kind_name(EditorProjectFileKind kind) noexcept {
    switch (kind) {
    case EditorProjectFileKind::CMake: return "CMake";
    case EditorProjectFileKind::VisualStudioSolution: return "Visual Studio solution";
    case EditorProjectFileKind::VisualStudioProject: return "Visual Studio project";
    case EditorProjectFileKind::DotNetProject: return ".NET project";
    case EditorProjectFileKind::Meson: return "Meson";
    case EditorProjectFileKind::Cargo: return "Cargo";
    case EditorProjectFileKind::CompileCommands: return "compile_commands";
    case EditorProjectFileKind::ClangdConfig: return "clangd config";
    case EditorProjectFileKind::Unknown: return "Unknown";
    }
    return "Unknown";
}

} // namespace shinkou::editor

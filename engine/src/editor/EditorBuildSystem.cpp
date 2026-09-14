#include "shinkou/editor/EditorBuildSystem.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <cwchar>
#include <cstdlib>
#include <iterator>
#include <regex>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace shinkou::editor {
namespace {

struct ToolCandidate {
    EditorToolKind kind;
    const char* name;
    const char* const* executableNames;
    std::size_t executableNameCount;
};

constexpr const char* kCMakeNames[] = {"cmake", "cmake.exe"};
constexpr const char* kNinjaNames[] = {"ninja", "ninja.exe"};
constexpr const char* kMSBuildNames[] = {"MSBuild", "MSBuild.exe", "msbuild", "msbuild.exe"};
constexpr const char* kClangNames[] = {"clang", "clang.exe"};
constexpr const char* kClangClNames[] = {"clang-cl", "clang-cl.exe"};
constexpr const char* kGccNames[] = {"gcc", "gcc.exe"};
constexpr const char* kGxxNames[] = {"g++", "g++.exe"};
constexpr const char* kDotNetNames[] = {"dotnet", "dotnet.exe"};

constexpr ToolCandidate kCandidates[] = {
    {EditorToolKind::CMake, "CMake", kCMakeNames, std::size(kCMakeNames)},
    {EditorToolKind::Ninja, "Ninja", kNinjaNames, std::size(kNinjaNames)},
    {EditorToolKind::MSBuild, "MSBuild", kMSBuildNames, std::size(kMSBuildNames)},
    {EditorToolKind::Clang, "Clang", kClangNames, std::size(kClangNames)},
    {EditorToolKind::ClangCl, "clang-cl", kClangClNames, std::size(kClangClNames)},
    {EditorToolKind::GCC, "GCC", kGccNames, std::size(kGccNames)},
    {EditorToolKind::GXX, "G++", kGxxNames, std::size(kGxxNames)},
    {EditorToolKind::DotNet, ".NET SDK", kDotNetNames, std::size(kDotNetNames)},
};

char path_separator() noexcept {
#if defined(_WIN32)
    return ';';
#else
    return ':';
#endif
}

std::filesystem::path normalize_root(std::filesystem::path root) {
    std::error_code error;
    if (root.empty()) root = std::filesystem::current_path(error);
    if (root.empty()) return {};
    const auto absolute = std::filesystem::absolute(root, error);
    return (error ? root : absolute).lexically_normal();
}

bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const auto rootString = root.lexically_normal().generic_string();
    const auto candidateString = candidate.lexically_normal().generic_string();
    if (rootString.empty() || candidateString == rootString) return true;
    return candidateString.rfind(rootString + '/', 0) == 0;
}

std::filesystem::path resolve_project_path(const std::filesystem::path& root,
                                           const std::filesystem::path& input) {
    if (input.empty()) return {};
    const auto candidate = input.is_absolute() ? input.lexically_normal() :
        (root / input).lexically_normal();
    return is_within(root, candidate) ? candidate : std::filesystem::path{};
}

std::string relative_or_empty(const std::filesystem::path& root,
                              const std::filesystem::path& input) {
    const auto resolved = resolve_project_path(root, input);
    if (resolved.empty()) return {};
    std::error_code error;
    const auto relative = std::filesystem::relative(resolved, root, error);
    if (error || relative.empty()) return ".";
    return relative.generic_string();
}

std::filesystem::path find_executable(const std::vector<std::filesystem::path>& searchDirectories,
                                      const ToolCandidate& candidate) {
    for (const auto& directory : searchDirectories) {
        if (directory.empty()) continue;
        for (std::size_t index = 0; index < candidate.executableNameCount; ++index) {
            const auto path = (directory / candidate.executableNames[index]).lexically_normal();
            std::error_code error;
            if (std::filesystem::is_regular_file(path, error) && !error) return path;
        }
    }
    return {};
}

std::vector<std::filesystem::path> split_search_path(std::string_view searchPath) {
    std::vector<std::filesystem::path> result;
    std::size_t begin = 0;
    while (begin <= searchPath.size()) {
        const auto end = searchPath.find(path_separator(), begin);
        const auto part = searchPath.substr(begin, end == std::string_view::npos ?
            searchPath.size() - begin : end - begin);
        if (!part.empty()) result.emplace_back(std::string(part));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return result;
}

EditorDiagnosticSeverity severity_from_string(std::string_view value) noexcept {
    if (value == "warning") return EditorDiagnosticSeverity::Warning;
    if (value == "error" || value == "fatal error") return EditorDiagnosticSeverity::Error;
    return EditorDiagnosticSeverity::Note;
}

std::filesystem::path normalize_diagnostic_path(const std::filesystem::path& root,
                                                std::string value) {
    while (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                 (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    const auto input = std::filesystem::path(value);
    if (root.empty() || !input.is_absolute()) return input.lexically_normal();
    std::error_code error;
    const auto relative = std::filesystem::relative(input, root, error);
    if (!error && !relative.empty() && relative.generic_string().rfind("../", 0) != 0) {
        return relative.lexically_normal();
    }
    return input.lexically_normal();
}

bool parse_size(std::string_view value, std::size_t& output) noexcept {
    if (value.empty()) return false;
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, output);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

void append_arguments(std::vector<std::string>& destination,
                      const std::vector<std::string>& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

bool has_nul(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

std::string invalid_profile_message(std::string_view field) {
    return "Build profile contains an unsafe or empty " + std::string(field);
}

} // namespace

EditorBuildSystem::EditorBuildSystem(std::filesystem::path projectRoot) {
    set_project_root(std::move(projectRoot));
}

void EditorBuildSystem::set_project_root(std::filesystem::path projectRoot) {
    projectRoot_ = normalize_root(std::move(projectRoot));
}

void EditorBuildSystem::refresh_toolchains(std::string_view searchPath) {
    toolchains_ = discover_toolchains(projectRoot_, searchPath);
}

std::vector<EditorToolDescriptor> EditorBuildSystem::discover_toolchains(
    const std::filesystem::path& projectRoot, std::string_view searchPath) {
    const auto root = normalize_root(projectRoot);
    std::vector<std::filesystem::path> directories;
    if (!root.empty()) {
        directories.emplace_back(root / ".shinkou" / "tools");
        directories.emplace_back(root / "Tools");
    }
    if (searchPath.empty()) {
        const char* environment = std::getenv("PATH");
        if (environment) searchPath = environment;
    }
    const auto environmentDirectories = split_search_path(searchPath);
    directories.insert(directories.end(), environmentDirectories.begin(), environmentDirectories.end());

    std::vector<EditorToolDescriptor> result;
    result.reserve(std::size(kCandidates));
    for (const auto& candidate : kCandidates) {
        const auto executable = find_executable(directories, candidate);
        result.push_back({candidate.kind, candidate.name, executable, !executable.empty()});
    }
    return result;
}

const EditorToolDescriptor* EditorBuildSystem::find_tool(EditorToolKind kind) const noexcept {
    const auto found = std::find_if(toolchains_.begin(), toolchains_.end(), [kind](const auto& tool) {
        return tool.kind == kind;
    });
    return found == toolchains_.end() ? nullptr : &*found;
}

EditorBuildPlan EditorBuildSystem::plan(const EditorBuildProfile& profile) const {
    EditorBuildPlan result;
    if (projectRoot_.empty()) {
        result.error = "Build project root is not configured";
        return result;
    }
    if (profile.id.empty() || profile.name.empty()) {
        result.error = invalid_profile_message("profile identity");
        return result;
    }
    const auto source = relative_or_empty(projectRoot_, profile.sourceDirectory);
    const auto build = relative_or_empty(projectRoot_, profile.buildDirectory);
    if (source.empty()) {
        result.error = invalid_profile_message("source directory");
        return result;
    }
    if (build.empty() || build == ".") {
        result.error = invalid_profile_message("build directory");
        return result;
    }
    if (profile.configuration.find('\0') != std::string::npos || profile.generator.find('\0') != std::string::npos ||
        profile.target.find('\0') != std::string::npos) {
        result.error = "Build profile contains a NUL character";
        return result;
    }
    for (const auto& argument : profile.configureArguments) {
        if (has_nul(argument)) { result.error = "Configure arguments contain a NUL character"; return result; }
    }
    for (const auto& argument : profile.buildArguments) {
        if (has_nul(argument)) { result.error = "Build arguments contain a NUL character"; return result; }
    }

    const auto* configureTool = find_tool(profile.configureTool);
    const auto* buildTool = find_tool(profile.buildTool);
    if (!configureTool || !configureTool->available) {
        result.error = "Configure tool is not available: " + std::string(tool_name(profile.configureTool));
        return result;
    }
    if (!buildTool || !buildTool->available) {
        result.error = "Build tool is not available: " + std::string(tool_name(profile.buildTool));
        return result;
    }

    EditorBuildCommand configure;
    configure.step = EditorBuildStep::Configure;
    configure.tool = profile.configureTool;
    configure.executable = configureTool->executable;
    configure.workingDirectory = projectRoot_;
    if (profile.configureTool != EditorToolKind::CMake) {
        result.error = "Only CMake configure planning is supported in this round";
        return result;
    }
    configure.arguments = {"-S", source, "-B", build};
    if (!profile.generator.empty()) configure.arguments.insert(configure.arguments.end(), {"-G", profile.generator});
    append_arguments(configure.arguments, profile.configureArguments);

    EditorBuildCommand buildCommand;
    buildCommand.step = EditorBuildStep::Build;
    buildCommand.tool = profile.buildTool;
    buildCommand.executable = buildTool->executable;
    buildCommand.workingDirectory = projectRoot_;
    switch (profile.buildTool) {
    case EditorToolKind::CMake:
        buildCommand.arguments = {"--build", build};
        if (!profile.configuration.empty()) buildCommand.arguments.insert(buildCommand.arguments.end(), {"--config", profile.configuration});
        if (!profile.target.empty()) buildCommand.arguments.insert(buildCommand.arguments.end(), {"--target", profile.target});
        break;
    case EditorToolKind::Ninja:
        buildCommand.workingDirectory = projectRoot_ / build;
        buildCommand.arguments = {profile.target.empty() ? "all" : profile.target};
        break;
    case EditorToolKind::MSBuild: {
        const auto projectFile = relative_or_empty(projectRoot_, profile.projectFile);
        if (projectFile.empty() || projectFile == ".") {
            result.error = invalid_profile_message("MSBuild project file");
            return result;
        }
        buildCommand.arguments = {projectFile, "/t:Build"};
        if (!profile.configuration.empty()) buildCommand.arguments.push_back("/p:Configuration=" + profile.configuration);
        break;
    }
    case EditorToolKind::DotNet: {
        const auto projectFile = relative_or_empty(projectRoot_, profile.projectFile);
        if (projectFile.empty() || projectFile == ".") {
            result.error = invalid_profile_message(".NET project file");
            return result;
        }
        buildCommand.arguments = {"build", projectFile};
        if (!profile.configuration.empty()) buildCommand.arguments.insert(buildCommand.arguments.end(), {"--configuration", profile.configuration});
        break;
    }
    default:
        result.error = "Build tool is discovered but has no profile adapter: " + std::string(tool_name(profile.buildTool));
        return result;
    }
    append_arguments(buildCommand.arguments, profile.buildArguments);
    result.valid = true;
    result.commands = {std::move(configure), std::move(buildCommand)};
    return result;
}

std::vector<EditorBuildDiagnostic> EditorBuildSystem::parse_diagnostics(
    std::string_view output, const std::filesystem::path& projectRoot) {
    static const std::regex msvc(
        R"(^(.+)\(([0-9]+)(?:,([0-9]+))?\):\s*(fatal error|error|warning|note)\b\s*([A-Za-z0-9_-]+)?\s*:?[\s]*(.*)$)");
    static const std::regex gcc(
        R"(^(.+):([0-9]+):([0-9]+):\s*(fatal error|error|warning|note):\s*(.*)$)");
    static const std::regex gccNoColumn(
        R"(^(.+):([0-9]+):\s*(fatal error|error|warning|note):\s*(.*)$)");
    std::vector<EditorBuildDiagnostic> result;
    std::size_t begin = 0;
    while (begin <= output.size()) {
        const auto end = output.find('\n', begin);
        std::string line(output.substr(begin, end == std::string_view::npos ? output.size() - begin : end - begin));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch match;
        EditorBuildDiagnostic diagnostic;
        if (std::regex_match(line, match, msvc)) {
            std::size_t lineNumber = 0;
            std::size_t columnNumber = 0;
            if (!parse_size(match[2].str(), lineNumber) ||
                (match[3].matched && !parse_size(match[3].str(), columnNumber))) {
                if (end == std::string_view::npos) break;
                begin = end + 1;
                continue;
            }
            diagnostic.file = normalize_diagnostic_path(projectRoot, match[1].str());
            diagnostic.line = lineNumber;
            diagnostic.column = columnNumber;
            diagnostic.severity = severity_from_string(match[4].str());
            diagnostic.code = match[5].matched ? match[5].str() : std::string{};
            diagnostic.message = match[6].str();
        } else if (std::regex_match(line, match, gcc)) {
            std::size_t lineNumber = 0;
            std::size_t columnNumber = 0;
            if (!parse_size(match[2].str(), lineNumber) || !parse_size(match[3].str(), columnNumber)) {
                if (end == std::string_view::npos) break;
                begin = end + 1;
                continue;
            }
            diagnostic.file = normalize_diagnostic_path(projectRoot, match[1].str());
            diagnostic.line = lineNumber;
            diagnostic.column = columnNumber;
            diagnostic.severity = severity_from_string(match[4].str());
            diagnostic.message = match[5].str();
        } else if (std::regex_match(line, match, gccNoColumn)) {
            std::size_t lineNumber = 0;
            if (!parse_size(match[2].str(), lineNumber)) {
                if (end == std::string_view::npos) break;
                begin = end + 1;
                continue;
            }
            diagnostic.file = normalize_diagnostic_path(projectRoot, match[1].str());
            diagnostic.line = lineNumber;
            diagnostic.severity = severity_from_string(match[3].str());
            diagnostic.message = match[4].str();
        }
        if (!diagnostic.message.empty()) result.push_back(std::move(diagnostic));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return result;
}

std::string_view EditorBuildSystem::tool_name(EditorToolKind kind) noexcept {
    switch (kind) {
    case EditorToolKind::CMake: return "CMake";
    case EditorToolKind::Ninja: return "Ninja";
    case EditorToolKind::MSBuild: return "MSBuild";
    case EditorToolKind::Clang: return "Clang";
    case EditorToolKind::ClangCl: return "clang-cl";
    case EditorToolKind::GCC: return "GCC";
    case EditorToolKind::GXX: return "G++";
    case EditorToolKind::DotNet: return ".NET SDK";
    case EditorToolKind::Unknown: return "Unknown";
    }
    return "Unknown";
}

#if defined(_WIN32)
namespace {

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<std::size_t>(INT_MAX)) return {};
    const auto length = static_cast<int>(value.size());
    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (required <= 0) required = MultiByteToWideChar(CP_ACP, 0, value.data(), length, nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, result.data(), required) <= 0 &&
        MultiByteToWideChar(CP_ACP, 0, value.data(), length, result.data(), required) <= 0) return {};
    return result;
}

std::wstring quote_windows_argument(std::string_view argument) {
    const auto value = utf8_to_wide(argument);
    const bool needsQuotes = value.empty() || value.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needsQuotes) return value;
    std::wstring result;
    result.reserve(value.size() + 2);
    result.push_back(L'"');
    std::size_t backslashes = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
            backslashes = 0;
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

bool equal_key(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto a = left[index] >= L'a' && left[index] <= L'z' ? left[index] - (L'a' - L'A') : left[index];
        const auto b = right[index] >= L'a' && right[index] <= L'z' ? right[index] - (L'a' - L'A') : right[index];
        if (a != b) return false;
    }
    return true;
}

bool baseline_environment_key(std::wstring_view key) noexcept {
    static constexpr std::wstring_view names[] = {
        L"PATH", L"PATHEXT", L"SYSTEMROOT", L"TEMP", L"TMP", L"COMSPEC",
        L"USERPROFILE", L"HOMEDRIVE", L"HOMEPATH", L"NUMBER_OF_PROCESSORS",
        L"PROCESSOR_ARCHITECTURE",
    };
    for (const auto name : names) if (equal_key(key, name)) return true;
    return false;
}

bool allowed_environment_key(std::wstring_view key, const EditorBuildProcessOptions& options) {
    if (!options.environmentAllowList.empty()) {
        for (const auto& allowed : options.environmentAllowList) {
            if (equal_key(key, utf8_to_wide(allowed))) return true;
        }
        return false;
    }
    return baseline_environment_key(key);
}

std::wstring make_environment_block(const EditorBuildProcessOptions& options) {
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    if (options.inheritEnvironment) {
        LPWCH environment = GetEnvironmentStringsW();
        if (environment) {
            for (const wchar_t* cursor = environment; *cursor != L'\0'; cursor += std::wcslen(cursor) + 1) {
                const std::wstring item(cursor);
                const auto separator = item.find(L'=');
                if (separator == std::wstring::npos) continue;
                const auto key = item.substr(0, separator);
                if (allowed_environment_key(key, options)) entries.emplace_back(key, item.substr(separator + 1));
            }
            FreeEnvironmentStringsW(environment);
        }
    }
    for (const auto& overrideValue : options.environmentOverrides) {
        const auto key = utf8_to_wide(overrideValue.first);
        const auto value = utf8_to_wide(overrideValue.second);
        if (key.empty() || key.find(L'=') != std::wstring::npos) continue;
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
            return equal_key(entry.first, key);
        });
        if (found == entries.end()) entries.emplace_back(key, value);
        else found->second = value;
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    std::wstring block;
    for (const auto& entry : entries) {
        block.append(entry.first);
        block.push_back(L'=');
        block.append(entry.second);
        block.push_back(L'\0');
    }
    if (!block.empty()) block.push_back(L'\0');
    return block;
}

struct PipeReadState {
    HANDLE handle{nullptr};
    bool open{false};
};

void close_handle(HANDLE& handle) noexcept {
    if (handle) CloseHandle(handle);
    handle = nullptr;
}

void drain_pipe(PipeReadState& pipe, std::string& destination, std::size_t limit,
                const std::function<void(EditorBuildOutputStream, std::string_view)>& callback,
                EditorBuildOutputStream stream) {
    if (!pipe.open || !pipe.handle) return;
    while (pipe.open) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe.handle, nullptr, 0, nullptr, &available, nullptr)) {
            pipe.open = false;
            close_handle(pipe.handle);
            return;
        }
        if (available == 0) return;
        std::array<char, 4096> buffer{};
        const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        DWORD read = 0;
        if (!ReadFile(pipe.handle, buffer.data(), requested, &read, nullptr) || read == 0) {
            pipe.open = false;
            close_handle(pipe.handle);
            return;
        }
        if (destination.size() < limit) {
            const auto count = std::min<std::size_t>(read, limit - destination.size());
            destination.append(buffer.data(), count);
        }
        if (callback) {
            try {
                callback(stream, std::string_view(buffer.data(), read));
            } catch (...) {
                // Output observers are diagnostic/UI helpers. A faulty
                // observer must never prevent pipe draining or leak a child
                // process handle out of the runner.
            }
        }
    }
}

} // namespace
#endif

EditorBuildProcessResult EditorBuildProcess::run(const EditorBuildCommand& command,
                                                 const EditorBuildProcessOptions& options,
                                                 std::atomic_bool& cancelRequested) {
    EditorBuildProcessResult result;
    if (command.executable.empty()) {
        result.state = EditorBuildProcessState::LaunchFailed;
        result.error = "Build command has no executable";
        return result;
    }
#if !defined(_WIN32)
    (void)command;
    (void)options;
    (void)cancelRequested;
    result.state = EditorBuildProcessState::Unsupported;
    result.error = "Editor build process execution is currently supported on Windows only";
    return result;
#else
    if (cancelRequested.load(std::memory_order_relaxed)) {
        result.state = EditorBuildProcessState::Cancelled;
        result.error = "Build cancelled before launch";
        return result;
    }
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE stdoutRead = nullptr, stdoutWrite = nullptr, stderrRead = nullptr, stderrWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &security, 0) ||
        !CreatePipe(&stderrRead, &stderrWrite, &security, 0) ||
        !SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0)) {
        close_handle(stdoutRead); close_handle(stdoutWrite); close_handle(stderrRead); close_handle(stderrWrite);
        result.state = EditorBuildProcessState::LaunchFailed;
        result.error = "Unable to create build output pipes";
        return result;
    }

    std::wstring commandLine = quote_windows_argument(command.executable.string());
    for (const auto& argument : command.arguments) {
        commandLine.push_back(L' ');
        commandLine += quote_windows_argument(argument);
    }
    auto environment = make_environment_block(options);
    const auto workingDirectory = command.workingDirectory.empty() ?
        std::filesystem::current_path() : command.workingDirectory;
    const auto application = utf8_to_wide(command.executable.string());
    const auto working = utf8_to_wide(workingDirectory.generic_string());
    if (application.empty() || working.empty() || commandLine.size() > static_cast<std::size_t>(INT_MAX)) {
        close_handle(stdoutRead); close_handle(stdoutWrite); close_handle(stderrRead); close_handle(stderrWrite);
        result.state = EditorBuildProcessState::LaunchFailed;
        result.error = "Build command contains an invalid Windows path";
        return result;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = stderrWrite;
    PROCESS_INFORMATION process{};
    auto mutableCommandLine = commandLine;
    const BOOL created = CreateProcessW(application.c_str(), mutableCommandLine.data(), nullptr, nullptr, TRUE,
        CREATE_UNICODE_ENVIRONMENT, environment.empty() ? nullptr : environment.data(), working.c_str(), &startup, &process);
    close_handle(stdoutWrite);
    close_handle(stderrWrite);
    if (!created) {
        close_handle(stdoutRead); close_handle(stderrRead);
        result.state = EditorBuildProcessState::LaunchFailed;
        result.error = "CreateProcessW failed: " + std::to_string(GetLastError());
        return result;
    }
    close_handle(process.hThread);
    PipeReadState output{stdoutRead, true};
    PipeReadState error{stderrRead, true};
    const auto started = std::chrono::steady_clock::now();
    bool terminated = false;
    while (output.open || error.open) {
        drain_pipe(output, result.standardOutput, options.outputLimitBytes,
                   options.outputCallback, EditorBuildOutputStream::StandardOutput);
        drain_pipe(error, result.standardError, options.outputLimitBytes,
                   options.outputCallback, EditorBuildOutputStream::StandardError);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (!terminated && cancelRequested.load(std::memory_order_relaxed)) {
            TerminateProcess(process.hProcess, 1223u);
            terminated = true;
            result.state = EditorBuildProcessState::Cancelled;
        } else if (!terminated && options.timeout.count() > 0 && elapsed >= options.timeout) {
            TerminateProcess(process.hProcess, 1460u);
            terminated = true;
            result.state = EditorBuildProcessState::TimedOut;
            result.error = "Build process timed out";
        }
        const auto wait = WaitForSingleObject(process.hProcess, 20u);
        if (wait == WAIT_FAILED) {
            if (!terminated) result.error = "WaitForSingleObject failed: " + std::to_string(GetLastError());
            terminated = true;
        }
        if (wait == WAIT_OBJECT_0 && !output.open && !error.open) break;
    }
    close_handle(output.handle);
    close_handle(error.handle);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    close_handle(process.hProcess);
    result.exitCode = exitCode;
    if (result.state == EditorBuildProcessState::Cancelled || result.state == EditorBuildProcessState::TimedOut) {
        // Preserve the terminal cancellation/timeout state selected above.
    } else if (exitCode == 0) {
        result.state = EditorBuildProcessState::Succeeded;
    } else {
        result.state = EditorBuildProcessState::Failed;
        if (result.error.empty()) result.error = "Build process exited with code " + std::to_string(exitCode);
    }
    std::string diagnosticsText = result.standardOutput;
    if (!result.standardError.empty()) {
        if (!diagnosticsText.empty()) diagnosticsText.push_back('\n');
        diagnosticsText += result.standardError;
    }
    result.diagnostics = EditorBuildSystem::parse_diagnostics(diagnosticsText, options.diagnosticRoot);
    return result;
#endif
}

} // namespace shinkou::editor

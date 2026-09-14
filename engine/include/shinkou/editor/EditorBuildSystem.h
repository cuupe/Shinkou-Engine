#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shinkou::editor {

enum class EditorToolKind : std::uint8_t {
    CMake,
    Ninja,
    MSBuild,
    Clang,
    ClangCl,
    GCC,
    GXX,
    DotNet,
    Unknown,
};

enum class EditorBuildStep : std::uint8_t {
    Configure,
    Build,
};

enum class EditorDiagnosticSeverity : std::uint8_t {
    Note,
    Warning,
    Error,
};

struct EditorToolDescriptor {
    EditorToolKind kind{EditorToolKind::Unknown};
    std::string name;
    std::filesystem::path executable;
    bool available{false};
};

// A profile contains data, not a shell command. Arguments remain separate
// strings until a platform process runner consumes them, which prevents the
// editor from accidentally turning project data into shell syntax.
struct EditorBuildProfile {
    std::string id{"default"};
    std::string name{"CMake Debug"};
    EditorToolKind configureTool{EditorToolKind::CMake};
    EditorToolKind buildTool{EditorToolKind::CMake};
    std::filesystem::path sourceDirectory{"."};
    std::filesystem::path buildDirectory{"out/build/editor"};
    std::filesystem::path projectFile{};
    std::string generator;
    std::string configuration{"Debug"};
    std::string target;
    std::vector<std::string> configureArguments;
    std::vector<std::string> buildArguments;
};

struct EditorBuildCommand {
    EditorBuildStep step{EditorBuildStep::Build};
    EditorToolKind tool{EditorToolKind::Unknown};
    std::filesystem::path executable;
    std::filesystem::path workingDirectory;
    std::vector<std::string> arguments;
};

struct EditorBuildPlan {
    bool valid{false};
    std::string error;
    std::vector<EditorBuildCommand> commands;
};

struct EditorBuildDiagnostic {
    EditorDiagnosticSeverity severity{EditorDiagnosticSeverity::Note};
    std::filesystem::path file;
    std::size_t line{0};
    std::size_t column{0};
    std::string code;
    std::string message;
};

enum class EditorBuildOutputStream : std::uint8_t {
    StandardOutput,
    StandardError,
};

struct EditorBuildProcessOptions {
    std::chrono::milliseconds timeout{std::chrono::minutes(10)};
    std::size_t outputLimitBytes{4u * 1024u * 1024u};
    std::filesystem::path diagnosticRoot;
    bool inheritEnvironment{true};
    // An empty allow-list uses a small OS-safe baseline. Toolchain-specific
    // variables such as INCLUDE/LIB can be explicitly added by the profile
    // owner without exposing the full editor environment to child processes.
    std::vector<std::string> environmentAllowList;
    std::vector<std::pair<std::string, std::string>> environmentOverrides;
    // Called from the process worker thread with bounded pipe chunks. The
    // callback must copy data it needs and must not touch UI objects directly.
    std::function<void(EditorBuildOutputStream, std::string_view)> outputCallback;
};

enum class EditorBuildProcessState : std::uint8_t {
    NotStarted,
    Succeeded,
    Failed,
    Cancelled,
    TimedOut,
    LaunchFailed,
    Unsupported,
};

struct EditorBuildProcessResult {
    EditorBuildProcessState state{EditorBuildProcessState::NotStarted};
    std::uint32_t exitCode{0};
    std::string error;
    std::string standardOutput;
    std::string standardError;
    std::vector<EditorBuildDiagnostic> diagnostics;
};

class EditorBuildProcess final {
public:
    static EditorBuildProcessResult run(const EditorBuildCommand& command,
                                        const EditorBuildProcessOptions& options,
                                        std::atomic_bool& cancelRequested);
};

class EditorBuildSystem final {
public:
    explicit EditorBuildSystem(std::filesystem::path projectRoot = {});

    void set_project_root(std::filesystem::path projectRoot);
    const std::filesystem::path& project_root() const noexcept { return projectRoot_; }

    // Refresh is intentionally explicit. It may inspect PATH and a small
    // project-local tools directory, so callers must not invoke it from paint.
    void refresh_toolchains(std::string_view searchPath = {});
    const std::vector<EditorToolDescriptor>& toolchains() const noexcept { return toolchains_; }

    EditorBuildPlan plan(const EditorBuildProfile& profile) const;

    static std::vector<EditorToolDescriptor> discover_toolchains(
        const std::filesystem::path& projectRoot, std::string_view searchPath = {});
    static std::vector<EditorBuildDiagnostic> parse_diagnostics(
        std::string_view output, const std::filesystem::path& projectRoot = {});
    static std::string_view tool_name(EditorToolKind kind) noexcept;

private:
    std::filesystem::path projectRoot_;
    std::vector<EditorToolDescriptor> toolchains_;

    const EditorToolDescriptor* find_tool(EditorToolKind kind) const noexcept;
};

} // namespace shinkou::editor

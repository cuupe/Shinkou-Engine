#pragma once

#include "shinkou/editor/EditorBuildSystem.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::editor {

struct EditorBuildProfileLoadResult {
    bool valid{false};
    std::string error;
    EditorBuildProfile profile{};
};

struct EditorBuildProfileSet {
    std::vector<EditorBuildProfile> profiles;
    std::string selectedId{"default"};
};

struct EditorBuildProfileSetLoadResult {
    bool valid{false};
    std::string error;
    EditorBuildProfileSet set{};
};

// Build profiles are project data, not command lines. The format is bounded,
// versioned and intentionally stores relative paths so it can be moved with a
// project without silently escaping its root.
class EditorBuildProfileStore final {
public:
    static constexpr std::uint32_t CurrentVersion = 1;
    static constexpr std::uint32_t CurrentSetVersion = 2;

    static bool serialize(const EditorBuildProfile& profile, std::string& json,
                          std::string* error = nullptr);
    static EditorBuildProfileLoadResult deserialize(std::string_view json);
    static bool serialize_set(const EditorBuildProfileSet& set, std::string& json,
                              std::string* error = nullptr);
    static EditorBuildProfileSetLoadResult deserialize_set(std::string_view json);
};

enum class EditorIdeKind : std::uint8_t {
    VisualStudio,
    Rider,
    VisualStudioCode,
    Clangd,
    Unknown,
};

struct EditorIdeDescriptor {
    EditorIdeKind kind{EditorIdeKind::Unknown};
    std::string name;
    std::filesystem::path executable;
    bool available{false};
};

struct EditorIdeLaunchPlan {
    bool valid{false};
    std::string error;
    EditorIdeKind kind{EditorIdeKind::Unknown};
    std::filesystem::path executable;
    std::filesystem::path workingDirectory;
    std::vector<std::string> arguments;
};

enum class EditorExternalLaunchState : std::uint8_t {
    NotStarted,
    Launched,
    InvalidPlan,
    LaunchFailed,
    Unsupported,
};

struct EditorExternalLaunchResult {
    EditorExternalLaunchState state{EditorExternalLaunchState::NotStarted};
    std::uint32_t processId{0};
    std::string error;
};

enum class EditorExternalProcessState : std::uint8_t {
    NotStarted,
    Running,
    Exited,
    NotFound,
    QueryFailed,
    Unsupported,
};

struct EditorExternalProcessStatus {
    EditorExternalProcessState state{EditorExternalProcessState::NotStarted};
    std::uint32_t processId{0};
    std::uint32_t exitCode{0};
    std::string error;
};

// Discovery and launch deliberately avoid shell parsing. A launch plan is
// inspectable and testable before the platform adapter creates a process.
class EditorToolIntegration final {
public:
    static std::vector<EditorIdeDescriptor> discover_ides(
        const std::filesystem::path& projectRoot, std::string_view searchPath = {});

    static EditorIdeLaunchPlan plan_ide_launch(
        const std::filesystem::path& projectRoot, const EditorBuildProfile& profile,
        EditorIdeKind kind, std::string_view selectedFile = {}, std::size_t line = 0,
        std::size_t column = 0, std::string_view searchPath = {});

    static EditorExternalLaunchResult launch(const EditorIdeLaunchPlan& plan);
    static EditorExternalProcessStatus query_process(std::uint32_t processId);
    static std::string_view ide_name(EditorIdeKind kind) noexcept;
};

} // namespace shinkou::editor

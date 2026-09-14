#pragma once

#include "shinkou/editor/EditorBuildSystem.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace shinkou::editor {

enum class EditorProjectFileKind : std::uint8_t {
    CMake,
    VisualStudioSolution,
    VisualStudioProject,
    DotNetProject,
    Meson,
    Cargo,
    CompileCommands,
    ClangdConfig,
    Unknown,
};

struct EditorProjectFileDescriptor {
    EditorProjectFileKind kind{EditorProjectFileKind::Unknown};
    std::filesystem::path relativePath;
    bool recommended{false};
};

struct EditorProjectDiscoveryResult {
    bool valid{false};
    std::string error;
    std::vector<EditorProjectFileDescriptor> files;
    std::filesystem::path recommendedProjectFile;
    std::filesystem::path compileCommandsFile;
};

struct EditorClangdConfigPlan {
    bool valid{false};
    std::string error;
    std::filesystem::path outputPath{".clangd"};
    std::filesystem::path compileCommandsFile;
    std::string content;
};

// Project discovery is deliberately data-only. It never launches a build
// tool, mutates a profile, or writes a config file; callers must make those
// actions explicit through the editor command path.
class EditorProjectIntegration final {
public:
    static constexpr std::size_t MaxFiles = 256;
    static constexpr std::size_t MaxScanEntries = 4096;
    static constexpr std::size_t MaxScanDepth = 8;

    static EditorProjectDiscoveryResult discover(
        const std::filesystem::path& projectRoot,
        const EditorBuildProfile* profile = nullptr);

    // The plan validates the selected compile database with the existing
    // bounded compile_commands parser. It produces content only; no file IO
    // occurs until write_clangd_config is called by an explicit command.
    static EditorClangdConfigPlan plan_clangd_config(
        const std::filesystem::path& projectRoot,
        const EditorBuildProfile* profile = nullptr);

    static bool write_clangd_config(
        const std::filesystem::path& projectRoot,
        const EditorClangdConfigPlan& plan,
        std::string* error = nullptr);

    static std::string_view file_kind_name(EditorProjectFileKind kind) noexcept;
};

} // namespace shinkou::editor

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::editor {

// A normalized compile command is data only. It is never executed by the
// editor; the build runner remains the only code allowed to launch tools.
struct EditorCompileCommand {
    std::filesystem::path directory;
    std::filesystem::path file;
    std::vector<std::string> arguments;
};

struct EditorCompileCommandsResult {
    bool valid{false};
    std::string error;
    std::vector<EditorCompileCommand> commands;
};

class EditorCompileCommands final {
public:
    // Parse the clangd/compile_commands.json format. Relative paths are
    // resolved against each command's directory and must stay under the
    // optional project root.
    static EditorCompileCommandsResult parse(std::string_view json,
                                             const std::filesystem::path& projectRoot = {});

    // Serialize normalized commands in the portable arguments-array form.
    // The result is suitable for clangd and other language tooling.
    static bool serialize(const std::vector<EditorCompileCommand>& commands,
                          const std::filesystem::path& projectRoot,
                          std::string& json, std::string* error = nullptr);
};

} // namespace shinkou::editor

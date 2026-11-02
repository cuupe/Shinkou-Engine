#include "shinkou/editor/EditorLayer.h"
#include <chrono>
#include <filesystem>
#include <iostream>

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("shinkou-no-imgui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        shinkou::editor::EditorLayer editor;
        editor.set_project_root(root.generic_string());
        editor.set_layout_path((root / "Saved/layout.json").generic_string());
        if (editor.initialize(true)) {
            std::cerr << "Editor presentation silently initialized without ImGui\n";
            return 1;
        }
        if (!editor.ui_interaction_regions().empty() || editor.ui_command_count() != 0) return 2;
    }
    std::filesystem::remove_all(root);
    std::cout << "Missing ImGui explicitly rejects editor presentation\n";
}

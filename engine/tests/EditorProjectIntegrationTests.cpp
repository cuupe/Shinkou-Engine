#include "shinkou/editor/EditorProjectIntegration.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
    using namespace shinkou::editor;
    const auto root = std::filesystem::temp_directory_path() /
        ("shinkou-project-integration-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    assert(std::filesystem::create_directories(root / "Game"));
    assert(std::filesystem::create_directories(root / "Tools"));
    assert(std::filesystem::create_directories(root / "out" / "build" / "editor"));
    assert(std::filesystem::create_directories(root / ".git"));
    assert(std::filesystem::create_directories(root / ".shinkou"));

    std::ofstream(root / "CMakeLists.txt") << "cmake_minimum_required(VERSION 3.20)\n";
    std::ofstream(root / "Engine.sln") << "Microsoft Visual Studio Solution File\n";
    std::ofstream(root / "Game" / "Game.vcxproj") << "<Project/>\n";
    std::ofstream(root / "Tools" / "Tool.csproj") << "<Project/>\n";
    std::ofstream(root / ".git" / "Hidden.sln") << "hidden\n";
    std::ofstream(root / ".shinkou" / "Hidden.csproj") << "hidden\n";
    std::ofstream(root / "out" / "build" / "editor" / "compile_commands.json") << R"json([
      {"directory":"out/build/editor","file":"../../../Game/main.cpp","arguments":["clang++","-c","../../../Game/main.cpp"]}
    ])json";

    EditorBuildProfile profile;
    profile.buildDirectory = "out/build/editor";
    const auto discovery = EditorProjectIntegration::discover(root, &profile);
    assert(discovery.valid);
    assert(discovery.recommendedProjectFile == std::filesystem::path("Engine.sln"));
    assert(discovery.compileCommandsFile == std::filesystem::path("out/build/editor/compile_commands.json"));
    assert(!discovery.files.empty());
    assert(std::find_if(discovery.files.begin(), discovery.files.end(), [](const auto& file) {
        return file.relativePath == std::filesystem::path(".git/Hidden.sln");
    }) == discovery.files.end());
    assert(std::find_if(discovery.files.begin(), discovery.files.end(), [](const auto& file) {
        return file.relativePath == std::filesystem::path(".shinkou/Hidden.csproj");
    }) == discovery.files.end());

    const auto plan = EditorProjectIntegration::plan_clangd_config(root, &profile);
    assert(plan.valid);
    assert(plan.compileCommandsFile == discovery.compileCommandsFile);
    assert(plan.content.find("CompilationDatabase: 'out/build/editor'") != std::string::npos);
    std::string error;
    assert(EditorProjectIntegration::write_clangd_config(root, plan, &error));
    std::ifstream config(root / ".clangd");
    const std::string configText((std::istreambuf_iterator<char>(config)), std::istreambuf_iterator<char>());
    assert(configText == plan.content);

    std::ofstream(root / "out" / "build" / "editor" / "compile_commands.json", std::ios::trunc) << "{ malformed";
    const auto malformed = EditorProjectIntegration::plan_clangd_config(root, &profile);
    assert(!malformed.valid && malformed.error.find("invalid") != std::string::npos);

    profile.projectFile = "Game/Game.vcxproj";
    const auto associated = EditorProjectIntegration::discover(root, &profile);
    assert(associated.recommendedProjectFile == std::filesystem::path("Game/Game.vcxproj"));
    EditorBuildProfile unsafeProfile;
    unsafeProfile.buildDirectory = "../outside";
    const auto unsafe = EditorProjectIntegration::plan_clangd_config(root, &unsafeProfile);
    assert(!unsafe.valid);
    assert(unsafe.error.find("outside") != std::string::npos);

    assert(EditorProjectIntegration::file_kind_name(EditorProjectFileKind::VisualStudioSolution) ==
           std::string_view("Visual Studio solution"));
    std::filesystem::remove_all(root, cleanup);
    std::cout << "Project discovery, profile association and clangd config safety passed\n";
    return 0;
}

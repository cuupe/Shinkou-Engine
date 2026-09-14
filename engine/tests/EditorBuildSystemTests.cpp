#include "shinkou/editor/EditorBuildSystem.h"
#include "shinkou/editor/EditorToolIntegration.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    using namespace shinkou::editor;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("shinkou-build-system-test-" + std::to_string(suffix));
    const auto tools = root / "tools";
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    assert(std::filesystem::create_directories(tools));
    for (const auto& name : {"cmake.exe", "ninja.exe", "MSBuild.exe", "clang-cl.exe", "dotnet.exe",
                             "devenv.exe", "rider64.exe", "code.exe", "clangd.exe"}) {
        std::ofstream(tools / name) << "test tool";
    }
    assert(std::filesystem::create_directories(root / "assets"));
    std::ofstream(root / "assets" / "main.cpp") << "int main() {}\n";

    EditorBuildSystem system(root);
    system.refresh_toolchains(tools.generic_string());
    assert(system.toolchains().size() == 8);
    assert(system.toolchains()[0].available);
    assert(system.toolchains()[1].available);
    assert(system.toolchains()[2].available);
    assert(system.toolchains()[4].available);
    assert(system.toolchains()[7].available);
    assert(!system.toolchains()[3].available);

    EditorBuildProfile cmake;
    cmake.id = "debug";
    cmake.name = "CMake Debug";
    cmake.generator = "Ninja";
    cmake.target = "shinkou_engine_sample";
    cmake.configureArguments = {"-DSHINKOU_BUILD_TESTS=ON"};
    cmake.buildArguments = {"--parallel", "2"};
    const auto cmakePlan = system.plan(cmake);
    assert(cmakePlan.valid && cmakePlan.commands.size() == 2);
    assert(cmakePlan.commands[0].arguments[0] == "-S");
    assert(cmakePlan.commands[0].arguments[1] == ".");
    assert(cmakePlan.commands[0].arguments[5] == "Ninja");
    assert(cmakePlan.commands[1].arguments[0] == "--build");
    assert(cmakePlan.commands[1].arguments.back() == "2");

    auto unsafe = cmake;
    unsafe.buildDirectory = "../outside";
    assert(!system.plan(unsafe).valid);

    auto msbuild = cmake;
    msbuild.buildTool = EditorToolKind::MSBuild;
    msbuild.projectFile = "Engine.sln";
    const auto msbuildPlan = system.plan(msbuild);
    assert(msbuildPlan.valid && msbuildPlan.commands[1].arguments[0] == "Engine.sln");
    assert(msbuildPlan.commands[1].arguments[1] == "/t:Build");

    const auto diagnostics = EditorBuildSystem::parse_diagnostics(
        "assets/main.cpp:12:7: error: expected ';'\n"
        "assets/main.cpp:19: warning: unused variable\n"
        "C:\\project\\assets\\mesh.cpp(4,2): error C2143: syntax error\n",
        root);
    assert(diagnostics.size() == 3);
    assert(diagnostics[0].severity == EditorDiagnosticSeverity::Error);
    assert(diagnostics[0].file.generic_string() == "assets/main.cpp");
    assert(diagnostics[0].line == 12 && diagnostics[0].column == 7);
    assert(diagnostics[1].severity == EditorDiagnosticSeverity::Warning);
    assert(diagnostics[1].line == 19 && diagnostics[1].column == 0);
    assert(diagnostics[2].code == "C2143");
    assert(diagnostics[2].line == 4 && diagnostics[2].column == 2);

    std::string profileJson;
    std::string profileError;
    assert(EditorBuildProfileStore::serialize(cmake, profileJson, &profileError));
    const auto loadedProfile = EditorBuildProfileStore::deserialize(profileJson);
    assert(loadedProfile.valid && loadedProfile.profile.id == cmake.id);
    assert(loadedProfile.profile.configureArguments == cmake.configureArguments);
    assert(loadedProfile.profile.buildArguments == cmake.buildArguments);
    auto unsafeProfile = cmake;
    unsafeProfile.buildDirectory = "../outside";
    assert(!EditorBuildProfileStore::serialize(unsafeProfile, profileJson, &profileError));
    assert(!EditorBuildProfileStore::deserialize("{\"version\":1,\"version\":1}" ).valid);
    auto release = cmake;
    release.id = "release";
    release.name = "CMake Release";
    release.configuration = "Release";
    EditorBuildProfileSet profileSet;
    profileSet.profiles = {cmake, release};
    profileSet.selectedId = release.id;
    assert(EditorBuildProfileStore::serialize_set(profileSet, profileJson, &profileError));
    const auto loadedSet = EditorBuildProfileStore::deserialize_set(profileJson);
    assert(loadedSet.valid && loadedSet.set.selectedId == "release" && loadedSet.set.profiles.size() == 2);
    auto invalidSet = profileJson;
    const auto selectedOffset = invalidSet.find("\"selectedId\": \"release\"");
    assert(selectedOffset != std::string::npos);
    invalidSet.replace(selectedOffset, std::string("\"selectedId\": \"release\"").size(), "\"selectedId\": \"missing\"");
    assert(!EditorBuildProfileStore::deserialize_set(invalidSet).valid);
    auto duplicateSet = profileSet;
    duplicateSet.profiles.push_back(cmake);
    assert(!EditorBuildProfileStore::serialize_set(duplicateSet, profileJson, &profileError));

    const auto ides = EditorToolIntegration::discover_ides(root, tools.generic_string());
    assert(ides.size() == 4);
    assert(std::all_of(ides.begin(), ides.end(), [](const auto& ide) { return ide.available; }));
    const auto vscodePlan = EditorToolIntegration::plan_ide_launch(
        root, cmake, EditorIdeKind::VisualStudioCode, "assets/main.cpp", 12, 7, tools.generic_string());
    assert(vscodePlan.valid && vscodePlan.arguments.size() == 4);
    assert(vscodePlan.arguments[1] == "--goto");
    assert(vscodePlan.arguments[2].find("assets/main.cpp:12:7") != std::string::npos);
    const auto riderPlan = EditorToolIntegration::plan_ide_launch(
        root, cmake, EditorIdeKind::Rider, "assets/main.cpp", 12, 7, tools.generic_string());
    assert(riderPlan.valid && std::find(riderPlan.arguments.begin(), riderPlan.arguments.end(), "--line") != riderPlan.arguments.end());
    const auto visualStudioPlan = EditorToolIntegration::plan_ide_launch(
        root, cmake, EditorIdeKind::VisualStudio, "assets/main.cpp", 12, 7, tools.generic_string());
    assert(visualStudioPlan.valid && std::find(visualStudioPlan.arguments.begin(), visualStudioPlan.arguments.end(), "/Command") != visualStudioPlan.arguments.end());
    const auto outsideIdePlan = EditorToolIntegration::plan_ide_launch(
        root, cmake, EditorIdeKind::VisualStudioCode, "../outside.cpp", 1, 1, tools.generic_string());
    assert(!outsideIdePlan.valid);
    const auto invalidLaunch = EditorToolIntegration::launch({});
    assert(invalidLaunch.state == EditorExternalLaunchState::InvalidPlan);
    assert(EditorToolIntegration::query_process(0).state == EditorExternalProcessState::NotStarted);
#if defined(_WIN32)
    if (const char* commandShell = std::getenv("ComSpec")) {
        EditorIdeLaunchPlan directLaunch;
        directLaunch.valid = true;
        directLaunch.executable = commandShell;
        directLaunch.workingDirectory = root;
        directLaunch.arguments = {"/C", "exit 0"};
        const auto launchResult = EditorToolIntegration::launch(directLaunch);
        assert(launchResult.state == EditorExternalLaunchState::Launched && launchResult.processId != 0);
        const auto processStatus = EditorToolIntegration::query_process(launchResult.processId);
        assert(processStatus.state == EditorExternalProcessState::Running ||
               processStatus.state == EditorExternalProcessState::Exited);
    }
#endif

    const auto malformed = EditorBuildSystem::parse_diagnostics(
        "assets/main.cpp:999999999999999999999999: error: malformed number\n", root);
    assert(malformed.empty());

#if defined(_WIN32)
    const char* commandShell = std::getenv("ComSpec");
    EditorBuildCommand processCommand;
    processCommand.executable = commandShell ? std::filesystem::path(commandShell) :
        std::filesystem::path("C:/Windows/System32/cmd.exe");
    processCommand.workingDirectory = root;
    processCommand.arguments = {"/C", "echo %SHINKOU_BUILD_TEST_VALUE%"};
    EditorBuildProcessOptions processOptions;
    processOptions.diagnosticRoot = root;
    processOptions.inheritEnvironment = false;
    processOptions.environmentAllowList = {"SHINKOU_BUILD_TEST_VALUE"};
    processOptions.environmentOverrides = {{"SHINKOU_BUILD_TEST_VALUE", "safe-output"}};
    std::vector<std::pair<EditorBuildOutputStream, std::string>> liveChunks;
    processOptions.outputCallback = [&liveChunks](EditorBuildOutputStream stream, std::string_view chunk) {
        liveChunks.emplace_back(stream, std::string(chunk));
    };
    std::atomic_bool cancelRequested{false};
    processCommand.arguments = {"/C", "echo %SHINKOU_BUILD_TEST_VALUE% & echo callback-out & echo callback-err 1>&2"};
    const auto processResult = EditorBuildProcess::run(processCommand, processOptions, cancelRequested);
    assert(processResult.state == EditorBuildProcessState::Succeeded);
    assert(processResult.standardOutput.find("safe-output") != std::string::npos);
    assert(processResult.standardOutput.find("callback-out") != std::string::npos);
    assert(processResult.standardError.find("callback-err") != std::string::npos);
    assert(std::any_of(liveChunks.begin(), liveChunks.end(), [](const auto& chunk) {
        return chunk.first == EditorBuildOutputStream::StandardOutput && chunk.second.find("callback-out") != std::string::npos;
    }));
    assert(std::any_of(liveChunks.begin(), liveChunks.end(), [](const auto& chunk) {
        return chunk.first == EditorBuildOutputStream::StandardError && chunk.second.find("callback-err") != std::string::npos;
    }));

    processOptions.outputCallback = {};
    processCommand.arguments = {"/C", "exit 7"};
    const auto failedProcess = EditorBuildProcess::run(processCommand, processOptions, cancelRequested);
    assert(failedProcess.state == EditorBuildProcessState::Failed);
    assert(failedProcess.exitCode == 7);

    processCommand.executable = "C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe";
    processCommand.arguments = {"-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 2"};
    auto timeoutOptions = processOptions;
    timeoutOptions.inheritEnvironment = true;
    timeoutOptions.environmentAllowList.clear();
    timeoutOptions.timeout = std::chrono::milliseconds(100);
    const auto timedOutProcess = EditorBuildProcess::run(processCommand, timeoutOptions, cancelRequested);
    assert(timedOutProcess.state == EditorBuildProcessState::TimedOut);

    cancelRequested.store(true);
    const auto cancelledProcess = EditorBuildProcess::run(processCommand, processOptions, cancelRequested);
    assert(cancelledProcess.state == EditorBuildProcessState::Cancelled);

    EditorBuildSystem actualTools(std::filesystem::current_path());
    actualTools.refresh_toolchains();
    const auto cmakeTool = std::find_if(actualTools.toolchains().begin(), actualTools.toolchains().end(),
        [](const auto& tool) { return tool.kind == EditorToolKind::CMake && tool.available; });
    if (cmakeTool != actualTools.toolchains().end()) {
        EditorBuildCommand versionCommand;
        versionCommand.executable = cmakeTool->executable;
        versionCommand.workingDirectory = std::filesystem::current_path();
        versionCommand.arguments = {"--version"};
        cancelRequested.store(false);
        const auto versionResult = EditorBuildProcess::run(versionCommand, {}, cancelRequested);
        assert(versionResult.state == EditorBuildProcessState::Succeeded);
        assert(versionResult.standardOutput.find("cmake") != std::string::npos);
    }
#endif

    std::filesystem::remove_all(root, cleanupError);
    std::cout << "Editor toolchain discovery, build planning, path safety, and diagnostics passed\n";
    return 0;
}

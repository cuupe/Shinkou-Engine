#include "shinkou/editor/EditorCompileCommands.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>

int main() {
    using namespace shinkou::editor;
    const auto root = std::filesystem::temp_directory_path() /
        ("shinkou-compile-commands-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    assert(std::filesystem::create_directories(root / "src"));

    const auto input = R"json([
      {"directory":".","file":"src/main.cpp","arguments":["clang++","-Iinclude","-c","src/main.cpp"]},
      {"directory":".","file":"src/util.cpp","command":"clang++ -I\"include dir\" -c src/util.cpp"}
    ])json";
    const auto parsed = EditorCompileCommands::parse(input, root);
    assert(parsed.valid && parsed.commands.size() == 2);
    assert(parsed.commands[0].directory == std::filesystem::weakly_canonical(root));
    assert(parsed.commands[0].file == std::filesystem::weakly_canonical(root / "src/main.cpp"));
    assert(parsed.commands[1].arguments.size() == 4);
    assert(parsed.commands[1].arguments[1] == "-Iinclude dir");

    std::string json;
    std::string error;
    assert(EditorCompileCommands::serialize(parsed.commands, root, json, &error));
    const auto roundTrip = EditorCompileCommands::parse(json, root);
    assert(roundTrip.valid && roundTrip.commands.size() == 2);
    assert(roundTrip.commands[0].arguments == parsed.commands[0].arguments);

    const auto outside = EditorCompileCommands::parse(
        R"json([{"directory":".","file":"../outside.cpp","arguments":["clang++"]}])json", root);
    assert(!outside.valid && outside.error.find("outside") != std::string::npos);

    EditorCompileCommand bad = parsed.commands.front();
    bad.file = root.parent_path() / "outside.cpp";
    assert(!EditorCompileCommands::serialize({bad}, root, json, &error));
    assert(json.empty());

    std::filesystem::remove_all(root, cleanup);
    std::cout << "Compile commands parsing, normalization and export safety passed\n";
    return 0;
}

#include "shinkou/editor/FileSystem.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>

int main() {
    using namespace shinkou::editor;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("shinkou-filesystem-test-" + std::to_string(suffix));
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    assert(std::filesystem::create_directories(root));

    FileSystemService files(root);
    std::string error;
    assert(files.ensure_directory("assets/materials", &error));
    assert(files.write_text_atomic("assets/materials/test.mat", "shader=flat\n", &error));
    bool directory = false;
    assert(files.exists("assets/materials", &directory) && directory);
    assert(files.exists("assets/materials/test.mat", &directory) && !directory);
    assert(files.project_relative_existing(root / "assets/materials/test.mat") ==
           std::filesystem::path("assets/materials/test.mat"));
    assert(files.project_relative_existing(root.parent_path() / "outside.mat").empty());
    assert(files.project_relative_existing(std::filesystem::path("assets/materials/test.mat")).empty());
    const auto resolved = files.resolve_existing("assets/materials/test.mat");
    assert(!resolved.empty() && resolved.filename() == "test.mat");
    assert(files.resolve_existing("../outside.mat").empty());

    const auto entries = files.list("assets", false, 16);
    assert(entries.size() == 1 && entries.front().directory);
    const auto initialScan = files.scan("assets", true, 16);
    assert(initialScan.changes.size() == initialScan.entries.size());
    assert(files.scan("assets", true, 16).changes.empty());
    assert(files.write_text_atomic("assets/materials/new.mat", "shader=unlit\n", &error));
    const auto changedScan = files.scan("assets", true, 16);
    assert(std::any_of(changedScan.changes.begin(), changedScan.changes.end(), [](const FileChange& change) {
        return change.type == FileChangeType::Added && change.relativePath == "assets/materials/new.mat" &&
            change.previousSize == 0 && change.currentSize == std::string("shader=unlit\n").size() &&
            change.previousWriteStamp == 0 && change.currentWriteStamp != 0;
    }));
    assert(files.rename("assets/materials/test.mat", "assets/materials/renamed.mat", &error));
    const auto renamedScan = files.scan("assets", true, 16);
    assert(std::any_of(renamedScan.changes.begin(), renamedScan.changes.end(), [](const FileChange& change) {
        return change.type == FileChangeType::Added && change.relativePath == "assets/materials/renamed.mat" &&
            change.currentSize == std::string("shader=flat\n").size();
    }));
    assert(std::any_of(renamedScan.changes.begin(), renamedScan.changes.end(), [](const FileChange& change) {
        return change.type == FileChangeType::Removed && change.relativePath == "assets/materials/test.mat" &&
            change.previousSize == std::string("shader=flat\n").size() && change.currentSize == 0;
    }));
    std::string content;
    assert(files.read_text("assets/materials/renamed.mat", content, &error));
    assert(content == "shader=flat\n");
    bool truncated = false;
    assert(files.read_text_limited("assets/materials/renamed.mat", 64, content, &truncated, &error));
    assert(!truncated && content == "shader=flat\n");
    assert(files.write_text_atomic("assets/materials/renamed.mat", "replacement", &error));
    assert(files.read_text_limited("assets/materials/renamed.mat", 4, content, &truncated, &error));
    assert(truncated && content == "repl");
    assert(files.read_text("assets/materials/renamed.mat", content, &error) && content == "replacement");
    const auto modifiedScan = files.scan("assets", true, 16);
    assert(std::any_of(modifiedScan.changes.begin(), modifiedScan.changes.end(), [](const FileChange& change) {
        return change.type == FileChangeType::Modified && change.relativePath == "assets/materials/renamed.mat" &&
            change.previousSize == std::string("shader=flat\n").size() &&
            change.currentSize == std::string("replacement").size() &&
            change.previousWriteStamp != 0 && change.currentWriteStamp != 0;
    }));
    assert(!files.write_text_atomic("assets/materials", "must not replace a directory", &error));
    assert(files.exists("assets/materials/renamed.mat"));
    assert(!files.rename("assets/materials/renamed.mat", "../outside.mat", &error));
    assert(files.remove("assets/materials/renamed.mat", &error));
    const auto removedScan = files.scan("assets", true, 16);
    assert(std::any_of(removedScan.changes.begin(), removedScan.changes.end(), [](const FileChange& change) {
        return change.type == FileChangeType::Removed && change.relativePath == "assets/materials/renamed.mat" &&
            change.previousSize == std::string("replacement").size() && change.currentSize == 0;
    }));
    assert(!files.exists("assets/materials/renamed.mat"));
    assert(files.remove("assets/materials", &error));
    assert(!files.exists("assets/materials"));

    std::filesystem::remove_all(root, cleanupError);
    std::cout << "File system root safety and resource operations passed\n";
    return 0;
}

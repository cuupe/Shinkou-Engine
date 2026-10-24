#include "shinkou/editor/FileSystem.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace {

void create_directory_link(const std::filesystem::path& target, const std::filesystem::path& link,
                           std::error_code& error) {
#if defined(_WIN32)
    // Junctions work without Developer Mode and exercise the MinGW case
    // where std::filesystem does not recognize native directory links.
    struct JunctionData {
        DWORD tag{IO_REPARSE_TAG_MOUNT_POINT};
        WORD length{0}, reserved{0};
        WORD substituteOffset{0}, substituteLength{0};
        WORD printOffset{0}, printLength{0};
        wchar_t paths[4096]{};
    } data;
    const auto printName = std::filesystem::absolute(target).wstring();
    const auto substitute = L"\\??\\" + printName;
    assert(substitute.size() + printName.size() + 2 < 4096);
    data.substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.printOffset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    data.printLength = static_cast<WORD>(printName.size() * sizeof(wchar_t));
    data.length = static_cast<WORD>(8 + data.printOffset + data.printLength + sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), data.paths);
    std::copy(printName.begin(), printName.end(), data.paths + substitute.size() + 1);
    if (!std::filesystem::create_directory(link, error)) return;
    const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = std::error_code(GetLastError(), std::system_category());
        RemoveDirectoryW(link.c_str());
        return;
    }
    DWORD returned = 0;
    const auto success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT,
        &data, data.length + 8, nullptr, 0, &returned, nullptr);
    const auto code = success ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    error = std::error_code(code, std::system_category());
    if (!success) RemoveDirectoryW(link.c_str());
#else
    std::filesystem::create_directory_symlink(target, link, error);
#endif
}

void test_scan_boundaries(const std::filesystem::path& root) {
    using namespace shinkou::editor;
    FileSystemService files(root);
    assert(files.write_text_atomic("a.txt", "a"));
    assert(files.write_text_atomic("b.txt", "b"));
    assert(files.write_text_atomic("nested/c.txt", "c"));
    const auto initial = files.scan(true, 16);
    assert(initial.complete && !initial.truncated && initial.error.empty());
    assert(initial.entries.size() == 4);
    assert(files.list_detailed({}, true, 4).complete);

    // Reducing a scan budget must not turn unvisited files into removals.
    const auto bounded = files.scan(true, 1);
    assert(bounded.entries.size() == 1);
    assert(!bounded.complete && bounded.truncated && bounded.error.empty());
    assert(bounded.changes.empty());
    assert(files.scan(true, 16).changes.empty());
    const auto zeroBudget = files.scan(true, 0);
    assert(!zeroBudget.complete && zeroBudget.truncated && zeroBudget.changes.empty());
    const auto outside = files.scan("../outside", true, 16);
    assert(!outside.complete && !outside.error.empty() && outside.changes.empty());
    const auto notDirectory = files.scan("a.txt", true, 16);
    assert(!notDirectory.complete && !notDirectory.error.empty() && notDirectory.changes.empty());
    assert(files.scan(true, 16).changes.empty());
    assert(files.write_text_atomic("a.txt", "modified during a bounded scan"));
    assert(files.scan(true, 1).changes.empty());
    const auto recovered = files.scan(true, 16);
    assert(std::any_of(recovered.changes.begin(), recovered.changes.end(), [](const auto& change) {
        return change.relativePath == "a.txt" && change.type == FileChangeType::Modified;
    }));

    // Recursive and direct-child scans describe different populations.
    const auto shallow = files.scan(false, 16);
    assert(shallow.entries.size() == 3);
    assert(std::none_of(shallow.changes.begin(), shallow.changes.end(), [](const auto& change) {
        return change.type == FileChangeType::Removed;
    }));
    assert(files.scan(false, 16).changes.empty());

    FileSystemService missing(root);
    assert(missing.scan("nested", true, 16).entries.size() == 1);
    assert(files.remove("nested"));
    const auto deletedScope = missing.scan("nested", true, 16);
    assert(deletedScope.complete && deletedScope.entries.empty());
    assert(deletedScope.changes.size() == 1);
    assert(deletedScope.changes.front().type == FileChangeType::Removed);
    assert(missing.scan("nested", true, 0).complete);
}

void test_io_boundaries(const std::filesystem::path& root) {
    using namespace shinkou::editor;
    FileSystemService files(root);
    std::string error = "old error";
    assert(files.write_text_atomic("data.txt", "content", &error) && error.empty());
    std::string content = "old content";
    bool truncated = true;
    assert(!files.read_text_limited("missing.txt", 16, content, &truncated, &error));
    assert(content.empty() && !truncated && !error.empty());
    assert(files.read_text_limited("data.txt", 0, content, &truncated, &error));
    assert(content.empty() && truncated && error.empty());
    assert(files.read_text("data.txt", content, &error) && content == "content" && error.empty());
    assert(!files.write_text_atomic({}, "must not replace root", &error));
    assert(!files.write_text_atomic("../outside.txt", "outside", &error));
    assert(!files.remove(".", &error));
    assert(!files.rename(".", "renamed-root", &error));
    assert(files.write_text_atomic("empty.txt", {}, &error));
    assert(files.read_text("empty.txt", content, &error) && content.empty());
    assert(!files.rename("data.txt", "empty.txt", &error));
    assert(files.read_text("data.txt", content, &error) && content == "content");
    const auto unicode = std::filesystem::u8path(u8"资源/中文.txt");
    assert(files.write_text_atomic(unicode, u8"文件内容", &error));
    assert(files.read_text(unicode, content, &error) && content == u8"文件内容");
    FileSystemService volume(root.root_path());
    assert(!volume.resolve_existing(root.lexically_relative(root.root_path()) / "data.txt").empty());
#if defined(_WIN32)
    assert(files.resolve_existing("C:data.txt").empty());
    assert(files.resolve_existing("\\Windows").empty());
#endif
}

void test_links(const std::filesystem::path& root) {
    using namespace shinkou::editor;
    FileSystemService files(root / "project");
    FileSystemService outside(root / "outside");
    assert(files.write_text_atomic("target/keep.txt", "keep"));
    assert(outside.write_text_atomic("keep.txt", "outside"));
    std::error_code linkError;
    create_directory_link(root / "outside", files.root() / "external", linkError);
    if (linkError) {
        std::cout << "SKIP directory link tests: " << linkError.message() << '\n';
        return;
    }
    create_directory_link(files.root() / "target", files.root() / "alias", linkError);
    assert(!linkError);
    // Some Windows filesystems reject creating a junction to an absent target.
    // Remove the target after creation to exercise the same dangling-link state.
    std::filesystem::create_directory(files.root() / "missing");
    create_directory_link(files.root() / "missing", files.root() / "dangling", linkError);
    assert(!linkError);
    assert(std::filesystem::remove(files.root() / "missing"));

    const auto entries = files.list_detailed({}, true, 16);
    assert(entries.complete);
    assert(std::any_of(entries.entries.begin(), entries.entries.end(), [](const auto& entry) {
        return entry.relativePath == "alias" && entry.name == "alias" && entry.directory;
    }));
    assert(std::none_of(entries.entries.begin(), entries.entries.end(), [](const auto& entry) {
        const auto path = entry.relativePath.generic_string();
        return path == "external" || path.rfind("external/", 0) == 0 ||
            path == "dangling" || path.rfind("alias/", 0) == 0 || path.rfind("../", 0) == 0;
    }));
    assert(files.resolve_existing("external/keep.txt").empty());
    assert(files.project_relative_existing(files.root() / "external/keep.txt").empty());
    FileSystemService linkedRoot(files.root() / "alias");
    assert(linkedRoot.exists("keep.txt"));
    assert(!files.write_text_atomic("external/keep.txt", "bad"));
    assert(!files.remove("external/keep.txt"));
    assert(!files.rename("target/keep.txt", "dangling"));
    assert(files.rename("alias", "renamed-alias"));
    assert(files.exists("target/keep.txt"));
    assert(files.remove("renamed-alias"));
    assert(files.exists("target/keep.txt"));
    assert(files.remove("dangling"));
    assert(files.remove("external"));
    assert(outside.exists("keep.txt"));
    assert(files.ensure_directory("container"));
    create_directory_link(root / "outside", files.root() / "container/external", linkError);
    assert(!linkError);
    assert(files.remove("container"));
    assert(outside.exists("keep.txt"));
    std::cout << "Directory link boundary and mutation tests passed\n";
}

} // namespace

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

    test_scan_boundaries(root / "scan-boundaries");
    test_io_boundaries(root / "io-boundaries");
    test_links(root / "links");

    std::filesystem::remove_all(root, cleanupError);
    std::cout << "File system root safety and resource operations passed\n";
    return 0;
}

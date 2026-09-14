#include "shinkou/editor/EditorModelPreview.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("shinkou-editor-model-preview-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code cleanup;
    std::filesystem::create_directories(root / "assets");
    std::ofstream(root / "assets/preview.obj") <<
        "# bounded OBJ fixture\n"
        "o Triangle\n"
        "v -1 -1 0\n"
        "v 1 -1 0\n"
        "v 0 1 0\n"
        "f 1/1/1 2/2/1 3/3/1\n";
    std::ofstream(root / "assets/broken.obj") << "v nan 0 0\n";

    shinkou::editor::FileSystemService files(root);
    const auto result = shinkou::editor::load_editor_model_preview(
        files, "assets/preview.obj", 7, 11);
    assert(result.generation == 7 && result.sourceStamp == 11);
    assert(result.snapshot && result.snapshot->valid());
    assert(result.snapshot->vertexCount == 3 && result.snapshot->triangleCount == 1);
    assert(result.snapshot->objectCount == 1 && result.snapshot->wireSegments->size() == 6);
    assert(result.snapshot->minX == -1.0f && result.snapshot->maxX == 1.0f);

    const std::string assetSource =
        "o AssetSystemTriangle\n"
        "v -2 -1 0\n"
        "v 2 -1 0\n"
        "v 0 2 0\n"
        "f 1 2 3\n";
    const std::vector<std::uint8_t> assetBytes(assetSource.begin(), assetSource.end());
    const auto assetResult = shinkou::editor::load_editor_obj_preview_bytes(
        "assets/preview.obj", assetBytes, 17, 19);
    assert(assetResult.snapshot && assetResult.snapshot->valid());
    assert(assetResult.snapshot->vertexCount == 3 && assetResult.snapshot->triangleCount == 1 &&
           assetResult.snapshot->minX == -2.0f && assetResult.snapshot->maxY == 2.0f);

    std::atomic_bool cancelled{true};
    const auto cancelledResult = shinkou::editor::load_editor_model_preview(
        files, "assets/preview.obj", 8, 12, &cancelled);
    assert(!cancelledResult.snapshot && cancelledResult.error.find("cancelled") != std::string::npos);

    const auto broken = shinkou::editor::load_editor_model_preview(
        files, "assets/broken.obj", 9, 13);
    assert(!broken.snapshot && broken.error.find("invalid vertex") != std::string::npos);
    const auto outside = shinkou::editor::load_editor_model_preview(
        files, "../outside.obj", 10, 14);
    assert(!outside.snapshot && !outside.error.empty());
    std::filesystem::remove_all(root, cleanup);
    std::cout << "Editor model preview provider passed\n";
    return 0;
}

#include "shinkou/editor/EditorImagePreview.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("shinkou-editor-image-preview-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code cleanup;
    std::filesystem::create_directories(root / "assets");
    constexpr unsigned char png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xb5, 0x1c, 0x0c, 0x02,
        0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54,
        0x78, 0x9c, 0x63, 0x64, 0x00, 0x02, 0x00, 0x00,
        0x05, 0x00, 0x01, 0xe9, 0x8d, 0x5d, 0x3c,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
        0xae, 0x42, 0x60, 0x82};
    {
        std::ofstream file(root / "assets/preview.png", std::ios::binary);
        file.write(reinterpret_cast<const char*>(png), static_cast<std::streamsize>(sizeof(png)));
    }

    shinkou::editor::FileSystemService files(root);
    const auto result = shinkou::editor::load_editor_image_preview(
        files, "assets/preview.png", 7, 11, 64);
    assert(result.generation == 7 && result.sourceStamp == 11);
    assert(result.path == "assets/preview.png");
    if (result.error.empty()) {
        assert(result.sourceWidth == 1 && result.sourceHeight == 1);
        assert(result.snapshot && result.snapshot->valid());
        assert(result.snapshot->width == 1 && result.snapshot->height == 1);
    } else {
        assert(result.snapshot == nullptr);
    }

    const std::vector<std::uint8_t> assetBytes(std::begin(png), std::end(png));
    const auto bytesResult = shinkou::editor::load_editor_image_preview_bytes(
        "assets/preview.png", assetBytes, 9, 13, 64);
    assert(bytesResult.generation == 9 && bytesResult.sourceStamp == 13 &&
           bytesResult.path == "assets/preview.png");
    if (bytesResult.error.empty()) {
        assert(bytesResult.sourceWidth == 1 && bytesResult.sourceHeight == 1 &&
               bytesResult.snapshot && bytesResult.snapshot->valid());
    } else {
        assert(bytesResult.snapshot == nullptr);
    }

    const auto outside = shinkou::editor::load_editor_image_preview(
        files, "../outside.png", 8, 12);
    assert(!outside.snapshot && !outside.error.empty());
    std::filesystem::remove_all(root, cleanup);
    std::cout << "Editor image preview provider passed\n";
    return 0;
}

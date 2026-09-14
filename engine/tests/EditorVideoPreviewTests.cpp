#include "shinkou/editor/EditorVideoPreview.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

void u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void fourcc(std::vector<std::uint8_t>& bytes, const char* value) {
    for (int index = 0; index < 4; ++index) bytes.push_back(static_cast<std::uint8_t>(value[index]));
}

void i32(std::vector<std::uint8_t>& bytes, std::int32_t value) {
    u32(bytes, static_cast<std::uint32_t>(value));
}

std::vector<std::uint8_t> chunk(const char* name, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> result;
    fourcc(result, name);
    u32(result, static_cast<std::uint32_t>(payload.size()));
    result.insert(result.end(), payload.begin(), payload.end());
    if ((payload.size() & 1u) != 0) result.push_back(0);
    return result;
}

std::vector<std::uint8_t> list(const char* name, const std::vector<std::uint8_t>& children) {
    std::vector<std::uint8_t> payload;
    fourcc(payload, name);
    payload.insert(payload.end(), children.begin(), children.end());
    return chunk("LIST", payload);
}

std::vector<std::uint8_t> make_uncompressed_avi() {
    std::vector<std::uint8_t> avih;
    u32(avih, 1000000); // one frame per second
    u32(avih, 16); u32(avih, 0); u32(avih, 0); u32(avih, 1);
    u32(avih, 0); u32(avih, 2); u32(avih, 16); u32(avih, 2); u32(avih, 2);
    for (int index = 0; index < 4; ++index) u32(avih, 0);

    std::vector<std::uint8_t> strh;
    fourcc(strh, "vids"); fourcc(strh, "DIB ");
    u32(strh, 0); u16(strh, 0); u16(strh, 0); u32(strh, 0); u32(strh, 1); u32(strh, 1);
    u32(strh, 0); u32(strh, 2); u32(strh, 16); u32(strh, 0xffffffffu); u32(strh, 0);
    u16(strh, 0); u16(strh, 0); u16(strh, 2); u16(strh, 2);

    std::vector<std::uint8_t> strf;
    u32(strf, 40); i32(strf, 2); i32(strf, -2); u16(strf, 1); u16(strf, 32);
    u32(strf, 0); u32(strf, 16); i32(strf, 0); i32(strf, 0); u32(strf, 0); u32(strf, 0);

    std::vector<std::uint8_t> strl;
    const auto strhChunk = chunk("strh", strh);
    const auto strfChunk = chunk("strf", strf);
    strl.insert(strl.end(), strhChunk.begin(), strhChunk.end());
    strl.insert(strl.end(), strfChunk.begin(), strfChunk.end());
    std::vector<std::uint8_t> hdrl;
    const auto avihChunk = chunk("avih", avih);
    hdrl.insert(hdrl.end(), avihChunk.begin(), avihChunk.end());
    const auto strlList = list("strl", strl);
    hdrl.insert(hdrl.end(), strlList.begin(), strlList.end());

    const std::vector<std::uint8_t> pixels{
        0, 0, 255, 0, 0, 255, 0, 0,
        255, 0, 0, 0, 255, 255, 255, 0};
    const auto frame = chunk("00db", pixels);
    const std::vector<std::uint8_t> secondPixels{
        255, 255, 255, 0, 255, 255, 255, 0,
        0, 255, 255, 0, 0, 255, 255, 0};
    const auto secondFrame = chunk("00db", secondPixels);
    std::vector<std::uint8_t> frames = frame;
    frames.insert(frames.end(), secondFrame.begin(), secondFrame.end());
    const auto movi = list("movi", frames);
    std::vector<std::uint8_t> payload;
    fourcc(payload, "AVI ");
    const auto hdrlList = list("hdrl", hdrl);
    payload.insert(payload.end(), hdrlList.begin(), hdrlList.end());
    payload.insert(payload.end(), movi.begin(), movi.end());
    std::vector<std::uint8_t> result;
    fourcc(result, "RIFF");
    u32(result, static_cast<std::uint32_t>(payload.size()));
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("shinkou-editor-video-preview-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code cleanup;
    std::filesystem::create_directories(root / "assets");
    {
        const auto bytes = make_uncompressed_avi();
        std::ofstream file(root / "assets/preview.avi", std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    std::ofstream(root / "assets/broken.avi", std::ios::binary) << "not a video";

    shinkou::editor::FileSystemService files(root);
    const auto result = shinkou::editor::load_editor_video_preview(
        files, "assets/preview.avi", 7, 11, 64);
    assert(result.generation == 7 && result.sourceStamp == 11);
    assert(result.path == "assets/preview.avi");
#if defined(_WIN32)
    assert(result.snapshot && result.snapshot->valid());
    assert(result.snapshot->width == 2 && result.snapshot->height == 2);
    assert(result.snapshot->firstFrame->width == 2 && result.snapshot->firstFrame->height == 2);
    assert(result.snapshot->duration >= 0.0 && result.snapshot->frameRate > 0.0);
#else
    assert(!result.snapshot && result.error.find("unavailable") != std::string::npos);
#endif

    const auto seekResult = shinkou::editor::load_editor_video_frame(
        files, "assets/preview.avi", 17, 21, 1.0, 64);
    assert(seekResult.generation == 17 && seekResult.sourceStamp == 21);
#if defined(_WIN32)
    assert(seekResult.snapshot && seekResult.snapshot->valid() && seekResult.snapshot->frameTime >= 0.0);
#else
    assert(!seekResult.snapshot && seekResult.error.find("unavailable") != std::string::npos);
#endif

    std::atomic_bool cancelled{true};
    const auto cancelledResult = shinkou::editor::load_editor_video_preview(
        files, "assets/preview.avi", 8, 12, 64, &cancelled);
    assert(!cancelledResult.snapshot && cancelledResult.error.find("cancelled") != std::string::npos);

    const auto broken = shinkou::editor::load_editor_video_preview(
        files, "assets/broken.avi", 9, 13);
    assert(!broken.snapshot && !broken.error.empty());
    const auto outside = shinkou::editor::load_editor_video_preview(
        files, "../outside.avi", 10, 14);
    assert(!outside.snapshot && !outside.error.empty());
    std::filesystem::remove_all(root, cleanup);
    std::cout << "Editor video preview provider passed\n";
    return 0;
}

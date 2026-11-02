#pragma once
#include "shinkou/render/RenderBackend.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace shinkou::render {
struct EditorGpuCaptureRequest {
    std::filesystem::path path;
    std::uintptr_t window{};
};
inline std::optional<EditorGpuCaptureRequest> editor_gpu_capture_request() {
    const auto *path = std::getenv("SHINKOU_UI_CAPTURE_PATH");
    if (!path || !*path)
        return {};
    EditorGpuCaptureRequest request{std::filesystem::u8path(path), 0};
    if (const auto *target = std::getenv("SHINKOU_UI_CAPTURE_TARGET_PATH")) {
        std::ifstream file(target);
        file >> request.window;
    }
    return request;
}
#if defined(SHINKOU_PLATFORM_WINDOWS)
template <class T> struct CaptureCom {
    T *value{};
    CaptureCom() = default;
    CaptureCom(const CaptureCom &) = delete;
    CaptureCom &operator=(const CaptureCom &) = delete;
    ~CaptureCom() {
        if (value)
            value->Release();
    }
    T **put() {
        return &value;
    }
    T *get() const {
        return value;
    }
};
inline bool write_editor_gpu_capture(const EditorGpuCaptureRequest &request,
                                     const TextureReadback &pixels, bool rgba) {
    if (!pixels.valid())
        return false;
    const auto temporary = request.path.wstring() + L".pending";
    BITMAPFILEHEADER header{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(pixels.width);
    info.biHeight = -static_cast<LONG>(pixels.height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info);
    const auto bytes = static_cast<std::size_t>(pixels.width) * 4;
    header.bfSize = header.bfOffBits + static_cast<DWORD>(bytes * pixels.height);
    std::ofstream file(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(&header), sizeof(header));
    file.write(reinterpret_cast<const char *>(&info), sizeof(info));
    std::vector<std::uint8_t> converted(rgba ? bytes : 0);
    for (std::uint32_t y = 0; y < pixels.height; ++y) {
        const auto *row = pixels.data.data() + y * pixels.rowPitch;
        if (rgba) {
            std::copy_n(row, bytes, converted.data());
            for (std::size_t x = 0; x < bytes; x += 4)
                std::swap(converted[x], converted[x + 2]);
            row = converted.data();
        }
        file.write(reinterpret_cast<const char *>(row), static_cast<std::streamsize>(bytes));
    }
    file.flush();
    const bool good = file.good();
    file.close();
    if (!good || !MoveFileExW(temporary.c_str(), request.path.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    std::ofstream kind(std::filesystem::path(request.path).concat(".kind"));
    kind << "EngineGpuReadback\n";
    return true;
}
#endif
} // namespace shinkou::render

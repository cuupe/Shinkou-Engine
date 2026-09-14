#include "shinkou/editor/EditorImagePreview.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <windows.h>
#include <wincodec.h>
#endif

namespace shinkou::editor {
namespace {

constexpr std::uintmax_t kMaxSourceBytes = 64u * 1024u * 1024u;
constexpr std::uint32_t kMaxDecodedDimension = 16384u;
constexpr std::size_t kMaxThumbnailPixels = 512u * 512u;

std::uint64_t image_revision(std::string_view path, std::uint64_t stamp,
                             std::uint32_t width, std::uint32_t height) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    };
    mix(path.data(), path.size());
    mix(&stamp, sizeof(stamp));
    mix(&width, sizeof(width));
    mix(&height, sizeof(height));
    return hash == 0 ? 1 : hash;
}

EditorImagePreviewResult failed(std::string path, std::uint64_t generation,
                                std::uint64_t sourceStamp, std::string error) {
    EditorImagePreviewResult result;
    result.generation = generation;
    result.sourceStamp = sourceStamp;
    result.path = std::move(path);
    result.error = std::move(error);
    return result;
}

#if defined(SHINKOU_PLATFORM_WINDOWS)
template<class T>
void release_com(T*& value) noexcept {
    if (value) value->Release();
    value = nullptr;
}

std::string win32_error(HRESULT value) {
    return "Windows Imaging Component error hr=" +
        std::to_string(static_cast<long long>(value));
}

EditorImagePreviewResult decode_wic_decoder(std::string path,
                                            std::uint64_t generation,
                                            std::uint64_t sourceStamp,
                                            std::uint32_t maxDimension,
                                            IWICImagingFactory* factory,
                                            IWICBitmapDecoder* decoder) {
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;
    auto finish = [&](EditorImagePreviewResult result) {
        release_com(converter);
        release_com(scaler);
        release_com(frame);
        return result;
    };

    HRESULT hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) return finish(failed(path, generation, sourceStamp, win32_error(hr)));

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    hr = frame->GetSize(&sourceWidth, &sourceHeight);
    if (FAILED(hr) || sourceWidth == 0 || sourceHeight == 0 ||
        sourceWidth > kMaxDecodedDimension || sourceHeight > kMaxDecodedDimension) {
        return finish(failed(path, generation, sourceStamp, "image dimensions exceed the preview limit"));
    }
    const auto sourcePixels = static_cast<std::uint64_t>(sourceWidth) * sourceHeight;
    if (sourcePixels > 256u * 1024u * 1024u)
        return finish(failed(path, generation, sourceStamp, "image decoded pixel budget exceeded"));

    const auto scale = std::min(1.0, static_cast<double>(maxDimension) /
        std::max(static_cast<double>(sourceWidth), static_cast<double>(sourceHeight)));
    const auto outputWidth = std::max<UINT>(1u, static_cast<UINT>(std::llround(sourceWidth * scale)));
    const auto outputHeight = std::max<UINT>(1u, static_cast<UINT>(std::llround(sourceHeight * scale)));
    IWICBitmapSource* bitmapSource = frame;
    if (outputWidth != sourceWidth || outputHeight != sourceHeight) {
        hr = factory->CreateBitmapScaler(&scaler);
        if (FAILED(hr) || !scaler) return finish(failed(path, generation, sourceStamp, win32_error(hr)));
        hr = scaler->Initialize(frame, outputWidth, outputHeight, WICBitmapInterpolationModeFant);
        if (FAILED(hr)) return finish(failed(path, generation, sourceStamp, win32_error(hr)));
        bitmapSource = scaler;
    }
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) return finish(failed(path, generation, sourceStamp, win32_error(hr)));
    hr = converter->Initialize(bitmapSource, GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return finish(failed(path, generation, sourceStamp, win32_error(hr)));
    const auto pixelCount = static_cast<std::size_t>(outputWidth) * outputHeight;
    if (pixelCount == 0 || pixelCount > kMaxThumbnailPixels)
        return finish(failed(path, generation, sourceStamp, "thumbnail pixel budget exceeded"));
    std::vector<std::uint8_t> pixels(pixelCount * 4u);
    hr = converter->CopyPixels(nullptr, outputWidth * 4u, static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr)) return finish(failed(path, generation, sourceStamp, win32_error(hr)));

    auto snapshot = std::make_shared<ui::UiImageSnapshot>();
    snapshot->revision = image_revision(path, sourceStamp, outputWidth, outputHeight);
    snapshot->width = outputWidth;
    snapshot->height = outputHeight;
    snapshot->bgraPremultiplied = std::move(pixels);
    EditorImagePreviewResult result;
    result.generation = generation;
    result.sourceStamp = sourceStamp;
    result.path = std::move(path);
    result.sourceWidth = sourceWidth;
    result.sourceHeight = sourceHeight;
    result.snapshot = std::move(snapshot);
    return finish(std::move(result));
}
#endif

} // namespace

EditorImagePreviewResult load_editor_image_preview(const FileSystemService& files,
                                                   std::string_view relativePath,
                                                   std::uint64_t generation,
                                                   std::uint64_t sourceStamp,
                                                   std::uint32_t maxDimension) {
    const std::string path(relativePath);
    const auto absolute = files.resolve_existing(path);
    if (absolute.empty()) return failed(path, generation, sourceStamp,
        "image path is outside the project or no longer exists");
    std::error_code sizeError;
    const auto sourceBytes = std::filesystem::file_size(absolute, sizeError);
    if (sizeError) return failed(path, generation, sourceStamp, "image file size is unavailable");
    if (sourceBytes > kMaxSourceBytes) return failed(path, generation, sourceStamp,
        "image file exceeds the 64 MiB preview limit");
    maxDimension = std::clamp(maxDimension, 1u, 512u);

#if !defined(SHINKOU_PLATFORM_WINDOWS)
    (void)maxDimension;
    return failed(path, generation, sourceStamp, "image preview provider is unavailable on this platform");
#else
    const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
        return failed(path, generation, sourceStamp, "COM initialization failed");

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        if (uninitialize) CoUninitialize();
        return failed(path, generation, sourceStamp, win32_error(hr));
    }
    const auto widePath = [&absolute] {
        const auto value = absolute.wstring();
        return value;
    }();
    hr = factory->CreateDecoderFromFilename(widePath.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || !decoder) {
        release_com(decoder);
        release_com(factory);
        if (uninitialize) CoUninitialize();
        return failed(path, generation, sourceStamp, win32_error(hr));
    }
    auto result = decode_wic_decoder(path, generation, sourceStamp, maxDimension, factory, decoder);
    release_com(decoder);
    release_com(factory);
    if (uninitialize) CoUninitialize();
    return result;
#endif
}

EditorImagePreviewResult load_editor_image_preview_bytes(
    std::string_view relativePath, const std::vector<std::uint8_t>& sourceBytes,
    std::uint64_t generation, std::uint64_t sourceStamp, std::uint32_t maxDimension) {
    const std::string path(relativePath);
    if (sourceBytes.size() > kMaxSourceBytes) return failed(path, generation, sourceStamp,
        "image bytes exceed the 64 MiB preview limit");
    maxDimension = std::clamp(maxDimension, 1u, 512u);

#if !defined(SHINKOU_PLATFORM_WINDOWS)
    (void)maxDimension;
    return failed(path, generation, sourceStamp, "image preview provider is unavailable on this platform");
#else
    const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
        return failed(path, generation, sourceStamp, "COM initialization failed");

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        if (uninitialize) CoUninitialize();
        return failed(path, generation, sourceStamp, win32_error(hr));
    }
    hr = factory->CreateStream(&stream);
    if (FAILED(hr) || !stream || sourceBytes.size() > std::numeric_limits<DWORD>::max()) {
        release_com(stream);
        release_com(factory);
        if (uninitialize) CoUninitialize();
        return failed(path, generation, sourceStamp, FAILED(hr) ? win32_error(hr) : "image byte stream is too large");
    }
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(reinterpret_cast<const BYTE*>(sourceBytes.data())),
                                      static_cast<DWORD>(sourceBytes.size()));
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (FAILED(hr) || !decoder) {
        release_com(decoder);
        release_com(stream);
        release_com(factory);
        if (uninitialize) CoUninitialize();
        return failed(path, generation, sourceStamp, win32_error(hr));
    }
    auto result = decode_wic_decoder(path, generation, sourceStamp, maxDimension, factory, decoder);
    release_com(decoder);
    release_com(stream);
    release_com(factory);
    if (uninitialize) CoUninitialize();
    return result;
#endif
}

} // namespace shinkou::editor

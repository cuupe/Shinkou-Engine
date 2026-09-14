#include "shinkou/editor/EditorVideoPreview.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <utility>
#include <vector>

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propidl.h>
#include <propvarutil.h>
#endif

namespace shinkou::editor {
namespace {

constexpr std::uintmax_t kMaxSourceBytes = 512u * 1024u * 1024u;
constexpr std::uint32_t kMaxDimension = 4096u;
constexpr std::size_t kMaxFrameBytes = 512u * 512u * 4u;

bool cancelled(const std::atomic_bool* value) noexcept {
    return value && value->load(std::memory_order_relaxed);
}

std::uint64_t video_revision(std::string_view path, std::uint64_t stamp,
                             std::uint32_t width, std::uint32_t height,
                             double duration) noexcept {
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
    mix(&duration, sizeof(duration));
    return hash == 0 ? 1 : hash;
}

EditorVideoPreviewResult failed(std::string path, std::uint64_t generation,
                                std::uint64_t sourceStamp, std::string error) {
    EditorVideoPreviewResult result;
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

std::string mf_error(HRESULT value) {
    return "Media Foundation error hr=" +
        std::to_string(static_cast<long long>(value));
}

bool get_u64(const PROPVARIANT& value, std::uint64_t& output) noexcept {
    if (value.vt == VT_UI8) {
        output = value.uhVal.QuadPart;
        return true;
    }
    if (value.vt == VT_I8 && value.hVal.QuadPart >= 0) {
        output = static_cast<std::uint64_t>(value.hVal.QuadPart);
        return true;
    }
    return false;
}

std::shared_ptr<ui::UiImageSnapshot> make_frame_snapshot(
    const BYTE* source, std::size_t sourceBytes, LONG stride,
    UINT width, UINT height, std::uint32_t maxDimension) {
    if (!source || width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension)
        return {};
    const auto scale = std::min(1.0, static_cast<double>(maxDimension) /
        std::max(static_cast<double>(width), static_cast<double>(height)));
    const auto outputWidth = std::max<UINT>(1u, static_cast<UINT>(std::llround(width * scale)));
    const auto outputHeight = std::max<UINT>(1u, static_cast<UINT>(std::llround(height * scale)));
    const auto sourceStride = static_cast<std::size_t>(std::abs(stride));
    const auto requiredBytes = sourceStride * static_cast<std::size_t>(height);
    if (sourceStride < static_cast<std::size_t>(width) * 4u || requiredBytes > sourceBytes ||
        static_cast<std::size_t>(outputWidth) * outputHeight * 4u > kMaxFrameBytes)
        return {};

    auto snapshot = std::make_shared<ui::UiImageSnapshot>();
    snapshot->width = outputWidth;
    snapshot->height = outputHeight;
    snapshot->bgraPremultiplied.resize(static_cast<std::size_t>(outputWidth) * outputHeight * 4u);
    for (UINT y = 0; y < outputHeight; ++y) {
        const auto sourceY = static_cast<UINT>((static_cast<std::uint64_t>(y) * height) / outputHeight);
        const auto rowIndex = stride >= 0 ? sourceY : height - 1u - sourceY;
        const auto* row = source + static_cast<std::size_t>(rowIndex) * sourceStride;
        for (UINT x = 0; x < outputWidth; ++x) {
            const auto sourceX = static_cast<UINT>((static_cast<std::uint64_t>(x) * width) / outputWidth);
            const auto* pixel = row + static_cast<std::size_t>(sourceX) * 4u;
            auto* output = snapshot->bgraPremultiplied.data() +
                (static_cast<std::size_t>(y) * outputWidth + x) * 4u;
            output[0] = pixel[0];
            output[1] = pixel[1];
            output[2] = pixel[2];
            output[3] = 255u;
        }
    }
    return snapshot;
}
#endif

} // namespace

EditorVideoPreviewResult load_editor_video_preview_at(
    const FileSystemService& files, std::string_view relativePath,
    std::uint64_t generation, std::uint64_t sourceStamp, double seconds,
    std::uint32_t maxDimension, const std::atomic_bool* cancel) {
    const std::string path(relativePath);
    const auto absolute = files.resolve_existing(path);
    if (absolute.empty()) return failed(path, generation, sourceStamp,
        "video path is outside the project or no longer exists");
    std::error_code sizeError;
    const auto sourceBytes = std::filesystem::file_size(absolute, sizeError);
    if (sizeError) return failed(path, generation, sourceStamp, "video file size is unavailable");
    if (sourceBytes > kMaxSourceBytes) return failed(path, generation, sourceStamp,
        "video file exceeds the 512 MiB preview limit");
    maxDimension = std::clamp(maxDimension, 1u, 512u);
    if (cancelled(cancel)) return failed(path, generation, sourceStamp, "video preview cancelled");

#if !defined(SHINKOU_PLATFORM_WINDOWS)
    (void)maxDimension;
    (void)seconds;
    (void)cancel;
    return failed(path, generation, sourceStamp,
        "video preview provider is unavailable on this platform");
#else
    const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
        return failed(path, generation, sourceStamp, "COM initialization failed");

    const auto finish = [&](EditorVideoPreviewResult result) {
        MFShutdown();
        if (uninitialize) CoUninitialize();
        return result;
    };
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) return finish(failed(path, generation, sourceStamp, mf_error(hr)));
    if (cancelled(cancel)) return finish(failed(path, generation, sourceStamp, "video preview cancelled"));

    IMFAttributes* attributes = nullptr;
    IMFSourceReader* reader = nullptr;
    IMFMediaType* nativeType = nullptr;
    IMFMediaType* outputType = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    IMFSample* sample = nullptr;
    auto release_all = [&](EditorVideoPreviewResult result) {
        release_com(buffer);
        release_com(sample);
        release_com(outputType);
        release_com(nativeType);
        release_com(reader);
        release_com(attributes);
        return finish(std::move(result));
    };

    hr = MFCreateAttributes(&attributes, 2);
    if (FAILED(hr)) return release_all(failed(path, generation, sourceStamp, mf_error(hr)));
    hr = attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
    if (FAILED(hr)) return release_all(failed(path, generation, sourceStamp, mf_error(hr)));
    const auto widePath = absolute.wstring();
    hr = MFCreateSourceReaderFromURL(widePath.c_str(), attributes, &reader);
    if (FAILED(hr) || !reader) return release_all(failed(path, generation, sourceStamp, mf_error(hr)));

    constexpr DWORD videoStream = MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    hr = reader->GetNativeMediaType(videoStream, 0, &nativeType);
    if (FAILED(hr) || !nativeType) return release_all(failed(path, generation, sourceStamp,
        "video stream is unavailable: " + mf_error(hr)));
    UINT32 width = 0;
    UINT32 height = 0;
    hr = MFGetAttributeSize(nativeType, MF_MT_FRAME_SIZE, &width, &height);
    if (FAILED(hr) || width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension)
        return release_all(failed(path, generation, sourceStamp, "video dimensions exceed the preview limit"));
    UINT32 rateNumerator = 0;
    UINT32 rateDenominator = 1;
    if (FAILED(MFGetAttributeRatio(nativeType, MF_MT_FRAME_RATE, &rateNumerator, &rateDenominator)) ||
        rateNumerator == 0 || rateDenominator == 0) {
        rateNumerator = 0;
        rateDenominator = 1;
    }

    PROPVARIANT durationValue;
    PropVariantInit(&durationValue);
    std::uint64_t durationTicks = 0;
    if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
                                                   MF_PD_DURATION, &durationValue)) &&
        get_u64(durationValue, durationTicks)) {
        // Media Foundation durations are expressed in 100-nanosecond units.
    }
    PropVariantClear(&durationValue);
    const double duration = static_cast<double>(durationTicks) / 10000000.0;
    const double targetSeconds = duration > 0.0
        ? std::clamp(std::isfinite(seconds) ? seconds : 0.0, 0.0, duration)
        : std::max(0.0, std::isfinite(seconds) ? seconds : 0.0);

    std::uint32_t audioStreamCount = 0;
    for (DWORD index = 0; index < 32; ++index) {
        IMFMediaType* audioType = nullptr;
        const auto audioResult = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                             index, &audioType);
        if (FAILED(audioResult) || !audioType) break;
        ++audioStreamCount;
        release_com(audioType);
    }

    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) return release_all(failed(path, generation, sourceStamp, mf_error(hr)));
    if (FAILED(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
        FAILED(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
        FAILED(MFSetAttributeSize(outputType, MF_MT_FRAME_SIZE, width, height)) ||
        (rateNumerator != 0 && FAILED(MFSetAttributeRatio(outputType, MF_MT_FRAME_RATE,
                                                           rateNumerator, rateDenominator))) ||
        FAILED(reader->SetCurrentMediaType(videoStream, nullptr, outputType))) {
        return release_all(failed(path, generation, sourceStamp,
            "video RGB32 conversion is unavailable"));
    }
    LONG stride = static_cast<LONG>(width * 4u);
    UINT32 configuredStride = 0;
    if (SUCCEEDED(outputType->GetUINT32(MF_MT_DEFAULT_STRIDE, &configuredStride)) && configuredStride != 0)
        stride = static_cast<LONG>(configuredStride);
    release_com(nativeType);
    release_com(outputType);

    DWORD actualStream = 0;
    DWORD flags = 0;
    LONGLONG timestamp = 0;
    if (targetSeconds > 0.0) {
        PROPVARIANT seekPosition;
        PropVariantInit(&seekPosition);
        seekPosition.vt = VT_I8;
        seekPosition.hVal.QuadPart = static_cast<LONGLONG>(std::llround(targetSeconds * 10000000.0));
        hr = reader->SetCurrentPosition(GUID_NULL, seekPosition);
        PropVariantClear(&seekPosition);
        if (FAILED(hr)) return release_all(failed(path, generation, sourceStamp,
            "video seek is unavailable: " + mf_error(hr)));
    }
    hr = reader->ReadSample(videoStream, 0, &actualStream, &flags, &timestamp, &sample);
    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 || !sample)
        return release_all(failed(path, generation, sourceStamp,
            "video first frame is unavailable: " + mf_error(hr)));
    if (cancelled(cancel)) return release_all(failed(path, generation, sourceStamp, "video preview cancelled"));
    hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr) || !buffer) return release_all(failed(path, generation, sourceStamp, mf_error(hr)));
    BYTE* data = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    hr = buffer->Lock(&data, &maxLength, &currentLength);
    if (FAILED(hr) || !data) return release_all(failed(path, generation, sourceStamp, mf_error(hr)));
    auto frame = make_frame_snapshot(data, currentLength, stride, width, height, maxDimension);
    buffer->Unlock();
    if (!frame) return release_all(failed(path, generation, sourceStamp,
        "video first frame exceeds the preview pixel budget"));
    frame->revision = video_revision(path, sourceStamp, frame->width, frame->height, duration);

    auto snapshot = std::make_shared<EditorVideoPreviewSnapshot>();
    snapshot->width = width;
    snapshot->height = height;
    snapshot->duration = std::isfinite(duration) ? std::max(0.0, duration) : 0.0;
    snapshot->frameRate = rateNumerator != 0 ?
        static_cast<double>(rateNumerator) / rateDenominator : 0.0;
    snapshot->frameTime = std::max(0.0, static_cast<double>(timestamp) / 10000000.0);
    snapshot->audioStreamCount = audioStreamCount;
    snapshot->firstFrame = std::move(frame);
    snapshot->revision = video_revision(path, sourceStamp, width, height, snapshot->duration);
    EditorVideoPreviewResult result;
    result.generation = generation;
    result.sourceStamp = sourceStamp;
    result.path = path;
    result.snapshot = std::move(snapshot);
    return release_all(std::move(result));
#endif
}

EditorVideoPreviewResult load_editor_video_preview(
    const FileSystemService& files, std::string_view relativePath,
    std::uint64_t generation, std::uint64_t sourceStamp,
    std::uint32_t maxDimension, const std::atomic_bool* cancel) {
    return load_editor_video_preview_at(files, relativePath, generation, sourceStamp,
                                         0.0, maxDimension, cancel);
}

EditorVideoPreviewResult load_editor_video_frame(
    const FileSystemService& files, std::string_view relativePath,
    std::uint64_t generation, std::uint64_t sourceStamp,
    double seconds, std::uint32_t maxDimension, const std::atomic_bool* cancel) {
    return load_editor_video_preview_at(files, relativePath, generation, sourceStamp,
                                         seconds, maxDimension, cancel);
}

} // namespace shinkou::editor

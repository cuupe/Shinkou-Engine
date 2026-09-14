#include "shinkou/editor/EditorAudioPreview.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <utility>

#if defined(SHINKOU_WITH_MINIAUDIO)
#include <miniaudio.h>
#endif

namespace shinkou::editor {
namespace {

constexpr std::uintmax_t kMaxSourceBytes = 256u * 1024u * 1024u;
constexpr std::uint32_t kMaxChannels = 32u;
constexpr std::uint32_t kMaxPeakCount = 512u;
constexpr std::uint64_t kMaxProbeFramesPerBin = 4096u;

std::uint64_t audio_revision(std::string_view path, std::uint64_t stamp,
                             std::uint32_t channels, std::uint32_t sampleRate,
                             std::uint64_t totalFrames) noexcept {
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
    mix(&channels, sizeof(channels));
    mix(&sampleRate, sizeof(sampleRate));
    mix(&totalFrames, sizeof(totalFrames));
    return hash == 0 ? 1 : hash;
}

EditorAudioPreviewResult failed(std::string path, std::uint64_t generation,
                                std::uint64_t sourceStamp, std::string error) {
    EditorAudioPreviewResult result;
    result.generation = generation;
    result.sourceStamp = sourceStamp;
    result.path = std::move(path);
    result.error = std::move(error);
    return result;
}

#if defined(SHINKOU_WITH_MINIAUDIO)
std::string decoder_error(ma_result result) {
    return std::string("audio decoder: ") + ma_result_description(result);
}
#endif

} // namespace

EditorAudioPreviewResult load_editor_audio_preview(const FileSystemService& files,
                                                   std::string_view relativePath,
                                                   std::uint64_t generation,
                                                   std::uint64_t sourceStamp,
                                                   std::uint32_t peakCount,
                                                   const std::atomic_bool* cancel) {
    const std::string path(relativePath);
    const auto absolute = files.resolve_existing(path);
    if (absolute.empty()) return failed(path, generation, sourceStamp,
        "audio path is outside the project or no longer exists");
    std::error_code sizeError;
    const auto sourceBytes = std::filesystem::file_size(absolute, sizeError);
    if (sizeError) return failed(path, generation, sourceStamp, "audio file size is unavailable");
    if (sourceBytes > kMaxSourceBytes) return failed(path, generation, sourceStamp,
        "audio file exceeds the 256 MiB preview limit");
    peakCount = std::clamp(peakCount, 1u, kMaxPeakCount);
    const auto cancelled = [cancel] { return cancel && cancel->load(std::memory_order_relaxed); };
    if (cancelled()) return failed(path, generation, sourceStamp, "audio preview cancelled");

#if !defined(SHINKOU_WITH_MINIAUDIO)
    return failed(path, generation, sourceStamp,
        "audio preview provider is unavailable without miniaudio");
#else
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder{};
    const auto initResult = ma_decoder_init_file(absolute.string().c_str(), &config, &decoder);
    if (initResult != MA_SUCCESS)
        return failed(path, generation, sourceStamp, decoder_error(initResult));

    ma_format format{};
    ma_uint32 channels = 0;
    ma_uint32 sampleRate = 0;
    ma_channel channelMap[MA_MAX_CHANNELS]{};
    const auto dataResult = ma_decoder_get_data_format(&decoder, &format, &channels,
        &sampleRate, channelMap, MA_MAX_CHANNELS);
    if (dataResult != MA_SUCCESS || channels == 0 || channels > kMaxChannels || sampleRate == 0) {
        ma_decoder_uninit(&decoder);
        return failed(path, generation, sourceStamp,
            dataResult == MA_SUCCESS ? "audio format exceeds the preview limits" : decoder_error(dataResult));
    }
    ma_uint64 totalFrames = 0;
    const auto lengthResult = ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
    if (lengthResult != MA_SUCCESS || totalFrames == 0) {
        ma_decoder_uninit(&decoder);
        return failed(path, generation, sourceStamp,
            lengthResult == MA_SUCCESS ? "audio stream has no frames" : decoder_error(lengthResult));
    }

    const auto duration = static_cast<double>(totalFrames) / static_cast<double>(sampleRate);
    std::vector<float> peaks(peakCount, 0.0f);
    const auto frameWindow = std::min<std::uint64_t>(kMaxProbeFramesPerBin,
        std::max<std::uint64_t>(1u, (totalFrames + peakCount - 1u) / peakCount));
    std::vector<float> samples(static_cast<std::size_t>(frameWindow) * channels);
    for (std::uint32_t bin = 0; bin < peakCount; ++bin) {
        if (cancelled()) {
            ma_decoder_uninit(&decoder);
            return failed(path, generation, sourceStamp, "audio preview cancelled");
        }
        const auto start = (totalFrames * bin) / peakCount;
        const auto remaining = totalFrames - start;
        const auto requested = std::min(frameWindow, remaining);
        if (ma_decoder_seek_to_pcm_frame(&decoder, start) != MA_SUCCESS) {
            ma_decoder_uninit(&decoder);
            return failed(path, generation, sourceStamp, "audio decoder seek failed while sampling waveform");
        }
        ma_uint64 framesRead = 0;
        if (ma_decoder_read_pcm_frames(&decoder, samples.data(), requested, &framesRead) != MA_SUCCESS) {
            ma_decoder_uninit(&decoder);
            return failed(path, generation, sourceStamp, "audio decoder read failed while sampling waveform");
        }
        if (cancelled()) {
            ma_decoder_uninit(&decoder);
            return failed(path, generation, sourceStamp, "audio preview cancelled");
        }
        float peak = 0.0f;
        const auto sampleCount = static_cast<std::size_t>(framesRead) * channels;
        for (std::size_t index = 0; index < sampleCount; ++index) {
            const float value = samples[index];
            if (std::isfinite(value)) peak = std::max(peak, std::abs(value));
        }
        peaks[bin] = std::clamp(peak, 0.0f, 1.0f);
    }
    ma_decoder_uninit(&decoder);

    auto snapshot = std::make_shared<EditorAudioPreviewSnapshot>();
    snapshot->revision = audio_revision(path, sourceStamp, channels, sampleRate, totalFrames);
    snapshot->channels = channels;
    snapshot->sampleRate = sampleRate;
    snapshot->totalFrames = totalFrames;
    snapshot->duration = duration;
    snapshot->peaks = std::move(peaks);
    EditorAudioPreviewResult result;
    result.generation = generation;
    result.sourceStamp = sourceStamp;
    result.path = path;
    result.snapshot = std::move(snapshot);
    return result;
#endif
}

} // namespace shinkou::editor

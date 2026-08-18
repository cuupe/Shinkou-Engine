#include "shinkou/audio/AudioCodec.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

#if defined(SHINKOU_WITH_MINIAUDIO)
#include <miniaudio.h>
#endif

namespace shinkou::audio {
namespace {
AudioCodecResult failure(std::string message) { return {false, std::move(message)}; }
AudioCodecResult success() { return {true, {}}; }

std::string quote_argument(const std::filesystem::path& path) {
    const auto value = path.string();
    std::string quoted = "\"";
    for (const auto character : value) {
        if (character == '"') quoted += '\\';
        quoted += character;
    }
    quoted += '"';
    return quoted;
}

#if defined(SHINKOU_WITH_MINIAUDIO)
ma_format to_ma_format(AudioSampleFormat format) {
    switch (format) {
    case AudioSampleFormat::Int16: return ma_format_s16;
    case AudioSampleFormat::Int24: return ma_format_s24;
    case AudioSampleFormat::Int32: return ma_format_s32;
    case AudioSampleFormat::Float32: return ma_format_f32;
    }
    return ma_format_f32;
}
#endif
}

AudioCodecResult AudioCodec::decode(const std::filesystem::path& path, AudioPcmBuffer& output,
                                    const AudioDecodeOptions& options) {
#if defined(SHINKOU_WITH_MINIAUDIO)
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32,
        options.preserveNativeChannels ? 0 : options.outputFormat.channels,
        options.preserveNativeChannels ? 0 : options.outputFormat.sampleRate);
    ma_decoder decoder{};
    const auto result = ma_decoder_init_file(path.string().c_str(), &config, &decoder);
    if (result != MA_SUCCESS) return failure("unable to decode audio: " + path.string());
    ma_format format{};
    ma_uint32 channels{};
    ma_uint32 sampleRate{};
    ma_channel channelMap[MA_MAX_CHANNELS]{};
    if (ma_decoder_get_data_format(&decoder, &format, &channels, &sampleRate, channelMap, MA_MAX_CHANNELS) != MA_SUCCESS) {
        ma_decoder_uninit(&decoder);
        return failure("unable to inspect decoded audio: " + path.string());
    }
    ma_uint64 length{};
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &length) != MA_SUCCESS) length = 0;
    output.format = options.outputFormat;
    output.format.sampleFormat = AudioSampleFormat::Float32;
    output.format.channels = channels;
    output.format.sampleRate = sampleRate;
    output.format.layout = channels == 1 ? AudioChannelLayout::Mono : channels == 2 ? AudioChannelLayout::Stereo : AudioChannelLayout::Custom;
    output.samples.resize(static_cast<std::size_t>(length) * channels);
    ma_uint64 framesRead{};
    if (length > 0 && ma_decoder_read_pcm_frames(&decoder, output.samples.data(), length, &framesRead) != MA_SUCCESS) {
        ma_decoder_uninit(&decoder);
        return failure("unable to read decoded audio: " + path.string());
    }
    output.samples.resize(static_cast<std::size_t>(framesRead) * channels);
    ma_decoder_uninit(&decoder);
    return success();
#else
    (void)path; (void)output; (void)options;
    return failure("audio decoding requires SHINKOU_WITH_MINIAUDIO");
#endif
}

AudioCodecResult AudioCodec::encode_wav(const std::filesystem::path& path, const AudioPcmBuffer& input,
                                        const AudioEncodeOptions& options) {
#if defined(SHINKOU_WITH_MINIAUDIO)
    if (!input.valid()) return failure("cannot encode invalid PCM buffer");
    ma_encoder_config config = ma_encoder_config_init(ma_encoding_format_wav, to_ma_format(options.sampleFormat), options.channels, options.sampleRate);
    ma_encoder encoder{};
    const auto result = ma_encoder_init_file(path.string().c_str(), &config, &encoder);
    if (result != MA_SUCCESS) return failure("unable to open WAV encoder: " + path.string());
    AudioPcmBuffer prepared = input;
    if (prepared.format.sampleRate != options.sampleRate) {
        AudioPcmBuffer resampled;
        if (!resample_linear(prepared, resampled, options.sampleRate)) { ma_encoder_uninit(&encoder); return failure("unable to resample PCM for WAV encoding"); }
        prepared = std::move(resampled);
    }
    if (prepared.format.channels != options.channels) {
        AudioPcmBuffer converted;
        converted.format = prepared.format;
        converted.format.channels = options.channels;
        if (!convert_channels(prepared, converted)) { ma_encoder_uninit(&encoder); return failure("unable to convert channels for WAV encoding"); }
        prepared = std::move(converted);
    }
    std::vector<std::int16_t> pcm16;
    std::vector<float> pcmFloat;
    const void* data = prepared.samples.data();
    if (options.sampleFormat == AudioSampleFormat::Int16) {
        pcm16.resize(prepared.samples.size());
        for (std::size_t i = 0; i < pcm16.size(); ++i) pcm16[i] = static_cast<std::int16_t>(std::clamp(prepared.samples[i], -1.0f, 1.0f) * 32767.0f);
        data = pcm16.data();
    } else if (options.sampleFormat != AudioSampleFormat::Float32) {
        ma_encoder_uninit(&encoder);
        return failure("WAV encoding currently supports Float32 and Int16 PCM");
    }
    ma_uint64 written{};
    const auto writeResult = ma_encoder_write_pcm_frames(&encoder, data, prepared.frame_count(), &written);
    ma_encoder_uninit(&encoder);
    return writeResult == MA_SUCCESS && written == prepared.frame_count() ? success() : failure("unable to write WAV PCM");
#else
    (void)path; (void)input; (void)options;
    return failure("WAV encoding requires SHINKOU_WITH_MINIAUDIO");
#endif
}

AudioCodecResult AudioCodec::transcode_ffmpeg(const AudioTranscodeOptions& options,
                                              const std::filesystem::path& ffmpegExecutable) {
    if (options.input.empty() || options.output.empty()) return failure("input and output paths are required");
    if (!options.overwrite && std::filesystem::exists(options.output)) return failure("output already exists: " + options.output.string());
    std::ostringstream command;
    command << quote_argument(ffmpegExecutable) << " -hide_banner -loglevel error ";
    command << (options.overwrite ? "-y " : "-n ") << "-i " << quote_argument(options.input);
    command << " -ar " << options.encoding.sampleRate << " -ac " << options.encoding.channels << " -c:a pcm_s16le " << quote_argument(options.output);
    return std::system(command.str().c_str()) == 0 ? success() : failure("ffmpeg transcoding failed");
}
}

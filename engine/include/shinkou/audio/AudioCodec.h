#pragma once

#include "shinkou/audio/AudioDSP.h"
#include <filesystem>
#include <string>

namespace shinkou::audio {

struct AudioDecodeOptions {
    AudioFormat outputFormat{};
    bool preserveNativeChannels{false};
};

struct AudioEncodeOptions {
    AudioSampleFormat sampleFormat{AudioSampleFormat::Int16};
    AudioChannelLayout layout{AudioChannelLayout::Stereo};
    std::uint32_t channels{2};
    std::uint32_t sampleRate{48000};
};

struct AudioTranscodeOptions {
    std::filesystem::path input{};
    std::filesystem::path output{};
    AudioEncodeOptions encoding{};
    bool overwrite{false};
};

struct AudioCodecResult {
    bool success{false};
    std::string error{};
};

class AudioCodec final {
public:
    static AudioCodecResult decode(const std::filesystem::path& path, AudioPcmBuffer& output,
                                   const AudioDecodeOptions& options = {});
    static AudioCodecResult encode_wav(const std::filesystem::path& path, const AudioPcmBuffer& input,
                                       const AudioEncodeOptions& options = {});
    static AudioCodecResult transcode_ffmpeg(const AudioTranscodeOptions& options,
                                             const std::filesystem::path& ffmpegExecutable = "ffmpeg");
};
}

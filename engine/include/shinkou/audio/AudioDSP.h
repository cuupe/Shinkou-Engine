#pragma once

#include "shinkou/Types.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace shinkou::audio {

constexpr std::uint32_t MaxAudioChannels = 32;

enum class AudioSampleFormat : std::uint8_t {
    Float32,
    Int16,
    Int24,
    Int32
};

enum class AudioChannelLayout : std::uint8_t {
    Mono,
    Stereo,
    Quad,
    FivePointOne,
    SevenPointOne,
    AmbisonicBFormat,
    Custom
};

struct AudioFormat {
    AudioSampleFormat sampleFormat{AudioSampleFormat::Float32};
    AudioChannelLayout layout{AudioChannelLayout::Stereo};
    std::uint32_t channels{2};
    std::uint32_t sampleRate{48000};
};

struct AudioPcmBuffer {
    AudioFormat format{};
    std::vector<float> samples{};

    std::size_t frame_count() const noexcept {
        return format.channels == 0 ? 0 : samples.size() / format.channels;
    }
    float* frame(std::uint32_t index) noexcept { return samples.data() + static_cast<std::size_t>(index) * format.channels; }
    const float* frame(std::uint32_t index) const noexcept { return samples.data() + static_cast<std::size_t>(index) * format.channels; }
    bool valid() const noexcept {
        return format.channels > 0 && format.sampleRate > 0 &&
               samples.size() % format.channels == 0;
    }
    void resize_frames(std::size_t frames) { samples.resize(frames * format.channels); }
};

struct AudioRealtimeBuffer {
    AudioFormat format{};
    float* samples{nullptr};
    std::uint32_t capacityFrames{0};
    std::uint32_t frames{0};

    bool valid() const noexcept {
        return samples != nullptr && format.channels > 0 &&
               format.channels <= MaxAudioChannels && frames <= capacityFrames;
    }
    std::uint32_t frame_count() const noexcept { return frames; }
    float* frame(std::uint32_t index) noexcept { return samples + static_cast<std::size_t>(index) * format.channels; }
    const float* frame(std::uint32_t index) const noexcept { return samples + static_cast<std::size_t>(index) * format.channels; }
};

enum class AudioEffectType : std::uint8_t {
    Gain,
    Pan,
    LowPass,
    HighPass,
    Compressor,
    Limiter,
    Delay,
    Reverb
};

struct AudioEffectDesc {
    AudioEffectType type{AudioEffectType::Gain};
    float amount{1.0f};
    float pan{0.0f};
    float cutoffHz{12000.0f};
    float q{0.707f};
    float thresholdDb{-12.0f};
    float ratio{4.0f};
    float attackSeconds{0.005f};
    float releaseSeconds{0.08f};
    float delaySeconds{0.25f};
    float feedback{0.35f};
    float wet{0.25f};
};

class AudioEffectChain final {
public:
    struct EffectState {
        AudioEffectDesc desc{};
        std::array<float, MaxAudioChannels> previous{};
        std::unique_ptr<float[]> delayBuffer{};
        std::size_t delayCapacityFrames{0};
        std::size_t cursor{0};
        float envelope{1.0f};
    };

private:
    AudioFormat format_{};
    std::vector<EffectState> effects_;

    void prepare_state(EffectState& state);

public:
    AudioEffectChain() = default;
    explicit AudioEffectChain(AudioFormat format) : format_(format) {}

    void set_format(AudioFormat format);
    const AudioFormat& format() const noexcept { return format_; }
    void clear();
    void add(AudioEffectDesc effect);
    std::size_t effect_count() const noexcept { return effects_.size(); }
    bool process(AudioPcmBuffer& buffer);
    bool process(AudioRealtimeBuffer& buffer) noexcept;
    float latency_seconds() const noexcept;
};

bool mix_add(AudioPcmBuffer& destination, const AudioPcmBuffer& source, float gain = 1.0f);
bool convert_channels(const AudioPcmBuffer& source, AudioPcmBuffer& destination);
bool resample_linear(const AudioPcmBuffer& source, AudioPcmBuffer& destination, std::uint32_t sampleRate);
void apply_gain(AudioPcmBuffer& buffer, float gain);
void hard_clip(AudioPcmBuffer& buffer, float limit = 1.0f);
}

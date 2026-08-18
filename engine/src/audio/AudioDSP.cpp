#include "shinkou/audio/AudioDSP.h"

#include <algorithm>
#include <cmath>

namespace shinkou::audio {

namespace {
constexpr float Pi = 3.14159265358979323846f;

float db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }
float clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }

template<class Buffer>
void process_filter(Buffer& buffer, AudioEffectChain::EffectState& state, bool highPass) {
    const auto channels = buffer.format.channels;
    const auto sampleRate = static_cast<float>(buffer.format.sampleRate);
    const auto cutoff = std::clamp(state.desc.cutoffHz, 10.0f, sampleRate * 0.49f);
    const auto alpha = std::exp(-2.0f * Pi * cutoff / sampleRate);
    for (std::size_t frame = 0; frame < buffer.frame_count(); ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            auto& sample = buffer.frame(static_cast<std::uint32_t>(frame))[channel];
            const auto input = sample;
            const auto low = (1.0f - alpha) * input + alpha * state.previous[channel];
            state.previous[channel] = low;
            if (highPass) {
                sample = input - low;
            } else {
                sample = low;
            }
        }
    }
}

template<class Buffer>
void process_delay(Buffer& buffer, AudioEffectChain::EffectState& state) {
    const auto channels = buffer.format.channels;
    const auto delayFrames = std::max<std::size_t>(1, static_cast<std::size_t>(state.desc.delaySeconds * buffer.format.sampleRate));
    if (!state.delayBuffer || state.delayCapacityFrames < delayFrames) return;
    auto cursor = state.cursor % delayFrames;
    const auto wet = clamp01(state.desc.wet);
    const auto feedback = clamp01(state.desc.feedback);
    for (std::size_t frame = 0; frame < buffer.frame_count(); ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto index = cursor * channels + channel;
            const auto delayed = state.delayBuffer[index];
            auto& sample = buffer.frame(static_cast<std::uint32_t>(frame))[channel];
            state.delayBuffer[index] = sample + delayed * feedback;
            sample = sample * (1.0f - wet) + delayed * wet;
        }
        cursor = (cursor + 1) % delayFrames;
    }
    state.cursor = cursor;
}

template<class Buffer>
void process_compressor(Buffer& buffer, AudioEffectChain::EffectState& state, bool limiter) {
    const auto threshold = limiter ? db_to_linear(-0.3f) : db_to_linear(state.desc.thresholdDb);
    const auto ratio = limiter ? 20.0f : std::max(state.desc.ratio, 1.0f);
    const auto attack = std::exp(-1.0f / (std::max(state.desc.attackSeconds, 0.0001f) * buffer.format.sampleRate));
    const auto release = std::exp(-1.0f / (std::max(state.desc.releaseSeconds, 0.0001f) * buffer.format.sampleRate));
    const auto channels = buffer.format.channels;
    for (std::size_t frame = 0; frame < buffer.frame_count(); ++frame) {
        float peak = 0.0f;
        for (std::size_t channel = 0; channel < channels; ++channel) peak = std::max(peak, std::abs(buffer.frame(static_cast<std::uint32_t>(frame))[channel]));
        const auto desired = peak > threshold ? threshold / peak : 1.0f;
        const auto target = std::pow(desired, 1.0f / ratio);
        state.envelope = target < state.envelope ? attack * state.envelope + (1.0f - attack) * target
                                                   : release * state.envelope + (1.0f - release) * target;
        for (std::size_t channel = 0; channel < channels; ++channel) buffer.frame(static_cast<std::uint32_t>(frame))[channel] *= state.envelope;
    }
}
}

void AudioEffectChain::set_format(AudioFormat format) {
    format_ = format;
    for (auto& effect : effects_) {
        prepare_state(effect);
    }
}

void AudioEffectChain::prepare_state(EffectState& state) {
    state.previous.fill(0.0f);
    state.cursor = 0;
    state.envelope = 1.0f;
    if (state.desc.type == AudioEffectType::Delay || state.desc.type == AudioEffectType::Reverb) {
        const auto frames = std::max<std::size_t>(1, static_cast<std::size_t>(state.desc.delaySeconds * format_.sampleRate));
        if (state.delayCapacityFrames < frames) {
            state.delayBuffer = std::make_unique<float[]>(frames * format_.channels);
            state.delayCapacityFrames = frames;
        }
        std::fill_n(state.delayBuffer.get(), state.delayCapacityFrames * format_.channels, 0.0f);
    }
}

void AudioEffectChain::clear() { effects_.clear(); }
void AudioEffectChain::add(AudioEffectDesc effect) {
    effects_.push_back({});
    effects_.back().desc = effect;
    prepare_state(effects_.back());
}

bool AudioEffectChain::process(AudioPcmBuffer& buffer) {
    if (!buffer.valid() || buffer.format.channels > MaxAudioChannels || format_.channels > MaxAudioChannels || buffer.format.channels != format_.channels || buffer.format.sampleRate != format_.sampleRate) return false;
    for (auto& state : effects_) {
        switch (state.desc.type) {
        case AudioEffectType::Gain:
            apply_gain(buffer, state.desc.amount);
            break;
        case AudioEffectType::Pan:
            if (buffer.format.channels >= 2) {
                const auto pan = std::clamp(state.desc.pan, -1.0f, 1.0f);
                const auto left = std::cos((pan + 1.0f) * Pi * 0.25f);
                const auto right = std::sin((pan + 1.0f) * Pi * 0.25f);
                for (std::size_t frame = 0; frame < buffer.frame_count(); ++frame) {
                    buffer.frame(static_cast<std::uint32_t>(frame))[0] *= left;
                    buffer.frame(static_cast<std::uint32_t>(frame))[1] *= right;
                }
            }
            break;
        case AudioEffectType::LowPass: process_filter(buffer, state, false); break;
        case AudioEffectType::HighPass: process_filter(buffer, state, true); break;
        case AudioEffectType::Compressor: process_compressor(buffer, state, false); break;
        case AudioEffectType::Limiter: process_compressor(buffer, state, true); break;
        case AudioEffectType::Delay:
        case AudioEffectType::Reverb: process_delay(buffer, state); break;
        }
    }
    return true;
}

bool AudioEffectChain::process(AudioRealtimeBuffer& buffer) noexcept {
    if (!buffer.valid() || buffer.format.channels != format_.channels || buffer.format.sampleRate != format_.sampleRate) return false;
    for (auto& state : effects_) {
        switch (state.desc.type) {
        case AudioEffectType::Gain:
            for (std::uint32_t frame = 0; frame < buffer.frames; ++frame) for (std::uint32_t channel = 0; channel < buffer.format.channels; ++channel) buffer.frame(frame)[channel] *= state.desc.amount;
            break;
        case AudioEffectType::LowPass: process_filter(buffer, state, false); break;
        case AudioEffectType::HighPass: process_filter(buffer, state, true); break;
        case AudioEffectType::Compressor: process_compressor(buffer, state, false); break;
        case AudioEffectType::Limiter: process_compressor(buffer, state, true); break;
        case AudioEffectType::Delay:
        case AudioEffectType::Reverb: process_delay(buffer, state); break;
        case AudioEffectType::Pan:
            if (buffer.format.channels >= 2) {
                const auto pan = std::clamp(state.desc.pan, -1.0f, 1.0f);
                const auto left = std::cos((pan + 1.0f) * Pi * 0.25f);
                const auto right = std::sin((pan + 1.0f) * Pi * 0.25f);
                for (std::uint32_t frame = 0; frame < buffer.frames; ++frame) {
                    buffer.frame(frame)[0] *= left;
                    buffer.frame(frame)[1] *= right;
                }
            }
            break;
        }
    }
    return true;
}

float AudioEffectChain::latency_seconds() const noexcept {
    float latency = 0.0f;
    for (const auto& effect : effects_) if (effect.desc.type == AudioEffectType::Delay || effect.desc.type == AudioEffectType::Reverb) latency += std::max(effect.desc.delaySeconds, 0.0f);
    return latency;
}

bool mix_add(AudioPcmBuffer& destination, const AudioPcmBuffer& source, float gain) {
    if (!destination.valid() || !source.valid() || destination.format.channels != source.format.channels || destination.format.sampleRate != source.format.sampleRate) return false;
    const auto count = std::min(destination.samples.size(), source.samples.size());
    for (std::size_t i = 0; i < count; ++i) destination.samples[i] += source.samples[i] * gain;
    return true;
}

bool convert_channels(const AudioPcmBuffer& source, AudioPcmBuffer& destination) {
    if (!source.valid() || destination.format.channels == 0 || destination.format.sampleRate != source.format.sampleRate) return false;
    destination.resize_frames(source.frame_count());
    const auto in = source.format.channels;
    const auto out = destination.format.channels;
    for (std::size_t frame = 0; frame < source.frame_count(); ++frame) {
        if (in == out) {
            for (std::size_t channel = 0; channel < out; ++channel) destination.samples[frame * out + channel] = source.samples[frame * in + channel];
        } else if (out == 1) {
            float sum = 0.0f;
            for (std::size_t channel = 0; channel < in; ++channel) sum += source.samples[frame * in + channel];
            destination.samples[frame] = sum / static_cast<float>(in);
        } else {
            for (std::size_t channel = 0; channel < out; ++channel) destination.samples[frame * out + channel] = source.samples[frame * in + std::min(channel, static_cast<std::size_t>(in - 1))];
        }
    }
    return true;
}

bool resample_linear(const AudioPcmBuffer& source, AudioPcmBuffer& destination, std::uint32_t sampleRate) {
    if (!source.valid() || source.frame_count() == 0 || sampleRate == 0) return false;
    destination.format = source.format;
    destination.format.sampleRate = sampleRate;
    const auto outputFrames = static_cast<std::size_t>(std::ceil(static_cast<double>(source.frame_count()) * sampleRate / source.format.sampleRate));
    destination.resize_frames(outputFrames);
    const auto channels = source.format.channels;
    for (std::size_t frame = 0; frame < outputFrames; ++frame) {
        const auto sourcePosition = static_cast<double>(frame) * source.format.sampleRate / sampleRate;
        const auto lower = std::min<std::size_t>(static_cast<std::size_t>(sourcePosition), source.frame_count() - 1);
        const auto upper = std::min(lower + 1, source.frame_count() - 1);
        const auto weight = static_cast<float>(sourcePosition - lower);
        for (std::size_t channel = 0; channel < channels; ++channel) destination.samples[frame * channels + channel] = source.samples[lower * channels + channel] * (1.0f - weight) + source.samples[upper * channels + channel] * weight;
    }
    return true;
}

void apply_gain(AudioPcmBuffer& buffer, float gain) { for (auto& sample : buffer.samples) sample *= gain; }
void hard_clip(AudioPcmBuffer& buffer, float limit) { limit = std::max(std::abs(limit), 0.0001f); for (auto& sample : buffer.samples) sample = std::clamp(sample, -limit, limit); }
}

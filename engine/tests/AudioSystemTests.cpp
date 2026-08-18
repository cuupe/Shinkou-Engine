#include "shinkou/audio/AudioSystem.h"
#include "shinkou/audio/AudioCodec.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <memory>
#include <vector>

namespace {
class TestBackend final : public shinkou::audio::IAudioBackend {
    struct VoiceSlot {
        shinkou::audio::AudioVoiceState state{shinkou::audio::AudioVoiceState::Invalid};
        std::uint32_t generation{1};
        bool active{false};
    };
    struct TrackSlot {
        shinkou::audio::AudioTrackDesc desc{};
        std::uint32_t generation{1};
        bool active{false};
    };
    std::vector<VoiceSlot> voices_;
    std::vector<std::uint32_t> freeVoices_;
    std::vector<TrackSlot> tracks_;
    std::vector<std::uint32_t> freeTracks_;

    bool valid_voice(shinkou::audio::AudioVoiceId voice) const {
        const auto index = shinkou::audio::audio_handle_index(voice);
        return index < voices_.size() && voices_[index].active && voices_[index].generation == shinkou::audio::audio_handle_generation(voice);
    }
    bool valid_track(shinkou::audio::AudioTrackId track) const {
        const auto index = shinkou::audio::audio_handle_index(track);
        return index < tracks_.size() && tracks_[index].active && tracks_[index].generation == shinkou::audio::audio_handle_generation(track);
    }
public:
    bool initialize(const shinkou::audio::AudioConfig& config) override {
        voices_.assign(config.maxVoices, {});
        freeVoices_.clear();
        freeVoices_.reserve(voices_.size());
        for (std::uint32_t i = 0; i < voices_.size(); ++i) freeVoices_.push_back(static_cast<std::uint32_t>(voices_.size() - i - 1u));
        tracks_.assign(config.maxTracks, {});
        freeTracks_.clear();
        freeTracks_.reserve(tracks_.size());
        for (std::uint32_t i = 0; i < tracks_.size(); ++i) freeTracks_.push_back(static_cast<std::uint32_t>(tracks_.size() - i - 1u));
        return true;
    }
    void shutdown() override { voices_.clear(); freeVoices_.clear(); tracks_.clear(); freeTracks_.clear(); }
    void update(shinkou::Seconds) override {}
    shinkou::audio::AudioVoiceId play(const shinkou::audio::AudioAssetDesc&, const shinkou::audio::AudioPlayParams& params) override {
        if (freeVoices_.empty()) return 0;
        const auto index = freeVoices_.back();
        freeVoices_.pop_back();
        auto& slot = voices_[index];
        slot.active = true;
        slot.state = params.startPaused ? shinkou::audio::AudioVoiceState::Paused : shinkou::audio::AudioVoiceState::Playing;
        return shinkou::audio::make_audio_handle(index, slot.generation);
    }
    void stop(shinkou::audio::AudioVoiceId voice, shinkou::Seconds) override { if (valid_voice(voice)) voices_[shinkou::audio::audio_handle_index(voice)].state = shinkou::audio::AudioVoiceState::Stopped; }
    void pause(shinkou::audio::AudioVoiceId voice) override { if (valid_voice(voice)) voices_[shinkou::audio::audio_handle_index(voice)].state = shinkou::audio::AudioVoiceState::Paused; }
    void resume(shinkou::audio::AudioVoiceId voice) override { if (valid_voice(voice)) voices_[shinkou::audio::audio_handle_index(voice)].state = shinkou::audio::AudioVoiceState::Playing; }
    void set_volume(shinkou::audio::AudioVoiceId, float) override {}
    void set_pitch(shinkou::audio::AudioVoiceId, float) override {}
    void set_pan(shinkou::audio::AudioVoiceId, float) override {}
    void set_spatial(shinkou::audio::AudioVoiceId, const shinkou::audio::AudioPlayParams&) override {}
    void set_listener(const shinkou::audio::AudioListener&) override {}
    void set_bus_volume(shinkou::audio::AudioBus, float) override {}
    void set_bus_muted(shinkou::audio::AudioBus, bool) override {}
    shinkou::audio::AudioTrackId create_track(const shinkou::audio::AudioTrackDesc& desc) override {
        if (freeTracks_.empty()) return 0;
        const auto index = freeTracks_.back();
        freeTracks_.pop_back();
        tracks_[index].desc = desc;
        tracks_[index].active = true;
        return shinkou::audio::make_audio_handle(index, tracks_[index].generation);
    }
    void destroy_track(shinkou::audio::AudioTrackId track) override {
        if (!valid_track(track)) return;
        const auto index = shinkou::audio::audio_handle_index(track);
        auto& slot = tracks_[index];
        slot.active = false;
        slot.desc = {};
        slot.generation = (slot.generation + 1u) & shinkou::audio::AudioHandleGenerationMask;
        if (slot.generation == 0) slot.generation = 1;
        freeTracks_.push_back(index);
    }
    void set_track_volume(shinkou::audio::AudioTrackId track, float volume) override { if (valid_track(track)) tracks_[shinkou::audio::audio_handle_index(track)].desc.volume = volume; }
    void set_track_muted(shinkou::audio::AudioTrackId track, bool muted) override { if (valid_track(track)) tracks_[shinkou::audio::audio_handle_index(track)].desc.muted = muted; }
    shinkou::audio::AudioTrackSnapshot track_snapshot(shinkou::audio::AudioTrackId track) const override {
        if (!valid_track(track)) return {};
        const auto& desc = tracks_[shinkou::audio::audio_handle_index(track)].desc;
        return {track, desc.name, desc.bus, desc.volume, desc.muted, 0};
    }
    shinkou::audio::AudioVoiceState state(shinkou::audio::AudioVoiceId voice) const override {
        return valid_voice(voice) ? voices_[shinkou::audio::audio_handle_index(voice)].state : shinkou::audio::AudioVoiceState::Invalid;
    }
    std::uint32_t collect_finished(shinkou::audio::AudioVoiceId* output, std::uint32_t capacity) override {
        std::uint32_t count = 0;
        for (std::uint32_t index = 0; index < voices_.size(); ++index) {
            auto& slot = voices_[index];
            if (!slot.active || slot.state != shinkou::audio::AudioVoiceState::Stopped) continue;
            if (output && count < capacity) output[count++] = shinkou::audio::make_audio_handle(index, slot.generation);
            slot.active = false;
            slot.state = shinkou::audio::AudioVoiceState::Invalid;
            slot.generation = (slot.generation + 1u) & shinkou::audio::AudioHandleGenerationMask;
            if (slot.generation == 0) slot.generation = 1;
            freeVoices_.push_back(index);
        }
        return count;
    }
    void stop_all(shinkou::audio::AudioBus, shinkou::Seconds) override { for (auto& slot : voices_) if (slot.active) slot.state = shinkou::audio::AudioVoiceState::Stopped; }
    std::string last_error() const override { return {}; }
    shinkou::audio::AudioDiagnostics diagnostics() const override { return {}; }
};
}

int main() {
    shinkou::audio::AudioPcmBuffer pcm;
    pcm.format.channels = 1;
    pcm.format.layout = shinkou::audio::AudioChannelLayout::Mono;
    pcm.format.sampleRate = 24000;
    pcm.samples = {0.0f, 0.5f, -0.5f, 1.0f};
    shinkou::audio::AudioPcmBuffer stereo;
    stereo.format.channels = 2;
    stereo.format.layout = shinkou::audio::AudioChannelLayout::Stereo;
    stereo.format.sampleRate = 24000;
    assert(shinkou::audio::convert_channels(pcm, stereo));
    assert(stereo.frame_count() == pcm.frame_count());
    shinkou::audio::AudioPcmBuffer resampled;
    assert(shinkou::audio::resample_linear(stereo, resampled, 48000));
    assert(resampled.frame_count() == 8);
    shinkou::audio::AudioEffectChain effects(resampled.format);
    effects.add({shinkou::audio::AudioEffectType::Gain, 0.5f});
    effects.add({shinkou::audio::AudioEffectType::Limiter});
    assert(effects.process(resampled));
    std::array<float, 16> realtimeSamples{};
    std::fill(realtimeSamples.begin(), realtimeSamples.end(), 1.0f);
    shinkou::audio::AudioRealtimeBuffer realtime;
    realtime.format = resampled.format;
    realtime.samples = realtimeSamples.data();
    realtime.capacityFrames = 8;
    realtime.frames = 8;
    shinkou::audio::AudioEffectChain realtimeEffects(realtime.format);
    realtimeEffects.add({shinkou::audio::AudioEffectType::Pan, 1.0f, -1.0f});
    realtimeEffects.add({shinkou::audio::AudioEffectType::Gain, 0.5f});
    assert(realtimeEffects.process(realtime));
    assert(realtimeSamples[0] > 0.0f && realtimeSamples[1] == 0.0f);

    shinkou::audio::AudioConfig config;
    config.assetRoot = "assets/audio";
    config.maxAssets = 2;
    config.maxVoices = 2;
    config.maxTracks = 1;
    shinkou::audio::AudioSystem audio(std::make_unique<TestBackend>(), config);
    assert(audio.initialize());
    const auto asset = audio.load("ui/click.wav");
    assert(asset != 0);
    assert(audio.load("ui/click.wav") == asset);
    assert(audio.asset_count() == 1);
    shinkou::audio::AudioPlayParams params;
    params.bus = shinkou::audio::AudioBus::UI;
    const auto voice = audio.play(asset, params);
    assert(voice != 0 && audio.is_playing(voice));
    const auto secondVoice = audio.play(asset, params);
    assert(secondVoice != 0 && secondVoice != voice);
    assert(audio.play(asset, params) == 0);
    audio.pause(voice);
    assert(audio.state(voice) == shinkou::audio::AudioVoiceState::Paused);
    audio.resume(voice);
    assert(audio.is_playing(voice));
    audio.set_listener({});
    audio.set_bus_volume(shinkou::audio::AudioBus::UI, 0.5f);
    audio.stop(voice, 0.1f);
    assert(audio.state(voice) == shinkou::audio::AudioVoiceState::Stopped);
    audio.update(0.0f);
    assert(audio.state(voice) == shinkou::audio::AudioVoiceState::Invalid);
    const auto recycledVoice = audio.play(asset, params);
    assert(recycledVoice != 0 && recycledVoice != voice);
    audio.stop(recycledVoice);
    audio.update(0.0f);
    audio.unload(asset);
    assert(audio.asset_count() == 0);
    const auto secondAsset = audio.load("ui/other.wav");
    assert(secondAsset != 0 && secondAsset != asset);
    const auto thirdAsset = audio.load("ui/third.wav");
    assert(thirdAsset != 0);
    assert(audio.load("ui/fourth.wav") == 0);
    audio.unload(secondAsset);
    audio.unload(thirdAsset);
    const auto track = audio.create_track({"music", shinkou::audio::AudioBus::Music, 0.8f, false});
    assert(track != 0);
    assert(audio.create_track({"overflow", shinkou::audio::AudioBus::Music, 1.0f, false}) == 0);
    audio.set_track_volume(track, 0.6f);
    audio.set_track_muted(track, true);
    audio.destroy_track(track);
    const auto recycledTrack = audio.create_track({"music2", shinkou::audio::AudioBus::Music, 1.0f, false});
    assert(recycledTrack != 0 && recycledTrack != track);
    audio.destroy_track(recycledTrack);
    audio.shutdown();
    assert(audio.initialize());
    const auto afterRestart = audio.load("ui/restart.wav");
    assert(afterRestart != 0);
    audio.shutdown();
    return 0;
}

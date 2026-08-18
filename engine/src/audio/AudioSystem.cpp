#include "shinkou/audio/AudioSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#if defined(SHINKOU_WITH_MINIAUDIO)
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#endif

namespace shinkou::audio {
namespace {
#if defined(SHINKOU_WITH_MINIAUDIO)
float clamp_volume(float value) { return std::clamp(value, 0.0f, 1.0f); }
float clamp_pan(float value) { return std::clamp(value, -1.0f, 1.0f); }
float clamp_pitch(float value) { return std::max(value, 0.01f); }
#endif

std::uint32_t clamp_pool_size(std::uint32_t value) {
    return std::clamp(value, 1u, AudioHandleIndexMask);
}

class NullAudioBackend final : public IAudioBackend {
    struct VoiceSlot {
        AudioVoiceState state{AudioVoiceState::Invalid};
        std::uint32_t generation{1};
        bool active{false};
    };

    std::vector<VoiceSlot> voices_;
    std::vector<std::uint32_t> freeVoices_;
    struct TrackSlot {
        AudioTrackDesc desc{};
        std::uint32_t generation{1};
        bool active{false};
    };
    std::vector<TrackSlot> tracks_;
    std::vector<std::uint32_t> freeTracks_;
    std::string error_;

    bool valid_voice(AudioVoiceId voice) const noexcept {
        const auto index = audio_handle_index(voice);
        return index < voices_.size() && voices_[index].active &&
               voices_[index].generation == audio_handle_generation(voice);
    }
    bool valid_track(AudioTrackId track) const noexcept {
        const auto index = audio_handle_index(track);
        return index < tracks_.size() && tracks_[index].active &&
               tracks_[index].generation == audio_handle_generation(track);
    }

public:
    bool initialize(const AudioConfig& config) override {
        const auto capacity = clamp_pool_size(config.maxVoices);
        voices_.resize(capacity);
        for (auto& slot : voices_) { slot.active = false; slot.state = AudioVoiceState::Invalid; }
        freeVoices_.clear();
        freeVoices_.reserve(capacity);
        for (std::uint32_t i = 0; i < capacity; ++i) freeVoices_.push_back(capacity - i - 1u);
        const auto trackCapacity = clamp_pool_size(config.maxTracks);
        tracks_.resize(trackCapacity);
        for (auto& slot : tracks_) { slot.active = false; slot.desc = {}; }
        freeTracks_.clear();
        freeTracks_.reserve(trackCapacity);
        for (std::uint32_t i = 0; i < trackCapacity; ++i) freeTracks_.push_back(trackCapacity - i - 1u);
        return true;
    }
    void shutdown() override {
        for (auto& slot : voices_) {
            slot.active = false;
            slot.state = AudioVoiceState::Invalid;
            slot.generation = (slot.generation + 1u) & AudioHandleGenerationMask;
            if (slot.generation == 0) slot.generation = 1;
        }
        for (auto& slot : tracks_) {
            slot.active = false;
            slot.desc = {};
            slot.generation = (slot.generation + 1u) & AudioHandleGenerationMask;
            if (slot.generation == 0) slot.generation = 1;
        }
        freeVoices_.clear();
        freeTracks_.clear();
    }
    void update(Seconds) override {}

    AudioVoiceId play(const AudioAssetDesc&, const AudioPlayParams& params) override {
        if (freeVoices_.empty()) return 0;
        const auto index = freeVoices_.back();
        freeVoices_.pop_back();
        auto& slot = voices_[index];
        slot.active = true;
        slot.state = params.startPaused ? AudioVoiceState::Paused : AudioVoiceState::Playing;
        return make_audio_handle(index, slot.generation);
    }
    void stop(AudioVoiceId voice, Seconds) override {
        if (valid_voice(voice)) voices_[audio_handle_index(voice)].state = AudioVoiceState::Stopped;
    }
    void pause(AudioVoiceId voice) override {
        if (valid_voice(voice) && voices_[audio_handle_index(voice)].state == AudioVoiceState::Playing) voices_[audio_handle_index(voice)].state = AudioVoiceState::Paused;
    }
    void resume(AudioVoiceId voice) override {
        if (valid_voice(voice) && voices_[audio_handle_index(voice)].state == AudioVoiceState::Paused) voices_[audio_handle_index(voice)].state = AudioVoiceState::Playing;
    }
    void set_volume(AudioVoiceId, float) override {}
    void set_pitch(AudioVoiceId, float) override {}
    void set_pan(AudioVoiceId, float) override {}
    void set_spatial(AudioVoiceId, const AudioPlayParams&) override {}
    void set_listener(const AudioListener&) override {}
    void set_bus_volume(AudioBus, float) override {}
    void set_bus_muted(AudioBus, bool) override {}
    AudioTrackId create_track(const AudioTrackDesc& desc) override {
        if (freeTracks_.empty()) return 0;
        const auto index = freeTracks_.back();
        freeTracks_.pop_back();
        auto& slot = tracks_[index];
        slot.desc = desc;
        slot.active = true;
        return make_audio_handle(index, slot.generation);
    }
    void destroy_track(AudioTrackId track) override {
        if (!valid_track(track)) return;
        const auto index = audio_handle_index(track);
        auto& slot = tracks_[index];
        slot.active = false;
        slot.desc = {};
        slot.generation = (slot.generation + 1u) & AudioHandleGenerationMask;
        if (slot.generation == 0) slot.generation = 1;
        freeTracks_.push_back(index);
    }
    void set_track_volume(AudioTrackId track, float volume) override { if (valid_track(track)) tracks_[audio_handle_index(track)].desc.volume = std::clamp(volume, 0.0f, 1.0f); }
    void set_track_muted(AudioTrackId track, bool muted) override { if (valid_track(track)) tracks_[audio_handle_index(track)].desc.muted = muted; }
    AudioTrackSnapshot track_snapshot(AudioTrackId track) const override {
        if (!valid_track(track)) return {};
        const auto& desc = tracks_[audio_handle_index(track)].desc;
        return {track, desc.name, desc.bus, desc.volume, desc.muted, 0};
    }
    AudioVoiceState state(AudioVoiceId voice) const override { return valid_voice(voice) ? voices_[audio_handle_index(voice)].state : AudioVoiceState::Invalid; }

    std::uint32_t collect_finished(AudioVoiceId* output, std::uint32_t capacity) override {
        std::uint32_t count = 0;
        for (std::uint32_t index = 0; index < voices_.size(); ++index) {
            auto& slot = voices_[index];
            if (!slot.active || (slot.state != AudioVoiceState::Stopped && slot.state != AudioVoiceState::Finished)) continue;
            if (output && count < capacity) output[count++] = make_audio_handle(index, slot.generation);
            slot.active = false;
            slot.state = AudioVoiceState::Invalid;
            slot.generation = (slot.generation + 1u) & AudioHandleGenerationMask;
            if (slot.generation == 0) slot.generation = 1;
            freeVoices_.push_back(index);
        }
        return count;
    }
    void stop_all(AudioBus, Seconds) override { for (auto& slot : voices_) if (slot.active) slot.state = AudioVoiceState::Stopped; }
    std::string last_error() const override { return error_; }
    AudioDiagnostics diagnostics() const override {
        AudioDiagnostics result;
        for (const auto& slot : voices_) if (slot.active) ++result.activeVoices;
        return result;
    }
};

#if defined(SHINKOU_WITH_MINIAUDIO)
class MiniaudioBackend final : public IAudioBackend {
    struct VoiceSlot {
        ma_sound sound{};
        AudioBus bus{AudioBus::Sfx};
        AudioTrackId track{0};
        std::uint32_t generation{1};
        bool initialized{false};
    };
    struct TrackSlot {
        AudioTrackDesc desc{};
        ma_sound_group group{};
        std::uint32_t generation{1};
        bool initialized{false};
    };

    ma_engine engine_{};
    std::array<ma_sound_group, static_cast<std::size_t>(AudioBus::Count)> buses_{};
    std::array<bool, static_cast<std::size_t>(AudioBus::Count)> busInitialized_{};
    std::vector<VoiceSlot> voices_;
    std::vector<std::uint32_t> freeVoices_;
    std::vector<TrackSlot> tracks_;
    std::vector<std::uint32_t> freeTracks_;
    std::array<float, static_cast<std::size_t>(AudioBus::Count)> busVolumes_{};
    std::array<bool, static_cast<std::size_t>(AudioBus::Count)> busMuted_{};
    AudioTrackId nextTrack_{1};
    std::uint32_t maxVoices_{0};
    bool initialized_{false};
    std::string error_;

    static std::size_t bus_index(AudioBus bus) { return static_cast<std::size_t>(bus); }
    bool valid_voice(AudioVoiceId voice) const noexcept {
        const auto index = audio_handle_index(voice);
        return index < voices_.size() && voices_[index].initialized && voices_[index].generation == audio_handle_generation(voice);
    }
    bool valid_track(AudioTrackId track) const noexcept {
        const auto index = audio_handle_index(track);
        return index < tracks_.size() && tracks_[index].initialized && tracks_[index].generation == audio_handle_generation(track);
    }
    ma_sound_group* bus_group(AudioBus bus) {
        if (bus == AudioBus::Master || bus_index(bus) >= busInitialized_.size()) return nullptr;
        return busInitialized_[bus_index(bus)] ? &buses_[bus_index(bus)] : nullptr;
    }
    ma_sound_group* track_group(AudioTrackId track) {
        return valid_track(track) ? &tracks_[audio_handle_index(track)].group : nullptr;
    }
    void set_error(ma_result result, std::string_view action) {
        error_ = std::string(action) + " (miniaudio error " + std::to_string(static_cast<int>(result)) + ")";
    }
    void release_voice(std::uint32_t index) {
        auto& slot = voices_[index];
        if (!slot.initialized) return;
        ma_sound_uninit(&slot.sound);
        slot.initialized = false;
        slot.generation = (slot.generation + 1u) & AudioHandleGenerationMask;
        if (slot.generation == 0) slot.generation = 1;
        freeVoices_.push_back(index);
    }
    void release_track(std::uint32_t index) {
        auto& slot = tracks_[index];
        if (!slot.initialized) return;
        ma_sound_group_uninit(&slot.group);
        slot.desc = {};
        slot.initialized = false;
        slot.generation = (slot.generation + 1u) & AudioHandleGenerationMask;
        if (slot.generation == 0) slot.generation = 1;
        freeTracks_.push_back(index);
    }

public:
    bool initialize(const AudioConfig& config) override {
        maxVoices_ = clamp_pool_size(config.maxVoices);
        voices_.resize(maxVoices_);
        freeVoices_.clear();
        freeVoices_.reserve(maxVoices_);
        for (std::uint32_t i = 0; i < maxVoices_; ++i) freeVoices_.push_back(maxVoices_ - i - 1u);
        const auto trackCapacity = clamp_pool_size(config.maxTracks);
        tracks_.resize(trackCapacity);
        freeTracks_.clear();
        freeTracks_.reserve(trackCapacity);
        for (std::uint32_t i = 0; i < trackCapacity; ++i) freeTracks_.push_back(trackCapacity - i - 1u);
        busVolumes_.fill(1.0f);
        busMuted_.fill(false);

        ma_engine_config engineConfig = ma_engine_config_init();
        engineConfig.noAutoStart = config.startDevice ? MA_FALSE : MA_TRUE;
        engineConfig.channels = std::max(config.outputChannels, 1u);
        engineConfig.sampleRate = std::max(config.mixFormat.sampleRate, 1u);
        engineConfig.periodSizeInFrames = std::max(config.audioThreadBufferFrames, 1u);
        const auto result = ma_engine_init(&engineConfig, &engine_);
        if (result != MA_SUCCESS) { set_error(result, "audio device initialization failed"); return false; }
        initialized_ = true;
        for (std::size_t i = 1; i < busInitialized_.size(); ++i) {
            const auto busResult = ma_sound_group_init(&engine_, 0, nullptr, &buses_[i]);
            if (busResult != MA_SUCCESS) { set_error(busResult, "audio bus initialization failed"); shutdown(); return false; }
            busInitialized_[i] = true;
        }
        return true;
    }
    void shutdown() override {
        for (std::uint32_t i = 0; i < voices_.size(); ++i) if (voices_[i].initialized) ma_sound_uninit(&voices_[i].sound);
        for (std::uint32_t i = 0; i < tracks_.size(); ++i) if (tracks_[i].initialized) ma_sound_group_uninit(&tracks_[i].group);
        for (auto& slot : voices_) {
            slot.initialized = false;
            slot.generation = (slot.generation + 1u) & AudioHandleGenerationMask;
            if (slot.generation == 0) slot.generation = 1;
        }
        for (auto& slot : tracks_) {
            slot.initialized = false;
            slot.desc = {};
            slot.generation = (slot.generation + 1u) & AudioHandleGenerationMask;
            if (slot.generation == 0) slot.generation = 1;
        }
        freeVoices_.clear(); freeTracks_.clear();
        for (std::size_t i = 1; i < busInitialized_.size(); ++i) { if (busInitialized_[i]) ma_sound_group_uninit(&buses_[i]); busInitialized_[i] = false; }
        if (initialized_) ma_engine_uninit(&engine_);
        initialized_ = false;
    }
    void update(Seconds) override {}
    AudioVoiceId play(const AudioAssetDesc& asset, const AudioPlayParams& params) override {
        if (!initialized_ || freeVoices_.empty()) return 0;
        const auto index = freeVoices_.back();
        auto& slot = voices_[index];
        const auto flags = (params.streaming || asset.streaming) ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
        auto* parent = params.track != 0 ? track_group(params.track) : bus_group(params.bus);
        if (ma_sound_init_from_file(&engine_, asset.path.string().c_str(), flags, parent, nullptr, &slot.sound) != MA_SUCCESS) { error_ = "audio asset load failed: " + asset.path.string(); return 0; }
        slot.bus = params.bus; slot.track = params.track; slot.initialized = true;
        ma_sound_set_looping(&slot.sound, params.loop ? MA_TRUE : MA_FALSE);
        ma_sound_set_volume(&slot.sound, clamp_volume(params.volume));
        ma_sound_set_pitch(&slot.sound, clamp_pitch(params.pitch));
        ma_sound_set_pan(&slot.sound, clamp_pan(params.pan));
        ma_sound_set_spatialization_enabled(&slot.sound, params.spatialized ? MA_TRUE : MA_FALSE);
        ma_sound_set_position(&slot.sound, params.position.x, params.position.y, params.position.z);
        ma_sound_set_velocity(&slot.sound, params.velocity.x, params.velocity.y, params.velocity.z);
        ma_sound_set_min_distance(&slot.sound, std::max(params.minDistance, 0.001f));
        ma_sound_set_max_distance(&slot.sound, std::max(params.maxDistance, params.minDistance + 0.001f));
        ma_sound_set_rolloff(&slot.sound, std::max(params.rolloff, 0.0f));
        if (params.fadeInSeconds > 0.0f) ma_sound_set_fade_in_milliseconds(&slot.sound, 0.0f, clamp_volume(params.volume), static_cast<ma_uint64>(params.fadeInSeconds * 1000.0f));
        const auto voice = make_audio_handle(index, slot.generation);
        freeVoices_.pop_back();
        if (ma_sound_start(&slot.sound) != MA_SUCCESS) { release_voice(index); error_ = "audio voice start failed"; return 0; }
        if (params.startPaused) ma_sound_stop(&slot.sound);
        return voice;
    }
    void stop(AudioVoiceId voice, Seconds fadeOutSeconds) override { if (valid_voice(voice)) { auto& sound = voices_[audio_handle_index(voice)].sound; if (fadeOutSeconds > 0.0f) ma_sound_stop_with_fade_in_milliseconds(&sound, static_cast<ma_uint64>(fadeOutSeconds * 1000.0f)); else ma_sound_stop(&sound); } }
    void pause(AudioVoiceId voice) override { if (valid_voice(voice)) ma_sound_stop(&voices_[audio_handle_index(voice)].sound); }
    void resume(AudioVoiceId voice) override { if (valid_voice(voice)) ma_sound_start(&voices_[audio_handle_index(voice)].sound); }
    void set_volume(AudioVoiceId voice, float volume) override { if (valid_voice(voice)) ma_sound_set_volume(&voices_[audio_handle_index(voice)].sound, clamp_volume(volume)); }
    void set_pitch(AudioVoiceId voice, float pitch) override { if (valid_voice(voice)) ma_sound_set_pitch(&voices_[audio_handle_index(voice)].sound, clamp_pitch(pitch)); }
    void set_pan(AudioVoiceId voice, float pan) override { if (valid_voice(voice)) ma_sound_set_pan(&voices_[audio_handle_index(voice)].sound, clamp_pan(pan)); }
    void set_spatial(AudioVoiceId voice, const AudioPlayParams& params) override {
        if (!valid_voice(voice)) return;
        auto& sound = voices_[audio_handle_index(voice)].sound;
        ma_sound_set_spatialization_enabled(&sound, params.spatialized ? MA_TRUE : MA_FALSE);
        ma_sound_set_position(&sound, params.position.x, params.position.y, params.position.z);
        ma_sound_set_velocity(&sound, params.velocity.x, params.velocity.y, params.velocity.z);
        ma_sound_set_min_distance(&sound, std::max(params.minDistance, 0.001f));
        ma_sound_set_max_distance(&sound, std::max(params.maxDistance, params.minDistance + 0.001f));
        ma_sound_set_rolloff(&sound, std::max(params.rolloff, 0.0f));
    }
    void set_listener(const AudioListener& listener) override { ma_engine_listener_set_position(&engine_, 0, listener.position.x, listener.position.y, listener.position.z); ma_engine_listener_set_direction(&engine_, 0, listener.forward.x, listener.forward.y, listener.forward.z); ma_engine_listener_set_world_up(&engine_, 0, listener.up.x, listener.up.y, listener.up.z); ma_engine_listener_set_velocity(&engine_, 0, listener.velocity.x, listener.velocity.y, listener.velocity.z); }
    void set_bus_volume(AudioBus bus, float volume) override { if (bus_index(bus) >= busVolumes_.size()) return; busVolumes_[bus_index(bus)] = clamp_volume(volume); const auto value = busMuted_[bus_index(bus)] ? 0.0f : busVolumes_[bus_index(bus)]; if (bus == AudioBus::Master) ma_engine_set_volume(&engine_, value); else if (auto* group = bus_group(bus)) ma_sound_group_set_volume(group, value); }
    void set_bus_muted(AudioBus bus, bool muted) override { if (bus_index(bus) >= busMuted_.size()) return; busMuted_[bus_index(bus)] = muted; const auto value = muted ? 0.0f : busVolumes_[bus_index(bus)]; if (bus == AudioBus::Master) ma_engine_set_volume(&engine_, value); else if (auto* group = bus_group(bus)) ma_sound_group_set_volume(group, value); }
    AudioTrackId create_track(const AudioTrackDesc& desc) override {
        if (freeTracks_.empty()) return 0;
        const auto index = freeTracks_.back();
        auto& slot = tracks_[index];
        if (ma_sound_group_init(&engine_, 0, bus_group(desc.bus), &slot.group) != MA_SUCCESS) { error_ = "audio track initialization failed"; return 0; }
        freeTracks_.pop_back(); slot.desc = desc; slot.initialized = true; ma_sound_group_set_volume(&slot.group, desc.muted ? 0.0f : clamp_volume(desc.volume));
        return make_audio_handle(index, slot.generation);
    }
    void destroy_track(AudioTrackId track) override {
        if (!valid_track(track)) return;
        const auto index = audio_handle_index(track);
        for (std::uint32_t i = 0; i < voices_.size(); ++i) if (voices_[i].initialized && voices_[i].track == track) release_voice(i);
        release_track(index);
    }
    void set_track_volume(AudioTrackId track, float volume) override { if (valid_track(track)) { auto& slot = tracks_[audio_handle_index(track)]; slot.desc.volume = clamp_volume(volume); ma_sound_group_set_volume(&slot.group, slot.desc.muted ? 0.0f : slot.desc.volume); } }
    void set_track_muted(AudioTrackId track, bool muted) override { if (valid_track(track)) { auto& slot = tracks_[audio_handle_index(track)]; slot.desc.muted = muted; ma_sound_group_set_volume(&slot.group, muted ? 0.0f : slot.desc.volume); } }
    AudioTrackSnapshot track_snapshot(AudioTrackId track) const override {
        if (!valid_track(track)) return {};
        const auto index = audio_handle_index(track);
        const auto& desc = tracks_[index].desc;
        AudioTrackSnapshot result{track, desc.name, desc.bus, desc.volume, desc.muted, 0};
        for (const auto& voice : voices_) if (voice.initialized && voice.track == track && ma_sound_is_playing(&voice.sound)) ++result.activeVoices;
        return result;
    }
    AudioVoiceState state(AudioVoiceId voice) const override { if (!valid_voice(voice)) return AudioVoiceState::Invalid; const auto& sound = voices_[audio_handle_index(voice)].sound; if (ma_sound_is_playing(&sound)) return AudioVoiceState::Playing; return ma_sound_at_end(&sound) ? AudioVoiceState::Finished : AudioVoiceState::Paused; }
    std::uint32_t collect_finished(AudioVoiceId* output, std::uint32_t capacity) override {
        std::uint32_t count = 0;
        for (std::uint32_t i = 0; i < voices_.size(); ++i) {
            if (!voices_[i].initialized || !ma_sound_at_end(&voices_[i].sound)) continue;
            const auto handle = make_audio_handle(i, voices_[i].generation);
            release_voice(i);
            if (output && count < capacity) output[count++] = handle;
        }
        return count;
    }
    void stop_all(AudioBus bus, Seconds fadeOutSeconds) override { for (auto& voice : voices_) if (voice.initialized && (bus == AudioBus::Master || voice.bus == bus)) { if (fadeOutSeconds > 0.0f) ma_sound_stop_with_fade_in_milliseconds(&voice.sound, static_cast<ma_uint64>(fadeOutSeconds * 1000.0f)); else ma_sound_stop(&voice.sound); } }
    std::string last_error() const override { return error_; }
    AudioDiagnostics diagnostics() const override { AudioDiagnostics result; for (const auto& voice : voices_) if (voice.initialized) ++result.activeVoices; return result; }
    ~MiniaudioBackend() override { shutdown(); }
};
#endif
}

AudioSystem::AudioSystem(std::unique_ptr<IAudioBackend> backend, AudioConfig config)
    : backend_(std::move(backend)), config_(std::move(config)) {
    const auto assetCapacity = clamp_pool_size(config_.maxAssets);
    assets_.resize(assetCapacity);
    freeAssets_.reserve(assetCapacity);
    for (std::uint32_t i = 0; i < assetCapacity; ++i) freeAssets_.push_back(assetCapacity - i - 1u);
    const auto voiceCapacity = clamp_pool_size(config_.maxVoices);
    voiceHandles_.assign(voiceCapacity, 0);
    voiceAssets_.assign(voiceCapacity, 0);
    finishedVoices_.resize(voiceCapacity);
    tracks_.resize(clamp_pool_size(config_.maxTracks));
}

AudioSystem::~AudioSystem() { shutdown(); }
std::filesystem::path AudioSystem::resolve_path(const std::filesystem::path& path) const { return (path.is_absolute() || config_.assetRoot.empty() ? path : config_.assetRoot / path).lexically_normal(); }
AudioAssetId AudioSystem::find_asset(const std::filesystem::path& path) const { const auto resolved = resolve_path(path); for (const auto& asset : assets_) if (asset.active && asset.desc.path == resolved) return asset.id; return 0; }
bool AudioSystem::initialize() { if (initialized_) return true; if (!backend_ || !backend_->initialize(config_)) { lastError_ = backend_ ? backend_->last_error() : "audio backend is missing"; return false; } initialized_ = true; return true; }
void AudioSystem::shutdown() {
    if (backend_) backend_->shutdown();
    for (auto& asset : assets_) { asset.active = false; asset.id = 0; asset.desc = {}; }
    freeAssets_.clear();
    for (std::uint32_t i = 0; i < assets_.size(); ++i) freeAssets_.push_back(static_cast<std::uint32_t>(assets_.size() - i - 1u));
    for (auto& handle : voiceHandles_) handle = 0;
    for (auto& asset : voiceAssets_) asset = 0;
    for (auto& track : tracks_) { track.snapshot = {}; track.name.clear(); track.active = false; }
    assetCount_ = 0;
    trackCount_ = 0;
    initialized_ = false;
}
void AudioSystem::update(Seconds dt) { if (!initialized_ || !backend_) return; backend_->update(std::max(dt, 0.0f)); forget_finished_voices(); diagnostics_ = backend_->diagnostics(); if (lastError_.empty()) lastError_ = backend_->last_error(); }
void AudioSystem::forget_finished_voices() { const auto count = backend_->collect_finished(finishedVoices_.data(), static_cast<std::uint32_t>(finishedVoices_.size())); for (std::uint32_t i = 0; i < count; ++i) { const auto index = audio_handle_index(finishedVoices_[i]); if (index < voiceHandles_.size() && voiceHandles_[index] == finishedVoices_[i]) { voiceHandles_[index] = 0; voiceAssets_[index] = 0; } } }
AudioAssetId AudioSystem::load(const AudioAssetDesc& asset) { const auto resolved = resolve_path(asset.path); if (resolved.empty()) { lastError_ = "audio asset path is empty"; return 0; } if (const auto existing = find_asset(asset.path); existing != 0) return existing; if (freeAssets_.empty()) { lastError_ = "audio asset pool exhausted"; return 0; } const auto index = freeAssets_.back(); freeAssets_.pop_back(); auto& slot = assets_[index]; slot.desc = asset; slot.desc.path = resolved; slot.active = true; ++assetCount_; slot.id = make_audio_handle(index, slot.generation); return slot.id; }
AudioAssetId AudioSystem::load(std::filesystem::path path, bool streaming) { return load({std::move(path), streaming}); }
void AudioSystem::unload(AudioAssetId asset) { const auto index = audio_handle_index(asset); if (index >= assets_.size() || !assets_[index].active || assets_[index].id != asset) return; for (const auto handle : voiceHandles_) { if (handle == 0 || !backend_) continue; const auto voiceIndex = audio_handle_index(handle); if (voiceIndex < voiceAssets_.size() && voiceAssets_[voiceIndex] == asset) backend_->stop(handle, 0.0f); } assets_[index].active = false; assets_[index].id = 0; assets_[index].desc = {}; assets_[index].generation = (assets_[index].generation + 1u) & AudioHandleGenerationMask; if (assets_[index].generation == 0) assets_[index].generation = 1; freeAssets_.push_back(index); --assetCount_; }
bool AudioSystem::is_loaded(AudioAssetId asset) const noexcept { const auto index = audio_handle_index(asset); return index < assets_.size() && assets_[index].active && assets_[index].id == asset; }
AudioVoiceId AudioSystem::play(AudioAssetId asset, const AudioPlayParams& params) { if (!initialized_ || !backend_ || !is_loaded(asset)) { lastError_ = "audio asset is not loaded"; return 0; } const auto voice = backend_->play(assets_[audio_handle_index(asset)].desc, params); if (voice == 0) { lastError_ = backend_->last_error(); return 0; } const auto index = audio_handle_index(voice); if (index < voiceHandles_.size()) { voiceHandles_[index] = voice; voiceAssets_[index] = asset; } return voice; }
AudioVoiceId AudioSystem::play(std::filesystem::path path, const AudioPlayParams& params) { const auto asset = load(std::move(path), params.streaming); return asset == 0 ? 0 : play(asset, params); }
void AudioSystem::stop(AudioVoiceId voice, Seconds fadeOutSeconds) { if (backend_) backend_->stop(voice, std::max(fadeOutSeconds, 0.0f)); }
void AudioSystem::pause(AudioVoiceId voice) { if (backend_) backend_->pause(voice); }
void AudioSystem::resume(AudioVoiceId voice) { if (backend_) backend_->resume(voice); }
void AudioSystem::stop_all(AudioBus bus, Seconds fadeOutSeconds) { if (backend_) backend_->stop_all(bus, std::max(fadeOutSeconds, 0.0f)); }
AudioVoiceState AudioSystem::state(AudioVoiceId voice) const { return backend_ ? backend_->state(voice) : AudioVoiceState::Invalid; }
bool AudioSystem::is_playing(AudioVoiceId voice) const { return state(voice) == AudioVoiceState::Playing; }
void AudioSystem::set_volume(AudioVoiceId voice, float volume) { if (backend_) backend_->set_volume(voice, volume); }
void AudioSystem::set_pitch(AudioVoiceId voice, float pitch) { if (backend_) backend_->set_pitch(voice, pitch); }
void AudioSystem::set_pan(AudioVoiceId voice, float pan) { if (backend_) backend_->set_pan(voice, pan); }
void AudioSystem::set_spatial(AudioVoiceId voice, const AudioPlayParams& params) { if (backend_) backend_->set_spatial(voice, params); }
void AudioSystem::set_listener(const AudioListener& listener) { if (backend_) backend_->set_listener(listener); }
void AudioSystem::set_bus_volume(AudioBus bus, float volume) { if (backend_) backend_->set_bus_volume(bus, volume); }
void AudioSystem::set_bus_muted(AudioBus bus, bool muted) { if (backend_) backend_->set_bus_muted(bus, muted); }
AudioTrackId AudioSystem::create_track(AudioTrackDesc desc) { if (!backend_) return 0; const auto track = backend_->create_track(desc); if (track == 0) return 0; const auto index = audio_handle_index(track); if (index >= tracks_.size()) return 0; tracks_[index].name = std::move(desc.name); tracks_[index].snapshot = {track, tracks_[index].name, desc.bus, desc.volume, desc.muted, 0}; tracks_[index].active = true; ++trackCount_; return track; }
void AudioSystem::destroy_track(AudioTrackId track) { if (backend_) backend_->destroy_track(track); const auto index = audio_handle_index(track); if (index < tracks_.size() && tracks_[index].active && tracks_[index].snapshot.track == track) { tracks_[index].snapshot = {}; tracks_[index].name.clear(); tracks_[index].active = false; --trackCount_; } }
void AudioSystem::set_track_volume(AudioTrackId track, float volume) { if (backend_) backend_->set_track_volume(track, volume); }
void AudioSystem::set_track_muted(AudioTrackId track, bool muted) { if (backend_) backend_->set_track_muted(track, muted); }
AudioTrackSnapshot AudioSystem::track_snapshot(AudioTrackId track) const { return backend_ ? backend_->track_snapshot(track) : AudioTrackSnapshot{}; }
AudioBusSnapshot AudioSystem::bus_snapshot(AudioBus bus) const { AudioBusSnapshot result; result.bus = bus; if (backend_) result.activeVoices = backend_->diagnostics().activeVoices; return result; }

std::unique_ptr<IAudioBackend> create_audio_backend() {
#if defined(SHINKOU_WITH_MINIAUDIO)
    return std::make_unique<MiniaudioBackend>();
#else
    return std::make_unique<NullAudioBackend>();
#endif
}
}

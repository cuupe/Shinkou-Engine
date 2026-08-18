#pragma once

#include "shinkou/Math.h"
#include "shinkou/Types.h"
#include "shinkou/audio/AudioDSP.h"
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shinkou::audio {

using AudioAssetId = std::uint32_t;
using AudioVoiceId = std::uint32_t;
using AudioTrackId = std::uint32_t;

constexpr std::uint32_t AudioHandleIndexBits = 16;
constexpr std::uint32_t AudioHandleIndexMask = (1u << AudioHandleIndexBits) - 1u;
constexpr std::uint32_t AudioHandleGenerationMask = AudioHandleIndexMask;
constexpr std::uint32_t make_audio_handle(std::uint32_t index, std::uint32_t generation) noexcept {
    return ((generation & AudioHandleGenerationMask) << AudioHandleIndexBits) |
           ((index + 1u) & AudioHandleIndexMask);
}
constexpr std::uint32_t audio_handle_index(std::uint32_t handle) noexcept {
    const auto encoded = handle & AudioHandleIndexMask;
    return encoded == 0 ? AudioHandleIndexMask : encoded - 1u;
}
constexpr std::uint32_t audio_handle_generation(std::uint32_t handle) noexcept {
    return (handle >> AudioHandleIndexBits) & AudioHandleGenerationMask;
}

enum class AudioBus : std::uint8_t {
    Master = 0,
    Music,
    Sfx,
    Voice,
    Ambient,
    UI,
    Count
};

enum class AudioVoiceState : std::uint8_t {
    Invalid,
    Playing,
    Paused,
    Stopped,
    Finished
};

struct AudioConfig {
    std::filesystem::path assetRoot{};
    std::uint32_t maxVoices{128};
    bool startDevice{true};
    AudioFormat mixFormat{};
    std::uint32_t audioThreadBufferFrames{512};
    std::uint32_t outputChannels{2};
    std::uint32_t maxAssets{4096};
    std::uint32_t maxTracks{64};
};

struct AudioAssetDesc {
    std::filesystem::path path{};
    bool streaming{false};
    bool preload{false};
    AudioFormat preferredFormat{};
};

struct AudioPlayParams {
    AudioBus bus{AudioBus::Sfx};
    AudioTrackId track{0};
    bool loop{false};
    bool startPaused{false};
    bool spatialized{false};
    bool streaming{false};
    float volume{1.0f};
    float pitch{1.0f};
    float pan{0.0f};
    float fadeInSeconds{0.0f};
    math::Vec3 position{};
    math::Vec3 velocity{};
    float minDistance{1.0f};
    float maxDistance{100.0f};
    float rolloff{1.0f};
};

struct AudioBusSnapshot {
    AudioBus bus{AudioBus::Master};
    float volume{1.0f};
    bool muted{false};
    std::uint32_t activeVoices{0};
};

struct AudioTrackDesc {
    std::string name{};
    AudioBus bus{AudioBus::Sfx};
    float volume{1.0f};
    bool muted{false};
};

struct AudioTrackSnapshot {
    AudioTrackId track{0};
    std::string_view name{};
    AudioBus bus{AudioBus::Sfx};
    float volume{1.0f};
    bool muted{false};
    std::uint32_t activeVoices{0};
};

struct AudioDiagnostics {
    std::uint32_t activeVoices{0};
    std::uint32_t droppedVoices{0};
    std::uint64_t decodedFrames{0};
    std::uint64_t mixedFrames{0};
    std::uint64_t underruns{0};
    float mixCpuMilliseconds{0.0f};
};

struct AudioListener {
    math::Vec3 position{};
    math::Vec3 forward{0.0f, 0.0f, 1.0f};
    math::Vec3 up{0.0f, 1.0f, 0.0f};
    math::Vec3 velocity{};
};

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    virtual bool initialize(const AudioConfig& config) = 0;
    virtual void shutdown() = 0;
    virtual void update(Seconds dt) = 0;
    virtual AudioVoiceId play(const AudioAssetDesc& asset, const AudioPlayParams& params) = 0;
    virtual void stop(AudioVoiceId voice, Seconds fadeOutSeconds) = 0;
    virtual void pause(AudioVoiceId voice) = 0;
    virtual void resume(AudioVoiceId voice) = 0;
    virtual void set_volume(AudioVoiceId voice, float volume) = 0;
    virtual void set_pitch(AudioVoiceId voice, float pitch) = 0;
    virtual void set_pan(AudioVoiceId voice, float pan) = 0;
    virtual void set_spatial(AudioVoiceId voice, const AudioPlayParams& params) = 0;
    virtual void set_listener(const AudioListener& listener) = 0;
    virtual void set_bus_volume(AudioBus bus, float volume) = 0;
    virtual void set_bus_muted(AudioBus bus, bool muted) = 0;
    virtual AudioTrackId create_track(const AudioTrackDesc& desc) = 0;
    virtual void destroy_track(AudioTrackId track) = 0;
    virtual void set_track_volume(AudioTrackId track, float volume) = 0;
    virtual void set_track_muted(AudioTrackId track, bool muted) = 0;
    virtual AudioTrackSnapshot track_snapshot(AudioTrackId track) const = 0;
    virtual AudioVoiceState state(AudioVoiceId voice) const = 0;
    virtual std::uint32_t collect_finished(AudioVoiceId* output, std::uint32_t capacity) = 0;
    virtual void stop_all(AudioBus bus, Seconds fadeOutSeconds) = 0;
    virtual std::string last_error() const = 0;
    virtual AudioDiagnostics diagnostics() const = 0;
};

class AudioSystem final {
    struct AssetSlot {
        AudioAssetId id{0};
        AudioAssetDesc desc{};
        std::uint32_t generation{1};
        bool active{false};
    };
    std::unique_ptr<IAudioBackend> backend_;
    AudioConfig config_{};
    std::vector<AssetSlot> assets_;
    std::vector<std::uint32_t> freeAssets_;
    std::vector<AudioVoiceId> voiceHandles_;
    std::vector<AudioAssetId> voiceAssets_;
    struct TrackSlot {
        AudioTrackSnapshot snapshot{};
        std::string name{};
        bool active{false};
    };
    std::vector<TrackSlot> tracks_;
    std::vector<AudioVoiceId> finishedVoices_;
    std::uint32_t assetCount_{0};
    std::uint32_t trackCount_{0};
    bool initialized_{false};
    std::string lastError_;
    AudioDiagnostics diagnostics_{};

    std::filesystem::path resolve_path(const std::filesystem::path& path) const;
    AudioAssetId find_asset(const std::filesystem::path& path) const;
    void forget_finished_voices();

public:
    explicit AudioSystem(std::unique_ptr<IAudioBackend> backend, AudioConfig config = {});
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    AudioSystem(AudioSystem&&) noexcept = default;
    AudioSystem& operator=(AudioSystem&&) noexcept = default;

    bool initialize();
    void shutdown();
    void update(Seconds dt);

    AudioAssetId load(const AudioAssetDesc& asset);
    AudioAssetId load(std::filesystem::path path, bool streaming = false);
    void unload(AudioAssetId asset);
    bool is_loaded(AudioAssetId asset) const noexcept;
    std::size_t asset_count() const noexcept { return assetCount_; }

    AudioVoiceId play(AudioAssetId asset, const AudioPlayParams& params = {});
    AudioVoiceId play(std::filesystem::path path, const AudioPlayParams& params = {});
    void stop(AudioVoiceId voice, Seconds fadeOutSeconds = 0.0f);
    void pause(AudioVoiceId voice);
    void resume(AudioVoiceId voice);
    void stop_all(AudioBus bus = AudioBus::Master, Seconds fadeOutSeconds = 0.0f);
    AudioVoiceState state(AudioVoiceId voice) const;
    bool is_playing(AudioVoiceId voice) const;
    void set_volume(AudioVoiceId voice, float volume);
    void set_pitch(AudioVoiceId voice, float pitch);
    void set_pan(AudioVoiceId voice, float pan);
    void set_spatial(AudioVoiceId voice, const AudioPlayParams& params);

    void set_listener(const AudioListener& listener);
    void set_bus_volume(AudioBus bus, float volume);
    void set_bus_muted(AudioBus bus, bool muted);
    AudioTrackId create_track(AudioTrackDesc desc = {});
    void destroy_track(AudioTrackId track);
    void set_track_volume(AudioTrackId track, float volume);
    void set_track_muted(AudioTrackId track, bool muted);
    AudioTrackSnapshot track_snapshot(AudioTrackId track) const;
    AudioBusSnapshot bus_snapshot(AudioBus bus) const;
    AudioDiagnostics diagnostics() const noexcept { return diagnostics_; }

    bool initialized() const noexcept { return initialized_; }
    const std::string& last_error() const noexcept { return lastError_; }
    const AudioConfig& config() const noexcept { return config_; }
};

std::unique_ptr<IAudioBackend> create_audio_backend();
}

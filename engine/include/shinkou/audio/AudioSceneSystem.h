#pragma once

#include "shinkou/Types.h"
#include "shinkou/audio/AudioSystem.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace shinkou {
class World;
class GameObject;
namespace assets { class AssetSystem; }
namespace components { class AudioSourceComponent; }

namespace audio {

struct AudioSceneDiagnostics {
    std::size_t activeSources{0};
    std::size_t playingSources{0};
    std::size_t pendingSources{0};
    std::size_t invalidatedSources{0};
    std::size_t loadedClips{0};
    std::size_t failedSources{0};
    ObjectId listenerObject{0};
    bool listenerBound{false};
    std::string lastError{};
};

// Bridges serialized AudioSourceComponent intent to session-owned audio
// handles. World data never owns AudioAssetId/AudioVoiceId or backend state.
class AudioSceneSystem final {
    struct ClipBinding {
        AudioAssetId asset{0};
        std::size_t references{0};
    };

    struct SourceBinding {
        std::string path{};
        std::uint32_t bus{2};
        bool streaming{false};
        bool loop{false};
        bool spatialized{false};
        float volume{1.0f};
        float pitch{1.0f};
        float minDistance{1.0f};
        float maxDistance{100.0f};
        float rolloff{1.0f};
        std::uint64_t assetId{0};
        AudioAssetId asset{0};
        AudioVoiceId voice{0};
        bool started{false};
        bool pending{false};
        std::uint64_t manifestRevision{0};
        std::uint64_t lastSeenRevision{0};
    };

    std::unordered_map<std::string, ClipBinding> clips_;
    std::unordered_map<ObjectId, SourceBinding> sources_;
    std::uint64_t revision_{0};
    AudioSceneDiagnostics diagnostics_{};

    AudioAssetId acquire_clip(AudioSystem& audio, const std::string& path, bool streaming);
    void release_clip(AudioSystem& audio, const std::string& path, AudioAssetId asset);
    void stop_source(AudioSystem& audio, SourceBinding& binding);
    void sync_listener(World& world, AudioSystem& audio);
    bool start_source(GameObject& object, components::AudioSourceComponent& source,
                      SourceBinding& binding, AudioSystem& audio,
                      const assets::AssetSystem* assetSystem);

public:
    AudioSceneSystem() = default;
    AudioSceneSystem(const AudioSceneSystem&) = delete;
    AudioSceneSystem& operator=(const AudioSceneSystem&) = delete;

    void sync(World& world, AudioSystem& audio, const assets::AssetSystem* assetSystem = nullptr);
    bool play(World& world, ObjectId object, AudioSystem& audio,
              const assets::AssetSystem* assetSystem = nullptr);
    bool pause(ObjectId object, AudioSystem& audio);
    bool resume(ObjectId object, AudioSystem& audio);
    void stop(ObjectId object, AudioSystem& audio);
    void shutdown(AudioSystem& audio);

    // Presentation-facing transport state. Runtime voice/asset handles stay
    // private to this bridge; the editor only receives a bounded semantic
    // state string for the selected source.
    std::string transport_state(ObjectId object, const AudioSystem& audio) const;
    bool supports_cursor(ObjectId object, const AudioSystem& audio) const;
    double cursor_seconds(ObjectId object, const AudioSystem& audio) const;
    bool seek(ObjectId object, double seconds, AudioSystem& audio);

    const AudioSceneDiagnostics& diagnostics() const noexcept { return diagnostics_; }
    std::size_t source_count() const noexcept { return sources_.size(); }
    std::size_t clip_count() const noexcept { return clips_.size(); }
};

} // namespace audio
} // namespace shinkou

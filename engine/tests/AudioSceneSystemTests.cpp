#include "shinkou/World.h"
#include "shinkou/audio/AudioSceneSystem.h"
#include "shinkou/editor/EditorDocument.h"

#include <cassert>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>

namespace {
class RecordingBackend final : public shinkou::audio::IAudioBackend {
    std::unordered_map<shinkou::audio::AudioVoiceId, shinkou::audio::AudioVoiceState> voices_;
    shinkou::audio::AudioVoiceId nextVoice_{1};

public:
    void finish_all() {
        for (auto& [voice, state] : voices_) { (void)voice; state = shinkou::audio::AudioVoiceState::Finished; }
    }

    bool initialize(const shinkou::audio::AudioConfig&) override { return true; }
    void shutdown() override { voices_.clear(); }
    void update(shinkou::Seconds) override {}
    shinkou::audio::AudioVoiceId play(const shinkou::audio::AudioAssetDesc&,
                                      const shinkou::audio::AudioPlayParams& params) override {
        const auto voice = nextVoice_++;
        voices_[voice] = params.startPaused ? shinkou::audio::AudioVoiceState::Paused
                                             : shinkou::audio::AudioVoiceState::Playing;
        return voice;
    }
    void stop(shinkou::audio::AudioVoiceId voice, shinkou::Seconds) override {
        if (voices_.find(voice) != voices_.end()) voices_[voice] = shinkou::audio::AudioVoiceState::Stopped;
    }
    void pause(shinkou::audio::AudioVoiceId voice) override {
        if (voices_.find(voice) != voices_.end()) voices_[voice] = shinkou::audio::AudioVoiceState::Paused;
    }
    void resume(shinkou::audio::AudioVoiceId voice) override {
        if (voices_.find(voice) != voices_.end()) voices_[voice] = shinkou::audio::AudioVoiceState::Playing;
    }
    void set_volume(shinkou::audio::AudioVoiceId, float) override {}
    void set_pitch(shinkou::audio::AudioVoiceId, float) override {}
    void set_pan(shinkou::audio::AudioVoiceId, float) override {}
    void set_spatial(shinkou::audio::AudioVoiceId, const shinkou::audio::AudioPlayParams&) override {}
    void set_listener(const shinkou::audio::AudioListener&) override {}
    void set_bus_volume(shinkou::audio::AudioBus, float) override {}
    void set_bus_muted(shinkou::audio::AudioBus, bool) override {}
    shinkou::audio::AudioTrackId create_track(const shinkou::audio::AudioTrackDesc&) override { return 0; }
    void destroy_track(shinkou::audio::AudioTrackId) override {}
    void set_track_volume(shinkou::audio::AudioTrackId, float) override {}
    void set_track_muted(shinkou::audio::AudioTrackId, bool) override {}
    shinkou::audio::AudioTrackSnapshot track_snapshot(shinkou::audio::AudioTrackId) const override { return {}; }
    shinkou::audio::AudioVoiceState state(shinkou::audio::AudioVoiceId voice) const override {
        const auto found = voices_.find(voice);
        return found == voices_.end() ? shinkou::audio::AudioVoiceState::Invalid : found->second;
    }
    std::uint32_t collect_finished(shinkou::audio::AudioVoiceId* output, std::uint32_t capacity) override {
        std::uint32_t count = 0;
        for (auto it = voices_.begin(); it != voices_.end();) {
            if (it->second != shinkou::audio::AudioVoiceState::Stopped &&
                it->second != shinkou::audio::AudioVoiceState::Finished) {
                ++it;
                continue;
            }
            if (output && count < capacity) output[count++] = it->first;
            it = voices_.erase(it);
        }
        return count;
    }
    void stop_all(shinkou::audio::AudioBus, shinkou::Seconds) override {
        for (auto& [voice, state] : voices_) { (void)voice; state = shinkou::audio::AudioVoiceState::Stopped; }
    }
    std::string last_error() const override { return {}; }
    shinkou::audio::AudioDiagnostics diagnostics() const override {
        shinkou::audio::AudioDiagnostics result;
        for (const auto& [voice, state] : voices_) {
            (void)voice;
            if (state == shinkou::audio::AudioVoiceState::Playing) ++result.activeVoices;
        }
        return result;
    }
};
}

int main() {
    using namespace shinkou;
    using namespace shinkou::audio;

    World world;
    auto& first = world.create_object("Music");
    auto* firstSource = first.add_component<components::AudioSourceComponent>();
    assert(firstSource);
    firstSource->clipPath = "audio/loop.wav";
    firstSource->assetId = 42;
    firstSource->playOnStart = true;
    firstSource->loop = true;
    firstSource->spatialized = true;
    firstSource->volume = 0.5f;
    first.get_component<components::TransformComponent>()->set_position({3.0f, 2.0f, 1.0f});

    audio::AudioConfig config;
    config.assetRoot = "C:/shinkou-test-project";
    config.maxAssets = 8;
    config.maxVoices = 8;
    auto backend = std::make_unique<RecordingBackend>();
    auto* backendSpy = backend.get();
    AudioSystem audio(std::move(backend), config);
    assert(audio.initialize());
    AudioSceneSystem scene;

    world.update(0.0f);
    scene.sync(world, audio);
    assert(scene.source_count() == 1 && scene.clip_count() == 1);
    assert(scene.diagnostics().activeSources == 1 && scene.diagnostics().playingSources == 1);
    assert(audio.asset_count() == 1);

    scene.sync(world, audio);
    assert(scene.source_count() == 1 && scene.clip_count() == 1 && audio.asset_count() == 1);

    auto& second = world.create_object("MusicCopy");
    auto* secondSource = second.add_component<components::AudioSourceComponent>();
    assert(secondSource);
    secondSource->clipPath = firstSource->clipPath;
    secondSource->playOnStart = true;
    world.update(0.0f);
    scene.sync(world, audio);
    assert(scene.source_count() == 2 && scene.clip_count() == 1 && audio.asset_count() == 1);
    assert(scene.diagnostics().playingSources == 2);

    firstSource->set_enabled(false);
    scene.sync(world, audio);
    assert(scene.source_count() == 2 && scene.clip_count() == 1 && audio.asset_count() == 1);
    firstSource->set_enabled(true);
    firstSource->clipPath = "audio/other.wav";
    scene.sync(world, audio);
    assert(scene.clip_count() == 2 && audio.asset_count() == 2);

    editor::EditorDocument document;
    std::string error;
    assert(editor::EditorDocument::capture(world, first.id(), document, error));
    std::string json;
    assert(document.to_json(json, error));
    assert(json.find("AudioSource") != std::string::npos && json.find("assetId") != std::string::npos &&
           json.find("audio/other.wav") != std::string::npos);

    backendSpy->finish_all();
    audio.update(0.0f);
    scene.sync(world, audio);
    assert(scene.source_count() == 2 && scene.clip_count() == 0 && audio.asset_count() == 0);

    assert(scene.play(world, first.id(), audio));
    assert(scene.clip_count() == 1 && audio.asset_count() == 1);
    scene.stop(first.id(), audio);
    assert(scene.clip_count() == 0 && audio.asset_count() == 0);
    world.destroy_object(second);
    world.update(0.0f);
    scene.sync(world, audio);
    assert(scene.source_count() == 1 && scene.clip_count() == 0 && audio.asset_count() == 0);

    assert(scene.play(world, first.id(), audio));
    assert(scene.clip_count() == 1 && audio.asset_count() == 1);
    scene.shutdown(audio);
    assert(scene.source_count() == 0 && scene.clip_count() == 0 && audio.asset_count() == 0);
    audio.shutdown();
    return 0;
}

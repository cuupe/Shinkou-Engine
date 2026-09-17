#include "shinkou/World.h"
#include "shinkou/assets/AssetSystem.h"
#include "shinkou/audio/AudioSceneSystem.h"
#include "shinkou/editor/EditorDocument.h"
#include "shinkou/render/RenderScene.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>

namespace {
class RecordingBackend final : public shinkou::audio::IAudioBackend {
    std::unordered_map<shinkou::audio::AudioVoiceId, shinkou::audio::AudioVoiceState> voices_;
    shinkou::audio::AudioVoiceId nextVoice_{1};
    std::size_t stopCalls_{0};
    shinkou::audio::AudioPlayParams lastPlayParams_{};
    shinkou::audio::AudioPlayParams lastSpatialParams_{};
    shinkou::audio::AudioListener lastListener_{};
    std::unordered_map<shinkou::audio::AudioVoiceId, double> cursors_;
    std::size_t spatialCalls_{0};
    std::size_t listenerCalls_{0};

public:
    void finish_all() {
        for (auto& [voice, state] : voices_) { (void)voice; state = shinkou::audio::AudioVoiceState::Finished; }
    }
    std::size_t stop_calls() const noexcept { return stopCalls_; }
    const shinkou::audio::AudioPlayParams& last_play_params() const noexcept { return lastPlayParams_; }
    const shinkou::audio::AudioPlayParams& last_spatial_params() const noexcept { return lastSpatialParams_; }
    const shinkou::audio::AudioListener& last_listener() const noexcept { return lastListener_; }
    std::size_t spatial_calls() const noexcept { return spatialCalls_; }
    std::size_t listener_calls() const noexcept { return listenerCalls_; }

    bool initialize(const shinkou::audio::AudioConfig&) override { return true; }
    void shutdown() override { voices_.clear(); cursors_.clear(); }
    void update(shinkou::Seconds) override {}
    shinkou::audio::AudioAssetInfo inspect_asset(const shinkou::audio::AudioAssetDesc& asset) const override {
        return {asset.streaming, true, 30.0, true};
    }
    shinkou::audio::AudioVoiceId play(const shinkou::audio::AudioAssetDesc&,
                                      const shinkou::audio::AudioPlayParams& params) override {
        lastPlayParams_ = params;
        const auto voice = nextVoice_++;
        voices_[voice] = params.startPaused ? shinkou::audio::AudioVoiceState::Paused
                                             : shinkou::audio::AudioVoiceState::Playing;
        cursors_[voice] = 0.0;
        return voice;
    }
    void stop(shinkou::audio::AudioVoiceId voice, shinkou::Seconds) override {
        ++stopCalls_;
        if (voices_.find(voice) != voices_.end()) voices_[voice] = shinkou::audio::AudioVoiceState::Stopped;
    }
    void pause(shinkou::audio::AudioVoiceId voice) override {
        if (voices_.find(voice) != voices_.end()) voices_[voice] = shinkou::audio::AudioVoiceState::Paused;
    }
    void resume(shinkou::audio::AudioVoiceId voice) override {
        if (voices_.find(voice) != voices_.end()) voices_[voice] = shinkou::audio::AudioVoiceState::Playing;
    }
    void seek(shinkou::audio::AudioVoiceId voice, double seconds) override {
        if (voices_.find(voice) != voices_.end()) cursors_[voice] = std::max(0.0, seconds);
    }
    double cursor_seconds(shinkou::audio::AudioVoiceId voice) const override {
        const auto found = cursors_.find(voice);
        return found == cursors_.end() ? 0.0 : found->second;
    }
    bool supports_cursor() const noexcept override { return true; }
    void set_volume(shinkou::audio::AudioVoiceId, float) override {}
    void set_pitch(shinkou::audio::AudioVoiceId, float) override {}
    void set_pan(shinkou::audio::AudioVoiceId, float) override {}
    void set_spatial(shinkou::audio::AudioVoiceId, const shinkou::audio::AudioPlayParams& params) override {
        lastSpatialParams_ = params;
        ++spatialCalls_;
    }
    void set_listener(const shinkou::audio::AudioListener& listener) override {
        lastListener_ = listener;
        ++listenerCalls_;
    }
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

    const auto project = std::filesystem::temp_directory_path() /
        ("shinkou-audio-scene-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(project / "audio");
    std::ofstream(project / "audio/loop.wav", std::ios::binary) << "loop";
    std::ofstream(project / "audio/other.wav", std::ios::binary) << "other";
    assets::AssetSystemConfig assetConfig;
    assetConfig.projectRoot = project;
    assetConfig.cacheRoot = project / ".shinkou" / "cache";
    assetConfig.workerCount = 1;
    assetConfig.enableFileWatching = false;
    assetConfig.enableBackgroundWatcher = false;
    assetConfig.enableDiskCache = false;
    assets::AssetSystem assetSystem(assetConfig);
    assert(assetSystem.initialize());
    const auto manifest = assetSystem.scan_sources();
    assert(assetSystem.manifest_ready());
    const auto manifest_id_for = [&](const char* name) {
        const auto found = std::find_if(manifest.begin(), manifest.end(), [&](const auto& entry) {
            return entry.key.type == "audio" && entry.sourcePath.filename() == name;
        });
        return found == manifest.end() ? assets::AssetId{0} : found->id;
    };
    const auto loopAssetId = manifest_id_for("loop.wav");
    const auto otherAssetId = manifest_id_for("other.wav");
    assert(loopAssetId != 0 && otherAssetId != 0 && loopAssetId != otherAssetId);

    World world;
    auto& first = world.create_object("Music");
    auto* firstSource = first.add_component<components::AudioSourceComponent>();
    assert(firstSource);
    firstSource->clipPath = "audio/loop.wav";
    firstSource->assetId = loopAssetId;
    firstSource->playOnStart = true;
    firstSource->loop = true;
    firstSource->spatialized = true;
    firstSource->volume = 0.5f;
    firstSource->minDistance = 2.0f;
    firstSource->maxDistance = 45.0f;
    firstSource->rolloff = 0.6f;
    first.get_component<components::TransformComponent>()->set_position({3.0f, 2.0f, 1.0f});

    const auto camera = world.ecs().create();
    render::CameraComponent cameraComponent;
    cameraComponent.active = true;
    world.ecs().emplace<render::CameraComponent>(camera, cameraComponent);
    render::TransformComponent listenerTransform;
    listenerTransform.local.position = {10.0f, 4.0f, -2.0f};
    listenerTransform.local.rotation = math::FromAxisAngle({0.0f, 1.0f, 0.0f}, math::Pi * 0.5f);
    world.ecs().emplace<render::TransformComponent>(camera, listenerTransform);

    audio::AudioConfig config;
    config.assetRoot = project;
    config.maxAssets = 8;
    config.maxVoices = 8;
    auto backend = std::make_unique<RecordingBackend>();
    auto* backendSpy = backend.get();
    AudioSystem audio(std::move(backend), config);
    assert(audio.initialize());

    assets::AssetSystem pendingAssetSystem(assetConfig);
    assert(pendingAssetSystem.initialize());
    World pendingWorld;
    auto& pendingObject = pendingWorld.create_object("PendingMusic");
    auto* pendingSource = pendingObject.add_component<components::AudioSourceComponent>();
    assert(pendingSource);
    pendingSource->clipPath = "audio/loop.wav";
    pendingSource->assetId = loopAssetId;
    pendingSource->playOnStart = true;
    AudioSceneSystem pendingScene;
    pendingScene.sync(pendingWorld, audio, &pendingAssetSystem);
    assert(pendingScene.clip_count() == 0 && pendingScene.diagnostics().playingSources == 0 &&
           pendingScene.diagnostics().pendingSources == 1);
    pendingAssetSystem.scan_sources();
    pendingScene.sync(pendingWorld, audio, &pendingAssetSystem);
    assert(pendingScene.clip_count() == 1 && pendingScene.diagnostics().playingSources == 1);
    pendingScene.shutdown(audio);
    pendingAssetSystem.shutdown();

    AudioSceneSystem scene;

    world.update(0.0f);
    scene.sync(world, audio, &assetSystem);
    assert(scene.source_count() == 1 && scene.clip_count() == 1);
    assert(scene.diagnostics().activeSources == 1 && scene.diagnostics().playingSources == 1);
    assert(audio.asset_count() == 1);
    assert(scene.diagnostics().listenerBound && scene.diagnostics().listenerObject == 0);
    assert(backendSpy->listener_calls() > 0);
    assert(std::abs(backendSpy->last_listener().position.x - 10.0f) < 0.001f &&
           std::abs(backendSpy->last_listener().position.y - 4.0f) < 0.001f &&
           std::abs(backendSpy->last_listener().position.z + 2.0f) < 0.001f);
    assert(std::abs(backendSpy->last_listener().forward.x - 1.0f) < 0.001f &&
           std::abs(backendSpy->last_listener().forward.z) < 0.001f);
    assert(std::abs(backendSpy->last_play_params().position.x - 3.0f) < 0.001f &&
           std::abs(backendSpy->last_play_params().minDistance - 2.0f) < 0.001f &&
           std::abs(backendSpy->last_play_params().maxDistance - 45.0f) < 0.001f &&
           std::abs(backendSpy->last_play_params().rolloff - 0.6f) < 0.001f);
    assert(backendSpy->spatial_calls() > 0);

    firstSource->minDistance = 80.0f;
    firstSource->maxDistance = 20.0f;
    firstSource->rolloff = std::numeric_limits<float>::quiet_NaN();
    scene.sync(world, audio, &assetSystem);
    assert(backendSpy->last_play_params().minDistance == 80.0f &&
           backendSpy->last_play_params().maxDistance == 80.0f &&
           backendSpy->last_play_params().rolloff == 1.0f);

    cameraComponent.active = false;
    world.ecs().emplace_or_replace<render::CameraComponent>(camera, cameraComponent);
    scene.sync(world, audio, &assetSystem);
    assert(!scene.diagnostics().listenerBound && scene.diagnostics().listenerObject == 0);
    assert(std::abs(backendSpy->last_listener().forward.z - 1.0f) < 0.001f &&
           std::abs(backendSpy->last_listener().up.y - 1.0f) < 0.001f);
    cameraComponent.active = true;
    world.ecs().emplace_or_replace<render::CameraComponent>(camera, cameraComponent);

    scene.sync(world, audio, &assetSystem);
    assert(scene.source_count() == 1 && scene.clip_count() == 1 && audio.asset_count() == 1);

    auto& second = world.create_object("MusicCopy");
    auto* secondSource = second.add_component<components::AudioSourceComponent>();
    assert(secondSource);
    secondSource->clipPath = firstSource->clipPath;
    secondSource->playOnStart = true;
    world.update(0.0f);
    scene.sync(world, audio, &assetSystem);
    assert(scene.source_count() == 2 && scene.clip_count() == 1 && audio.asset_count() == 1);
    assert(scene.diagnostics().playingSources == 2);

    firstSource->set_enabled(false);
    scene.sync(world, audio, &assetSystem);
    assert(scene.source_count() == 2 && scene.clip_count() == 1 && audio.asset_count() == 1);
    firstSource->set_enabled(true);
    scene.sync(world, audio, &assetSystem);
    assert(scene.clip_count() == 1 && audio.asset_count() == 1 && scene.diagnostics().playingSources == 2);
    firstSource->clipPath = "audio/other.wav";
    firstSource->assetId = otherAssetId;
    scene.sync(world, audio, &assetSystem);
    assert(scene.clip_count() == 2 && audio.asset_count() == 2);

    const auto stopsBeforeManifestRefresh = backendSpy->stop_calls();
    const auto revisionBeforeManifestRefresh = assetSystem.manifest_revision();
    assetSystem.scan_sources();
    assert(assetSystem.manifest_revision() != revisionBeforeManifestRefresh);
    scene.sync(world, audio, &assetSystem);
    assert(scene.diagnostics().invalidatedSources == 1 &&
           backendSpy->stop_calls() >= stopsBeforeManifestRefresh + 1 &&
           scene.diagnostics().playingSources == 2);

    std::error_code removeError;
    std::filesystem::remove(project / "audio/other.wav", removeError);
    assert(!removeError);
    assetSystem.scan_sources();
    scene.sync(world, audio, &assetSystem);
    assert(scene.diagnostics().invalidatedSources == 1 && scene.diagnostics().failedSources == 1 &&
           scene.diagnostics().playingSources == 1 && scene.clip_count() == 1);
    std::ofstream(project / "audio/other.wav", std::ios::binary | std::ios::trunc) << "other-restored";
    assetSystem.scan_sources();
    scene.sync(world, audio, &assetSystem);
    assert(scene.diagnostics().invalidatedSources == 1 && scene.diagnostics().playingSources == 2 &&
           scene.clip_count() == 2);

    editor::EditorDocument document;
    std::string error;
    assert(editor::EditorDocument::capture(world, first.id(), document, error));
    std::string json;
    assert(document.to_json(json, error));
    assert(json.find("AudioSource") != std::string::npos && json.find("assetId") != std::string::npos &&
           json.find("audio/other.wav") != std::string::npos);

    backendSpy->finish_all();
    audio.update(0.0f);
    scene.sync(world, audio, &assetSystem);
    assert(scene.source_count() == 2 && scene.clip_count() == 0 && audio.asset_count() == 0);

    assert(scene.play(world, first.id(), audio, &assetSystem));
    assert(scene.clip_count() == 1 && audio.asset_count() == 1);
    assert(scene.transport_state(first.id(), audio) == "Playing");
    assert(scene.supports_cursor(first.id(), audio));
    assert(scene.has_duration(first.id()) && std::abs(scene.duration_seconds(first.id()) - 30.0) < 0.001);
    assert(scene.cursor_seconds(first.id(), audio) == 0.0);
    assert(scene.seek(first.id(), 12.5, audio));
    assert(std::abs(scene.cursor_seconds(first.id(), audio) - 12.5) < 0.001);
    assert(scene.seek(first.id(), 60.0, audio));
    assert(std::abs(scene.cursor_seconds(first.id(), audio) - 30.0) < 0.001);
    assert(!scene.seek(first.id(), -1.0, audio));
    assert(scene.pause(first.id(), audio));
    assert(scene.transport_state(first.id(), audio) == "Paused");
    assert(scene.resume(first.id(), audio));
    assert(scene.transport_state(first.id(), audio) == "Playing");
    scene.stop(first.id(), audio);
    assert(scene.transport_state(first.id(), audio) == "Stopped");
    assert(scene.clip_count() == 0 && audio.asset_count() == 0);
    firstSource->assetId = loopAssetId;
    assert(!scene.play(world, first.id(), audio, &assetSystem));
    assert(scene.clip_count() == 0 && scene.diagnostics().lastError.find("does not match") != std::string::npos);
    firstSource->assetId = otherAssetId;
    assert(scene.play(world, first.id(), audio, &assetSystem));
    scene.stop(first.id(), audio);
    world.destroy_object(second);
    world.update(0.0f);
    scene.sync(world, audio, &assetSystem);
    assert(scene.source_count() == 1 && scene.clip_count() == 0 && audio.asset_count() == 0);

    assert(scene.play(world, first.id(), audio, &assetSystem));
    assert(scene.clip_count() == 1 && audio.asset_count() == 1);
    scene.shutdown(audio);
    assert(scene.source_count() == 0 && scene.clip_count() == 0 && audio.asset_count() == 0);
    audio.shutdown();
    assetSystem.shutdown();
    std::error_code cleanupError;
    std::filesystem::remove_all(project, cleanupError);
    return 0;
}

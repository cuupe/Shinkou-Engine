#include "shinkou/World.h"
#include "shinkou/audio/AudioSystem.h"
#include "shinkou/editor/EditorAudioPreview.h"
#include "shinkou/editor/EditorLayer.h"
#include "shinkou/render/Renderer.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

void append_u16(std::vector<unsigned char>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xffu));
    bytes.push_back(static_cast<unsigned char>((value >> 8u) & 0xffu));
}

void append_u32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffu));
}

std::vector<unsigned char> make_wav() {
    constexpr std::uint32_t sampleRate = 8000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    constexpr std::uint32_t sampleCount = 8;
    constexpr std::uint32_t dataBytes = sampleCount * channels * bits / 8u;
    std::vector<unsigned char> bytes;
    bytes.reserve(44u + dataBytes);
    const auto chunk = [&bytes](const char* text) {
        for (int index = 0; index < 4; ++index) bytes.push_back(static_cast<unsigned char>(text[index]));
    };
    chunk("RIFF"); append_u32(bytes, 36u + dataBytes); chunk("WAVE");
    chunk("fmt "); append_u32(bytes, 16u); append_u16(bytes, 1u); append_u16(bytes, channels);
    append_u32(bytes, sampleRate); append_u32(bytes, sampleRate * channels * bits / 8u);
    append_u16(bytes, channels * bits / 8u); append_u16(bytes, bits);
    chunk("data"); append_u32(bytes, dataBytes);
    constexpr std::int16_t samples[] = {0, 32767, -32768, 0, 16384, -16384, 0, 0};
    for (const auto sample : samples) append_u16(bytes, static_cast<std::uint16_t>(sample));
    return bytes;
}

class PreviewBackend final : public shinkou::audio::IAudioBackend {
    shinkou::audio::AudioVoiceId voice_{0};
    shinkou::audio::AudioVoiceState state_{shinkou::audio::AudioVoiceState::Invalid};
    double cursorSeconds_{0.0};

public:
    bool initialize(const shinkou::audio::AudioConfig&) override { return true; }
    void shutdown() override { voice_ = 0; state_ = shinkou::audio::AudioVoiceState::Invalid; cursorSeconds_ = 0.0; }
    void update(shinkou::Seconds) override {}
    shinkou::audio::AudioVoiceId play(const shinkou::audio::AudioAssetDesc&,
                                      const shinkou::audio::AudioPlayParams& params) override {
        voice_ = shinkou::audio::make_audio_handle(0, 1);
        state_ = params.startPaused ? shinkou::audio::AudioVoiceState::Paused :
            shinkou::audio::AudioVoiceState::Playing;
        cursorSeconds_ = 0.0;
        return voice_;
    }
    void stop(shinkou::audio::AudioVoiceId voice, shinkou::Seconds) override {
        if (voice == voice_) state_ = shinkou::audio::AudioVoiceState::Stopped;
    }
    void pause(shinkou::audio::AudioVoiceId voice) override {
        if (voice == voice_) state_ = shinkou::audio::AudioVoiceState::Paused;
    }
    void resume(shinkou::audio::AudioVoiceId voice) override {
        if (voice == voice_) state_ = shinkou::audio::AudioVoiceState::Playing;
    }
    void seek(shinkou::audio::AudioVoiceId voice, double seconds) override {
        if (voice == voice_) cursorSeconds_ = std::max(0.0, seconds);
    }
    double cursor_seconds(shinkou::audio::AudioVoiceId voice) const override {
        return voice == voice_ ? cursorSeconds_ : 0.0;
    }
    bool supports_cursor() const noexcept override { return true; }
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
        return voice == voice_ ? state_ : shinkou::audio::AudioVoiceState::Invalid;
    }
    std::uint32_t collect_finished(shinkou::audio::AudioVoiceId*, std::uint32_t) override { return 0; }
    void stop_all(shinkou::audio::AudioBus, shinkou::Seconds) override {
        state_ = shinkou::audio::AudioVoiceState::Stopped;
    }
    std::string last_error() const override { return {}; }
    shinkou::audio::AudioDiagnostics diagnostics() const override { return {}; }
};

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("shinkou-editor-audio-preview-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code cleanup;
    std::filesystem::create_directories(root / "assets");
    const auto wav = make_wav();
    {
        std::ofstream file(root / "assets/preview.wav", std::ios::binary);
        file.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
    }
    std::ofstream(root / "assets/broken.wav", std::ios::binary) << "not audio";

    shinkou::editor::FileSystemService files(root);
    const auto result = shinkou::editor::load_editor_audio_preview(
        files, "assets/preview.wav", 3, 9, 32);
    assert(result.generation == 3 && result.sourceStamp == 9);
    assert(result.path == "assets/preview.wav");
    if (result.snapshot) {
        assert(result.error.empty());
        assert(result.snapshot->valid());
        assert(result.snapshot->channels == 1);
        assert(result.snapshot->sampleRate == 8000);
        assert(result.snapshot->totalFrames == 8);
        assert(result.snapshot->peaks.size() == 32);
        assert(std::any_of(result.snapshot->peaks.begin(), result.snapshot->peaks.end(),
            [](float value) { return value > 0.9f; }));
        assert(std::abs(result.snapshot->duration - 0.001) < 0.0001);
    } else {
        assert(result.error.find("unavailable") != std::string::npos);
    }
    const auto broken = shinkou::editor::load_editor_audio_preview(
        files, "assets/broken.wav", 4, 10);
    assert(!broken.snapshot && !broken.error.empty());
    const auto outside = shinkou::editor::load_editor_audio_preview(
        files, "../outside.wav", 5, 11);
    assert(!outside.snapshot && !outside.error.empty());
    std::atomic_bool cancelled{true};
    const auto cancelledResult = shinkou::editor::load_editor_audio_preview(
        files, "assets/preview.wav", 6, 12, 32, &cancelled);
    assert(!cancelledResult.snapshot && cancelledResult.error.find("cancelled") != std::string::npos);

    shinkou::audio::AudioConfig audioConfig;
    audioConfig.maxAssets = 4;
    shinkou::audio::AudioSystem audio(std::make_unique<PreviewBackend>(), audioConfig);
    assert(audio.initialize());
    shinkou::editor::EditorLayer editor;
    editor.set_project_root(root.generic_string());
    editor.set_layout_path((root / "Saved/Editor/layout.json").generic_string());
    editor.set_audio_system(&audio);
    assert(editor.initialize(false));
    editor.set_display_size(1280.0f, 720.0f, 1.0f);
    shinkou::World world;
    shinkou::render::Renderer renderer;
    editor.execute_command(shinkou::editor::EditorCommand::OpenAsset, "assets/preview.wav", world);
    for (int index = 0; index < 120 && editor.media_preview().kind != "Audio"; ++index) {
        editor.draw(renderer, world, 1.0f / 60.0f, static_cast<shinkou::FrameIndex>(index));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(editor.media_preview().kind == "Audio");
    assert(editor.media_preview().available);
    if (result.snapshot) {
        for (int index = 0; index < 120 && !editor.media_preview().audioPreview; ++index) {
            editor.draw(renderer, world, 1.0f / 60.0f, static_cast<shinkou::FrameIndex>(index + 120));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        assert(editor.media_preview().audioPreview && editor.media_preview().audioPreview->valid());
        editor.execute_command(shinkou::editor::EditorCommand::MediaSeek, "0.5", world);
        editor.draw(renderer, world, 1.0f / 60.0f, 240);
        assert(std::abs(editor.media_preview().currentTime -
                        editor.media_preview().duration * 0.5) < 0.00001);
    }
    editor.execute_command(shinkou::editor::EditorCommand::MediaPlay, {}, world);
    editor.draw(renderer, world, 1.0f / 60.0f, 241);
    assert(editor.media_preview().playbackState == "playing");
    editor.execute_command(shinkou::editor::EditorCommand::MediaSeek, "0.5", world);
    editor.draw(renderer, world, 1.0f / 60.0f, 2411);
    assert(std::abs(editor.media_preview().currentTime - editor.media_preview().duration * 0.5) < 0.00001);
    editor.execute_command(shinkou::editor::EditorCommand::MediaPause, {}, world);
    editor.draw(renderer, world, 1.0f / 60.0f, 242);
    assert(editor.media_preview().playbackState == "paused");
    editor.execute_command(shinkou::editor::EditorCommand::MediaStop, {}, world);
    editor.draw(renderer, world, 1.0f / 60.0f, 243);
    assert(editor.media_preview().playbackState == "stopped");
    assert(audio.asset_count() == 0);

    editor.shutdown();
    audio.shutdown();
    std::filesystem::remove_all(root, cleanup);
    std::cout << "Editor audio preview provider and transport passed\n";
    return 0;
}

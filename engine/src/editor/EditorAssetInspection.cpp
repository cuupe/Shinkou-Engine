#include "shinkou/GameObject.h"
#include "shinkou/World.h"
#include "shinkou/editor/EditorLayer.h"
#include <cmath>

namespace shinkou::editor {
void EditorLayer::load_asset_settings() {
    const auto root = fileSystem_.root().generic_u8string();
    if (assetSettingsPath_ == assetBrowser_.selection && assetSettingsRoot_ == root)
        return;
    assetSettingsPath_ = assetBrowser_.selection;
    assetSettingsRoot_ = root;
    assetSettings_ = {};
    assetSettingsLoadFailed_ = false;
    assetSettingsStatus_.clear();
    if (assetBrowser_.selection.empty())
        return;
    std::string error;
    if (!EditorAssetSettingsStore::load(fileSystem_, assetBrowser_.selection, assetSettings_,
                                        error)) {
        assetSettingsLoadFailed_ = true;
        assetSettingsStatus_ = "Settings load failed: " + error;
    } else
        assetSettingsStatus_ = "Asset settings loaded";
}
bool EditorLayer::set_asset_settings(EditorAssetSettings settings, bool persist) {
    load_asset_settings();
    if (assetBrowser_.selection.empty() || !settings.valid()) {
        assetSettingsStatus_ = "Invalid asset settings";
        return false;
    }
    if (persist) {
        std::string error;
        if (!EditorAssetSettingsStore::save(fileSystem_, assetBrowser_.selection, settings,
                                            error)) {
            assetSettingsStatus_ = "Settings save failed: " + error;
            return false;
        }
        assetSettingsLoadFailed_ = false;
    }
    const bool restart =
        settings.bus != assetSettings_.bus || settings.streaming != assetSettings_.streaming;
    const bool effectsChanged = settings.lowPass != assetSettings_.lowPass ||
                                settings.reverb != assetSettings_.reverb ||
                                settings.cutoffHz != assetSettings_.cutoffHz ||
                                settings.reverbWet != assetSettings_.reverbWet;
    assetSettings_ = settings;
    mediaPanel_.playback().loop = settings.loop;
    mediaPanel_.playback().volume = settings.volume;
    apply_asset_audio_settings(restart, effectsChanged);
    assetSettingsStatus_ =
        persist ? "Asset settings saved" : "Asset settings changed; Save to keep";
    sync_media_preview_state();
    return true;
}
bool EditorLayer::save_asset_settings() {
    return set_asset_settings(assetSettings_, true);
}
void EditorLayer::apply_asset_audio_settings(bool restart, bool effectsChanged) {
    if (!audioSystem_ || !audioPreviewVoice_)
        return;
    if (restart) {
        const bool paused =
            audioSystem_->state(audioPreviewVoice_) == audio::AudioVoiceState::Paused;
        const auto cursor = audioSystem_->cursor_seconds(audioPreviewVoice_);
        stop_audio_preview();
        start_audio_preview();
        if (audioPreviewVoice_) {
            if (audioSystem_->supports_cursor() && std::isfinite(cursor))
                audioSystem_->seek(audioPreviewVoice_, cursor);
            if (paused) {
                audioSystem_->pause(audioPreviewVoice_);
                mediaPanel_.playback().state = ui::MediaPlaybackState::Paused;
            }
        }
        return;
    }
    const auto p = assetSettings_.audio_params(audioPreviewTrack_);
    audioSystem_->set_volume(audioPreviewVoice_, p.volume);
    audioSystem_->set_pitch(audioPreviewVoice_, p.pitch);
    audioSystem_->set_pan(audioPreviewVoice_, p.pan);
    audioSystem_->set_loop(audioPreviewVoice_, p.loop);
    audioSystem_->set_spatial(audioPreviewVoice_, p);
    if (effectsChanged && audioPreviewTrack_)
        audioSystem_->set_track_effects(audioPreviewTrack_, assetSettings_.audio_effects());
}
void EditorLayer::remap_asset_settings(std::string_view from, std::string_view to) {
    std::string error;
    if (!EditorAssetSettingsStore::remap(fileSystem_, from, to, error))
        push_console("Asset settings migration failed: " + error);
    const auto prefix = std::string(from) + "/";
    if (assetSettingsPath_ == from || assetSettingsPath_.rfind(prefix, 0) == 0)
        assetSettingsPath_ = std::string(to) + assetSettingsPath_.substr(from.size());
}
bool EditorLayer::create_audio_source_from_asset(World &world) {
    const auto *indexed = assetIndex_ ? assetIndex_->find(assetBrowser_.selection) : nullptr;
    if (!indexed || indexed->descriptor.kind != AssetPreviewKind::Audio ||
        fileSystem_.resolve_existing(std::filesystem::u8path(assetBrowser_.selection)).empty())
        return false;
    if (!checkpoint(world))
        return false;
    world.register_component_type<components::AudioSourceComponent>("AudioSource");
    auto &object = world.create_object(
        std::filesystem::u8path(assetBrowser_.selection).stem().generic_u8string());
    auto *source = object.add_component<components::AudioSourceComponent>();
    if (!source) {
        object.destroy();
        return false;
    }
    source->clipPath = assetBrowser_.selection;
    source->bus = assetSettings_.bus;
    source->loop = assetSettings_.loop;
    source->volume = assetSettings_.volume;
    source->pitch = assetSettings_.pitch;
    source->streaming = assetSettings_.streaming;
    source->spatialized = assetSettings_.spatialized;
    source->minDistance = assetSettings_.minDistance;
    source->maxDistance = assetSettings_.maxDistance;
    source->rolloff = assetSettings_.rolloff;
    if (auto *transform = object.get_component<components::TransformComponent>())
        transform->set_position(
            {assetSettings_.positionX, assetSettings_.positionY, assetSettings_.positionZ});
    layout_.selectedObject = object.id();
    document_changed();
    lastStatus_ = "Created AudioSource from " + assetBrowser_.selection;
    return true;
}
} // namespace shinkou::editor

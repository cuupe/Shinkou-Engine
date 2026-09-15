#include "shinkou/audio/AudioSceneSystem.h"

#include "shinkou/GameObject.h"
#include "shinkou/World.h"
#include "shinkou/assets/AssetSystem.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace shinkou::audio {
namespace {

constexpr std::uint32_t kLastBus = static_cast<std::uint32_t>(AudioBus::Count) - 1u;

enum class AssetIdentityState : std::uint8_t { Valid, Pending, Invalid };

struct AssetIdentityCheck {
    AssetIdentityState state{AssetIdentityState::Valid};
    std::string error{};
};

bool valid_project_path(std::string_view value, std::filesystem::path& normalized) {
    if (value.empty()) return false;
    normalized = std::filesystem::u8path(value).lexically_normal();
    if (normalized.empty() || normalized.is_absolute()) return false;
    const auto text = normalized.generic_u8string();
    return normalized != "." && normalized != ".." && text.rfind("../", 0) != 0;
}

AudioPlayParams play_params(const components::AudioSourceComponent& source, const GameObject& object) noexcept {
    AudioPlayParams params;
    params.bus = static_cast<AudioBus>(std::min(source.bus, kLastBus));
    params.loop = source.loop;
    params.spatialized = source.spatialized;
    params.streaming = source.streaming;
    params.volume = std::clamp(std::isfinite(source.volume) ? source.volume : 1.0f, 0.0f, 1.0f);
    params.pitch = std::clamp(std::isfinite(source.pitch) ? source.pitch : 1.0f, 0.01f, 8.0f);
    if (const auto* transform = object.get_component<components::TransformComponent>())
        params.position = math::TransformPoint(transform->world_matrix(), {});
    return params;
}

AssetIdentityCheck validate_asset_identity(const components::AudioSourceComponent& source,
                                           const std::string& normalizedPath,
                                           const assets::AssetSystem* assetSystem) {
    if (source.assetId == 0) return {};
    if (!assetSystem || !assetSystem->initialized())
        return {AssetIdentityState::Pending, "AudioSource AssetSystem manifest is unavailable"};
    if (!assetSystem->manifest_ready())
        return {AssetIdentityState::Pending, "AudioSource AssetSystem manifest is still scanning"};

    assets::AssetManifestEntry entry;
    if (!assetSystem->find_manifest(source.assetId, entry)) {
        return {assetSystem->manifest_ready() ? AssetIdentityState::Invalid : AssetIdentityState::Pending,
                "AudioSource AssetId is not present in the current manifest"};
    }
    if (entry.key.type != "audio")
        return {AssetIdentityState::Invalid, "AudioSource AssetId does not identify an audio resource"};

    const auto sourcePath = assetSystem->resolve_source(normalizedPath);
    if (sourcePath.empty())
        return {AssetIdentityState::Invalid, "AudioSource clip path cannot be resolved by AssetSystem"};
    std::error_code error;
    if (!std::filesystem::is_regular_file(sourcePath, error) || error)
        return {AssetIdentityState::Invalid, "AudioSource clip file is missing"};

    auto manifestPath = entry.sourcePath;
    if (manifestPath.is_relative()) manifestPath = assetSystem->resolve_source(entry.key.uri);
    if (manifestPath.empty())
        return {AssetIdentityState::Invalid, "AudioSource manifest source path cannot be resolved"};
    error.clear();
    if (!std::filesystem::is_regular_file(manifestPath, error) || error)
        return {AssetIdentityState::Invalid, "AudioSource manifest entry points to a missing file"};

    const auto canonical = [](const std::filesystem::path& path) {
        std::error_code canonicalError;
        const auto result = std::filesystem::weakly_canonical(path, canonicalError);
        return canonicalError ? path.lexically_normal() : result;
    };
    if (canonical(sourcePath) != canonical(manifestPath))
        return {AssetIdentityState::Invalid, "AudioSource path does not match its AssetId"};
    return {};
}

} // namespace

AudioAssetId AudioSceneSystem::acquire_clip(AudioSystem& audio, const std::string& path, bool streaming) {
    const auto found = clips_.find(path);
    if (found != clips_.end()) {
        ++found->second.references;
        return found->second.asset;
    }
    const auto asset = audio.load(std::filesystem::u8path(path), streaming);
    if (asset == 0) return 0;
    clips_.emplace(path, ClipBinding{asset, 1});
    return asset;
}

void AudioSceneSystem::release_clip(AudioSystem& audio, const std::string& path, AudioAssetId asset) {
    const auto found = clips_.find(path);
    if (found == clips_.end()) return;
    if (found->second.references > 1) {
        --found->second.references;
        return;
    }
    const auto loaded = found->second.asset != 0 ? found->second.asset : asset;
    clips_.erase(found);
    if (loaded != 0) audio.unload(loaded);
}

void AudioSceneSystem::stop_source(AudioSystem& audio, SourceBinding& binding) {
    if (binding.voice != 0) audio.stop(binding.voice);
    binding.voice = 0;
    if (binding.asset != 0) release_clip(audio, binding.path, binding.asset);
    binding.asset = 0;
    binding.pending = false;
}

bool AudioSceneSystem::start_source(GameObject& object, components::AudioSourceComponent& source,
                                    SourceBinding& binding, AudioSystem& audio,
                                    const assets::AssetSystem* assetSystem) {
    binding.assetId = source.assetId;
    binding.manifestRevision = assetSystem ? assetSystem->manifest_revision() : 0;
    std::filesystem::path normalized;
    if (!valid_project_path(source.clipPath, normalized)) {
        diagnostics_.lastError = "AudioSource clip path must be a project-relative file";
        ++diagnostics_.failedSources;
        binding.started = true;
        return false;
    }
    binding.path = normalized.generic_u8string();
    binding.bus = std::min(source.bus, kLastBus);
    binding.streaming = source.streaming;
    binding.loop = source.loop;
    binding.spatialized = source.spatialized;
    binding.volume = source.volume;
    binding.pitch = source.pitch;
    binding.pending = false;
    const auto identity = validate_asset_identity(source, binding.path, assetSystem);
    if (identity.state != AssetIdentityState::Valid) {
        diagnostics_.lastError = identity.error;
        if (identity.state == AssetIdentityState::Pending) {
            binding.pending = true;
            return false;
        }
        ++diagnostics_.failedSources;
        binding.started = true;
        return false;
    }
    binding.asset = acquire_clip(audio, binding.path, source.streaming);
    if (binding.asset == 0) {
        diagnostics_.lastError = audio.last_error().empty() ? "AudioSource clip could not be loaded" : audio.last_error();
        ++diagnostics_.failedSources;
        binding.started = true;
        return false;
    }
    const auto voice = audio.play(binding.asset, play_params(source, object));
    if (voice == 0) {
        diagnostics_.lastError = audio.last_error().empty() ? "AudioSource voice could not be created" : audio.last_error();
        release_clip(audio, binding.path, binding.asset);
        binding.asset = 0;
        ++diagnostics_.failedSources;
        binding.started = true;
        return false;
    }
    binding.voice = voice;
    binding.started = true;
    return true;
}

void AudioSceneSystem::sync(World& world, AudioSystem& audio,
                            const assets::AssetSystem* assetSystem) {
    diagnostics_ = {};
    ++revision_;
    if (!audio.initialized()) {
        diagnostics_.lastError = "AudioSystem is not connected";
        shutdown(audio);
        diagnostics_.lastError = "AudioSystem is not connected";
        return;
    }

    world.each_object([&](GameObject& object) {
        auto* source = object.get_component<components::AudioSourceComponent>();
        if (!source) return;
        ++diagnostics_.activeSources;
        auto& binding = sources_[object.id()];
        binding.lastSeenRevision = revision_;

        const auto desiredPath = [&] {
            std::filesystem::path normalized;
            return valid_project_path(source->clipPath, normalized) ? normalized.generic_u8string() : std::string{};
        }();
        const bool pathChanged = binding.path != desiredPath || binding.streaming != source->streaming;
        const bool identityChanged = binding.assetId != source->assetId;
        const auto manifestRevision = assetSystem ? assetSystem->manifest_revision() : 0;
        const bool manifestChanged = source->assetId != 0 && !binding.pending &&
            binding.manifestRevision != manifestRevision;
        const bool configChanged = binding.bus != std::min(source->bus, kLastBus) ||
            binding.streaming != source->streaming || binding.loop != source->loop ||
            binding.spatialized != source->spatialized || binding.volume != source->volume ||
            binding.pitch != source->pitch || binding.assetId != source->assetId;
        if (pathChanged || identityChanged || manifestChanged || (configChanged && binding.voice != 0)) {
            if (manifestChanged) {
                ++diagnostics_.invalidatedSources;
                diagnostics_.lastError = "AudioSource binding invalidated by AssetSystem manifest revision";
            }
            stop_source(audio, binding);
            binding.path = desiredPath;
            binding.streaming = source->streaming;
            binding.assetId = source->assetId;
            binding.manifestRevision = manifestRevision;
            binding.started = false;
            binding.pending = false;
        }
        if (!object.active_in_hierarchy() || !source->enabled() || source->clipPath.empty()) {
            if (binding.voice != 0 || binding.asset != 0) stop_source(audio, binding);
            binding.started = false;
            binding.pending = false;
            return;
        }
        if (binding.voice != 0) {
            const auto state = audio.state(binding.voice);
            if (state == AudioVoiceState::Invalid || state == AudioVoiceState::Stopped ||
                state == AudioVoiceState::Finished) {
                stop_source(audio, binding);
                // A finished or externally stopped voice is not replayed by
                // playOnStart until the source is explicitly reactivated or
                // its configuration/path changes.
                binding.started = true;
            }
        }
        if (source->playOnStart && !binding.started) {
            start_source(object, *source, binding, audio, assetSystem);
            if (binding.pending) ++diagnostics_.pendingSources;
        }
        if (binding.voice != 0 && source->spatialized)
            audio.set_spatial(binding.voice, play_params(*source, object));
        if (binding.voice != 0 && audio.state(binding.voice) == AudioVoiceState::Playing) ++diagnostics_.playingSources;
    });

    for (auto it = sources_.begin(); it != sources_.end();) {
        if (it->second.lastSeenRevision == revision_) {
            ++it;
            continue;
        }
        stop_source(audio, it->second);
        it = sources_.erase(it);
    }
    diagnostics_.loadedClips = clips_.size();
}

bool AudioSceneSystem::play(World& world, ObjectId objectId, AudioSystem& audio,
                            const assets::AssetSystem* assetSystem) {
    auto* object = world.find_object(objectId);
    if (!object) return false;
    auto* source = object->get_component<components::AudioSourceComponent>();
    if (!source || !audio.initialized() || !object->active_in_hierarchy() || !source->enabled()) return false;
    auto& binding = sources_[objectId];
    if (binding.voice != 0 || binding.asset != 0) stop_source(audio, binding);
    binding.path.clear();
    binding.started = false;
    binding.pending = false;
    return start_source(*object, *source, binding, audio, assetSystem);
}

bool AudioSceneSystem::pause(ObjectId objectId, AudioSystem& audio) {
    const auto found = sources_.find(objectId);
    if (found == sources_.end() || found->second.voice == 0 ||
        audio.state(found->second.voice) != AudioVoiceState::Playing) return false;
    audio.pause(found->second.voice);
    return audio.state(found->second.voice) == AudioVoiceState::Paused;
}

bool AudioSceneSystem::resume(ObjectId objectId, AudioSystem& audio) {
    const auto found = sources_.find(objectId);
    if (found == sources_.end() || found->second.voice == 0 ||
        audio.state(found->second.voice) != AudioVoiceState::Paused) return false;
    audio.resume(found->second.voice);
    return audio.state(found->second.voice) == AudioVoiceState::Playing;
}

void AudioSceneSystem::stop(ObjectId objectId, AudioSystem& audio) {
    const auto found = sources_.find(objectId);
    if (found == sources_.end()) return;
    stop_source(audio, found->second);
    found->second.started = true;
}

std::string AudioSceneSystem::transport_state(ObjectId objectId, const AudioSystem& audio) const {
    if (!audio.initialized()) return "Unavailable";
    const auto found = sources_.find(objectId);
    if (found == sources_.end()) return "Stopped";
    const auto& binding = found->second;
    if (binding.pending) return "Pending";
    if (binding.voice == 0) return "Stopped";
    switch (audio.state(binding.voice)) {
    case AudioVoiceState::Playing: return "Playing";
    case AudioVoiceState::Paused: return "Paused";
    case AudioVoiceState::Finished: return "Finished";
    case AudioVoiceState::Stopped: return "Stopped";
    case AudioVoiceState::Invalid: return "Unavailable";
    }
    return "Unavailable";
}

void AudioSceneSystem::shutdown(AudioSystem& audio) {
    for (auto& [id, binding] : sources_) {
        (void)id;
        stop_source(audio, binding);
    }
    sources_.clear();
    for (auto& [path, binding] : clips_) {
        (void)path;
        if (binding.asset != 0) audio.unload(binding.asset);
    }
    clips_.clear();
    diagnostics_ = {};
}

} // namespace shinkou::audio

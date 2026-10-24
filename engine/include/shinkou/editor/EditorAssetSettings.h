#pragma once
#include "shinkou/editor/FileSystem.h"
#include "shinkou/audio/AudioSystem.h"

namespace shinkou::editor {
// Source files stay untouched. These are editor audition/display preferences,
// not promises of runtime import/transcode behavior.
struct EditorAssetSettings {
    std::uint32_t version{1};
    bool wordWrap{true}, lineNumbers{true}, documentStyles{true};
    float textScale{1.0f};
    bool imageCheckerboard{true};
    float imageZoom{1.0f};
    bool loop{false}, streaming{true}, spatialized{false};
    std::uint32_t bus{5}; // UI audition bus by default.
    float volume{1}, pitch{1}, pan{0}, fadeIn{0};
    float minDistance{1}, maxDistance{100}, rolloff{1};
    float positionX{0}, positionY{0}, positionZ{0};
    bool lowPass{false}, reverb{false};
    float cutoffHz{12000}, reverbWet{0.25f};
    float videoRate{1};
    bool modelAutoRotate{false}, modelWireframe{false};

    bool valid() const noexcept;
    audio::AudioPlayParams audio_params(audio::AudioTrackId track=0) const noexcept;
    std::vector<audio::AudioEffectDesc> audio_effects() const;
};
class EditorAssetSettingsStore {
public:
    static std::filesystem::path path_for(std::string_view asset);
    // Missing settings are defaults. Malformed settings leave the caller intact.
    static bool load(const FileSystemService&, std::string_view asset, EditorAssetSettings&, std::string& error);
    static bool save(const FileSystemService&, std::string_view asset, const EditorAssetSettings&, std::string& error);
    static bool remap(const FileSystemService&, std::string_view from, std::string_view to, std::string& error);
};
}

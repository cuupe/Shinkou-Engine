#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace shinkou::ui {

// Media is deliberately a state/model layer. A renderer or decoder adapter may
// interpret the URI and consume the state without this header knowing anything
// about ImGui, SDL, FFmpeg, WIC, OpenAL, or a GPU API.
inline constexpr std::uint32_t kMediaSchemaVersion = 1;
inline constexpr std::string_view kMediaSchema = "shinkou.ui-media";

enum class MediaKind : std::uint8_t { Image, Video, Audio };
enum class MediaPlaybackState : std::uint8_t { Stopped, Playing, Paused };

const char* media_kind_name(MediaKind kind) noexcept;
bool parse_media_kind(std::string_view value, MediaKind& kind) noexcept;
const char* media_playback_state_name(MediaPlaybackState state) noexcept;
bool parse_media_playback_state(std::string_view value, MediaPlaybackState& state) noexcept;

struct MediaResource {
    // URI may be a filesystem path, asset:// URI, http(s) URI, or an opaque
    // application-defined scheme. Resolving and decoding it is not this layer's job.
    std::string uri{};
    std::string mimeType{};
    std::string label{};

    bool valid(std::string* error = nullptr) const;
};

struct MediaPanelDescription {
    std::string id{};
    std::string title{"Media"};
    MediaKind kind{MediaKind::Image};
    MediaResource resource{};

    bool showPlaybackControls{true};
    bool showTimeline{true};
    bool showVolume{false};
    bool preserveAspectRatio{true};
    // Zero means that the renderer should use the decoded asset's aspect ratio.
    float preferredAspectRatio{0.0f};

    bool valid(std::string* error = nullptr) const;
};

struct MediaPlayback {
    MediaPlaybackState state{MediaPlaybackState::Stopped};
    // A duration of zero means unknown/not loaded. Unknown duration is still a
    // valid state for a streaming or not-yet-decoded video/audio resource.
    double currentTime{0.0};
    double duration{0.0};
    double volume{1.0};
    double playbackRate{1.0};
    bool loop{false};
    bool muted{false};

    bool valid(std::string* error = nullptr) const;
};

enum class MediaCommandType : std::uint8_t {
    Play,
    Pause,
    Stop,
    TogglePlay,
    Seek,
    SetDuration,
    SetVolume,
    SetLoop,
    SetMuted,
    SetPlaybackRate,
};

struct MediaCommand {
    MediaCommandType type{MediaCommandType::Pause};
    double value{0.0};
    bool flag{false};

    static MediaCommand play() noexcept;
    static MediaCommand pause() noexcept;
    static MediaCommand stop() noexcept;
    static MediaCommand toggle_play() noexcept;
    static MediaCommand seek(double seconds) noexcept;
    static MediaCommand set_duration(double seconds) noexcept;
    static MediaCommand set_volume(double normalizedVolume) noexcept;
    static MediaCommand set_loop(bool enabled) noexcept;
    static MediaCommand set_muted(bool enabled) noexcept;
    static MediaCommand set_playback_rate(double rate) noexcept;
};

enum class MediaCommandResult : std::uint8_t {
    Applied,
    Ignored,
    Invalid,
};

class MediaPanel final {
    MediaPanelDescription description_{};
    MediaPlayback playback_{};

public:
    MediaPanel() = default;
    explicit MediaPanel(MediaPanelDescription description) : description_(std::move(description)) {}
    MediaPanel(MediaPanelDescription description, MediaPlayback playback)
        : description_(std::move(description)), playback_(std::move(playback)) {}

    MediaPanelDescription& description() noexcept { return description_; }
    const MediaPanelDescription& description() const noexcept { return description_; }
    MediaPlayback& playback() noexcept { return playback_; }
    const MediaPlayback& playback() const noexcept { return playback_; }

    void set_description(MediaPanelDescription description) { description_ = std::move(description); }
    void set_resource(MediaResource resource) { description_.resource = std::move(resource); }

    // Apply is the renderer/engine integration seam. It only changes model
    // state; a backend adapter can observe the resulting state and do the work.
    MediaCommandResult apply(const MediaCommand& command, std::string* error = nullptr);

    // Advances known-duration media and also advances streams whose duration is
    // not known. Image panels intentionally do not advance or enter Playing.
    void update(double deltaSeconds);

    bool valid(std::string* error = nullptr) const;
    std::string to_json(bool pretty = false) const;
    bool from_json(std::string_view json, std::string* error = nullptr);
};

std::string serialize_json(const MediaPanel& panel, bool pretty = false);
bool deserialize_json(std::string_view json, MediaPanel& panel, std::string* error = nullptr);

} // namespace shinkou::ui

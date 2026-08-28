#include "shinkou/ui/Media.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

namespace shinkou::ui {
namespace {

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

bool finite(double value) noexcept { return std::isfinite(value); }

struct JsonValue {
    enum class Kind : std::uint8_t { Null, Boolean, Number, String, Object };
    Kind kind{Kind::Null};
    bool boolean{false};
    double number{0.0};
    std::string string{};
    std::map<std::string, JsonValue> object{};
};

class JsonParser final {
    std::string_view input_;
    std::size_t position_{0};
    std::string error_{};

    void skip_space() noexcept {
        while (position_ < input_.size()) {
            const char c = input_[position_];
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
            ++position_;
        }
    }

    bool fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
        return false;
    }

    bool consume(char expected) {
        skip_space();
        if (position_ >= input_.size() || input_[position_] != expected) {
            return fail(std::string("expected '") + expected + "'" );
        }
        ++position_;
        return true;
    }

    bool hex_digit(char c, unsigned& value) const noexcept {
        if (c >= '0' && c <= '9') value = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') value = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value = static_cast<unsigned>(c - 'A' + 10);
        else return false;
        return true;
    }

    void append_utf8(std::string& output, unsigned codePoint) {
        if (codePoint <= 0x7Fu) output.push_back(static_cast<char>(codePoint));
        else if (codePoint <= 0x7FFu) {
            output.push_back(static_cast<char>(0xC0u | (codePoint >> 6u)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        } else if (codePoint <= 0xFFFFu) {
            output.push_back(static_cast<char>(0xE0u | (codePoint >> 12u)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        } else if (codePoint <= 0x10FFFFu) {
            output.push_back(static_cast<char>(0xF0u | (codePoint >> 18u)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        }
    }

    bool parse_string(std::string& output) {
        skip_space();
        if (position_ >= input_.size() || input_[position_] != '"') return fail("expected JSON string");
        ++position_;
        output.clear();
        while (position_ < input_.size()) {
            const unsigned char raw = static_cast<unsigned char>(input_[position_++]);
            if (raw == '"') return true;
            if (raw < 0x20u) return fail("control character in JSON string");
            if (raw != '\\') {
                output.push_back(static_cast<char>(raw));
                continue;
            }
            if (position_ >= input_.size()) return fail("unfinished JSON escape");
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                unsigned codePoint = 0;
                for (unsigned index = 0; index < 4; ++index) {
                    if (position_ >= input_.size()) return fail("unfinished unicode escape");
                    unsigned digit = 0;
                    if (!hex_digit(input_[position_++], digit)) return fail("invalid unicode escape");
                    codePoint = (codePoint << 4u) | digit;
                }
                append_utf8(output, codePoint);
                break;
            }
            default: return fail("invalid JSON escape");
            }
        }
        return fail("unterminated JSON string");
    }

    bool parse_number(JsonValue& output) {
        skip_space();
        const std::size_t start = position_;
        while (position_ < input_.size()) {
            const char c = input_[position_];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') ++position_;
            else break;
        }
        if (start == position_) return fail("expected JSON value");
        const std::string number(input_.substr(start, position_ - start));
        char* end = nullptr;
        const double value = std::strtod(number.c_str(), &end);
        if (end == number.c_str() || *end != '\0' || !finite(value)) return fail("invalid JSON number");
        output.kind = JsonValue::Kind::Number;
        output.number = value;
        return true;
    }

    bool parse_value(JsonValue& output, unsigned depth) {
        if (depth > 32u) return fail("JSON nesting is too deep");
        skip_space();
        if (position_ >= input_.size()) return fail("missing JSON value");
        const char c = input_[position_];
        if (c == '"') {
            output.kind = JsonValue::Kind::String;
            return parse_string(output.string);
        }
        if (c == '{') {
            output.kind = JsonValue::Kind::Object;
            ++position_;
            skip_space();
            if (position_ < input_.size() && input_[position_] == '}') { ++position_; return true; }
            while (position_ < input_.size()) {
                std::string key;
                if (!parse_string(key) || !consume(':')) return false;
                JsonValue value;
                if (!parse_value(value, depth + 1u)) return false;
                output.object.insert_or_assign(std::move(key), std::move(value));
                skip_space();
                if (position_ < input_.size() && input_[position_] == '}') { ++position_; return true; }
                if (!consume(',')) return false;
            }
            return fail("unterminated JSON object");
        }
        if (input_.substr(position_, 4) == "true") {
            output.kind = JsonValue::Kind::Boolean; output.boolean = true; position_ += 4; return true;
        }
        if (input_.substr(position_, 5) == "false") {
            output.kind = JsonValue::Kind::Boolean; output.boolean = false; position_ += 5; return true;
        }
        if (input_.substr(position_, 4) == "null") {
            output.kind = JsonValue::Kind::Null; position_ += 4; return true;
        }
        return parse_number(output);
    }

public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool parse(JsonValue& output, std::string& error) {
        if (!parse_value(output, 0u)) { error = error_; return false; }
        skip_space();
        if (position_ != input_.size()) { error = "trailing data after JSON value"; return false; }
        return true;
    }
};

const JsonValue* field(const JsonValue& object, std::string_view name, std::string* error) {
    if (object.kind != JsonValue::Kind::Object) { set_error(error, "media JSON value is not an object"); return nullptr; }
    const auto iterator = object.object.find(std::string(name));
    if (iterator == object.object.end()) { set_error(error, "missing media field: " + std::string(name)); return nullptr; }
    return &iterator->second;
}

bool string_field(const JsonValue& object, std::string_view name, std::string& value, std::string* error) {
    const auto* item = field(object, name, error);
    if (!item) return false;
    if (item->kind != JsonValue::Kind::String) { set_error(error, "media field is not a string: " + std::string(name)); return false; }
    value = item->string;
    return true;
}

bool bool_field(const JsonValue& object, std::string_view name, bool& value, std::string* error) {
    const auto* item = field(object, name, error);
    if (!item) return false;
    if (item->kind != JsonValue::Kind::Boolean) { set_error(error, "media field is not a boolean: " + std::string(name)); return false; }
    value = item->boolean;
    return true;
}

bool number_field(const JsonValue& object, std::string_view name, double& value, std::string* error) {
    const auto* item = field(object, name, error);
    if (!item) return false;
    if (item->kind != JsonValue::Kind::Number) { set_error(error, "media field is not a number: " + std::string(name)); return false; }
    value = item->number;
    return true;
}

bool object_field(const JsonValue& object, std::string_view name, const JsonValue*& value, std::string* error) {
    value = field(object, name, error);
    if (!value) return false;
    if (value->kind != JsonValue::Kind::Object) { set_error(error, "media field is not an object: " + std::string(name)); return false; }
    return true;
}

std::string json_escape(std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char c : value) {
        switch (c) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (c < 0x20u) output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c) << std::dec;
            else output << static_cast<char>(c);
            break;
        }
    }
    output << '"';
    return output.str();
}

class JsonWriter final {
    std::ostringstream output_;
    bool pretty_{false};
    unsigned depth_{0};

    void newline() { if (pretty_) output_ << '\n' << std::string(depth_ * 2u, ' '); }
    void key(std::string_view name, bool& first) {
        if (!first) output_ << ',';
        newline();
        output_ << json_escape(name) << (pretty_ ? ": " : ":");
        first = false;
    }

public:
    explicit JsonWriter(bool pretty) : pretty_(pretty) {}

    void write(const MediaPanel& panel) {
        const auto& description = panel.description();
        const auto& playback = panel.playback();
        output_ << '{'; ++depth_;
        bool first = true;
        key("schema", first); output_ << json_escape(kMediaSchema);
        key("version", first); output_ << kMediaSchemaVersion;
        key("description", first); write_description(description);
        key("playback", first); write_playback(playback);
        --depth_; newline(); output_ << '}';
    }

    void write_description(const MediaPanelDescription& description) {
        output_ << '{'; ++depth_; bool first = true;
        key("id", first); output_ << json_escape(description.id);
        key("title", first); output_ << json_escape(description.title);
        key("kind", first); output_ << json_escape(media_kind_name(description.kind));
        key("resource", first); output_ << '{'; ++depth_; bool resourceFirst = true;
        key("uri", resourceFirst); output_ << json_escape(description.resource.uri);
        key("mimeType", resourceFirst); output_ << json_escape(description.resource.mimeType);
        key("label", resourceFirst); output_ << json_escape(description.resource.label);
        --depth_; newline(); output_ << '}';
        key("showPlaybackControls", first); output_ << (description.showPlaybackControls ? "true" : "false");
        key("showTimeline", first); output_ << (description.showTimeline ? "true" : "false");
        key("showVolume", first); output_ << (description.showVolume ? "true" : "false");
        key("preserveAspectRatio", first); output_ << (description.preserveAspectRatio ? "true" : "false");
        key("preferredAspectRatio", first); output_ << std::setprecision(17) << description.preferredAspectRatio;
        --depth_; newline(); output_ << '}';
    }

    void write_playback(const MediaPlayback& playback) {
        output_ << '{'; ++depth_; bool first = true;
        key("state", first); output_ << json_escape(media_playback_state_name(playback.state));
        key("currentTime", first); output_ << std::setprecision(17) << playback.currentTime;
        key("duration", first); output_ << std::setprecision(17) << playback.duration;
        key("volume", first); output_ << std::setprecision(17) << playback.volume;
        key("playbackRate", first); output_ << std::setprecision(17) << playback.playbackRate;
        key("loop", first); output_ << (playback.loop ? "true" : "false");
        key("muted", first); output_ << (playback.muted ? "true" : "false");
        --depth_; newline(); output_ << '}';
    }

    std::string str() const { return output_.str(); }
};

bool parse_description(const JsonValue& value, MediaPanelDescription& description, std::string* error) {
    if (value.kind != JsonValue::Kind::Object) { set_error(error, "media description is not an object"); return false; }
    if (!string_field(value, "id", description.id, error) || !string_field(value, "title", description.title, error)) return false;
    std::string kind;
    if (!string_field(value, "kind", kind, error) || !parse_media_kind(kind, description.kind)) { set_error(error, "invalid media kind"); return false; }
    const JsonValue* resource = nullptr;
    if (!object_field(value, "resource", resource, error) ||
        !string_field(*resource, "uri", description.resource.uri, error) ||
        !string_field(*resource, "mimeType", description.resource.mimeType, error) ||
        !string_field(*resource, "label", description.resource.label, error)) return false;
    if (!bool_field(value, "showPlaybackControls", description.showPlaybackControls, error) ||
        !bool_field(value, "showTimeline", description.showTimeline, error) ||
        !bool_field(value, "showVolume", description.showVolume, error) ||
        !bool_field(value, "preserveAspectRatio", description.preserveAspectRatio, error)) return false;
    double aspect = 0.0;
    if (!number_field(value, "preferredAspectRatio", aspect, error)) return false;
    description.preferredAspectRatio = static_cast<float>(aspect);
    return description.valid(error);
}

bool parse_playback(const JsonValue& value, MediaPlayback& playback, std::string* error) {
    if (value.kind != JsonValue::Kind::Object) { set_error(error, "media playback is not an object"); return false; }
    std::string state;
    if (!string_field(value, "state", state, error) || !parse_media_playback_state(state, playback.state)) { set_error(error, "invalid media playback state"); return false; }
    if (!number_field(value, "currentTime", playback.currentTime, error) ||
        !number_field(value, "duration", playback.duration, error) ||
        !number_field(value, "volume", playback.volume, error) ||
        !number_field(value, "playbackRate", playback.playbackRate, error) ||
        !bool_field(value, "loop", playback.loop, error) ||
        !bool_field(value, "muted", playback.muted, error)) return false;
    return playback.valid(error);
}

} // namespace

const char* media_kind_name(MediaKind kind) noexcept {
    switch (kind) {
    case MediaKind::Video: return "video";
    case MediaKind::Audio: return "audio";
    default: return "image";
    }
}

bool parse_media_kind(std::string_view value, MediaKind& kind) noexcept {
    if (value == "image") { kind = MediaKind::Image; return true; }
    if (value == "video") { kind = MediaKind::Video; return true; }
    if (value == "audio") { kind = MediaKind::Audio; return true; }
    return false;
}

const char* media_playback_state_name(MediaPlaybackState state) noexcept {
    switch (state) {
    case MediaPlaybackState::Playing: return "playing";
    case MediaPlaybackState::Paused: return "paused";
    default: return "stopped";
    }
}

bool parse_media_playback_state(std::string_view value, MediaPlaybackState& state) noexcept {
    if (value == "stopped") { state = MediaPlaybackState::Stopped; return true; }
    if (value == "playing") { state = MediaPlaybackState::Playing; return true; }
    if (value == "paused") { state = MediaPlaybackState::Paused; return true; }
    return false;
}

bool MediaResource::valid(std::string* error) const {
    if (uri.empty()) { set_error(error, "media resource URI is empty"); return false; }
    return true;
}

bool MediaPanelDescription::valid(std::string* error) const {
    if (id.empty()) { set_error(error, "media panel id is empty"); return false; }
    if (title.empty()) { set_error(error, "media panel title is empty"); return false; }
    if (!resource.valid(error)) return false;
    if (!finite(preferredAspectRatio) || preferredAspectRatio < 0.0f) {
        set_error(error, "preferred media aspect ratio is invalid"); return false;
    }
    return true;
}

bool MediaPlayback::valid(std::string* error) const {
    if (!finite(currentTime) || !finite(duration) || !finite(volume) || !finite(playbackRate) ||
        currentTime < 0.0 || duration < 0.0 || (currentTime > duration && duration > 0.0) ||
        volume < 0.0 || volume > 1.0 || playbackRate < 0.05 || playbackRate > 8.0) {
        set_error(error, "media playback values are invalid"); return false;
    }
    return true;
}

MediaCommand MediaCommand::play() noexcept { return {MediaCommandType::Play, 0.0, false}; }
MediaCommand MediaCommand::pause() noexcept { return {MediaCommandType::Pause, 0.0, false}; }
MediaCommand MediaCommand::stop() noexcept { return {MediaCommandType::Stop, 0.0, false}; }
MediaCommand MediaCommand::toggle_play() noexcept { return {MediaCommandType::TogglePlay, 0.0, false}; }
MediaCommand MediaCommand::seek(double seconds) noexcept { return {MediaCommandType::Seek, seconds, false}; }
MediaCommand MediaCommand::set_duration(double seconds) noexcept { return {MediaCommandType::SetDuration, seconds, false}; }
MediaCommand MediaCommand::set_volume(double normalizedVolume) noexcept { return {MediaCommandType::SetVolume, normalizedVolume, false}; }
MediaCommand MediaCommand::set_loop(bool enabled) noexcept { return {MediaCommandType::SetLoop, 0.0, enabled}; }
MediaCommand MediaCommand::set_muted(bool enabled) noexcept { return {MediaCommandType::SetMuted, 0.0, enabled}; }
MediaCommand MediaCommand::set_playback_rate(double rate) noexcept { return {MediaCommandType::SetPlaybackRate, rate, false}; }

MediaCommandResult MediaPanel::apply(const MediaCommand& command, std::string* error) {
    if (!finite(command.value)) { set_error(error, "media command value is not finite"); return MediaCommandResult::Invalid; }
    const bool playable = description_.kind == MediaKind::Video || description_.kind == MediaKind::Audio;
    switch (command.type) {
    case MediaCommandType::Play:
        if (!playable) return MediaCommandResult::Ignored;
        if (playback_.duration > 0.0 && playback_.currentTime >= playback_.duration) playback_.currentTime = 0.0;
        playback_.state = MediaPlaybackState::Playing; return MediaCommandResult::Applied;
    case MediaCommandType::Pause:
        if (!playable) return MediaCommandResult::Ignored;
        playback_.state = MediaPlaybackState::Paused; return MediaCommandResult::Applied;
    case MediaCommandType::Stop:
        if (!playable) return MediaCommandResult::Ignored;
        playback_.state = MediaPlaybackState::Stopped; playback_.currentTime = 0.0; return MediaCommandResult::Applied;
    case MediaCommandType::TogglePlay:
        if (!playable) return MediaCommandResult::Ignored;
        return apply(playback_.state == MediaPlaybackState::Playing ? MediaCommand::pause() : MediaCommand::play(), error);
    case MediaCommandType::Seek:
        if (!playable || command.value < 0.0) { set_error(error, "invalid media seek"); return MediaCommandResult::Invalid; }
        playback_.currentTime = playback_.duration > 0.0 ? std::min(command.value, playback_.duration) : command.value;
        return MediaCommandResult::Applied;
    case MediaCommandType::SetDuration:
        if (!playable || command.value < 0.0) { set_error(error, "invalid media duration"); return MediaCommandResult::Invalid; }
        playback_.duration = command.value;
        if (playback_.duration > 0.0) playback_.currentTime = std::min(playback_.currentTime, playback_.duration);
        return MediaCommandResult::Applied;
    case MediaCommandType::SetVolume:
        if (command.value < 0.0 || command.value > 1.0) { set_error(error, "media volume must be between 0 and 1"); return MediaCommandResult::Invalid; }
        playback_.volume = command.value; return MediaCommandResult::Applied;
    case MediaCommandType::SetLoop:
        playback_.loop = command.flag; return MediaCommandResult::Applied;
    case MediaCommandType::SetMuted:
        playback_.muted = command.flag; return MediaCommandResult::Applied;
    case MediaCommandType::SetPlaybackRate:
        if (command.value < 0.05 || command.value > 8.0) { set_error(error, "media playback rate must be between 0.05 and 8"); return MediaCommandResult::Invalid; }
        playback_.playbackRate = command.value; return MediaCommandResult::Applied;
    }
    set_error(error, "unknown media command");
    return MediaCommandResult::Invalid;
}

void MediaPanel::update(double deltaSeconds) {
    if (description_.kind == MediaKind::Image || playback_.state != MediaPlaybackState::Playing ||
        !finite(deltaSeconds) || deltaSeconds <= 0.0) return;
    playback_.currentTime += deltaSeconds * playback_.playbackRate;
    if (playback_.duration <= 0.0 || playback_.currentTime < playback_.duration) return;
    if (playback_.loop) {
        playback_.currentTime = std::fmod(playback_.currentTime, playback_.duration);
        if (playback_.currentTime < 0.0) playback_.currentTime = 0.0;
    } else {
        playback_.currentTime = playback_.duration;
        playback_.state = MediaPlaybackState::Stopped;
    }
}

bool MediaPanel::valid(std::string* error) const {
    return description_.valid(error) && playback_.valid(error);
}

std::string MediaPanel::to_json(bool pretty) const { return serialize_json(*this, pretty); }

bool MediaPanel::from_json(std::string_view json, std::string* error) { return deserialize_json(json, *this, error); }

std::string serialize_json(const MediaPanel& panel, bool pretty) {
    JsonWriter writer(pretty);
    writer.write(panel);
    return writer.str();
}

bool deserialize_json(std::string_view json, MediaPanel& panel, std::string* error) {
    if (json.size() > 1024u * 1024u) { set_error(error, "media JSON is too large"); return false; }
    JsonValue root;
    std::string parseError;
    if (!JsonParser(json).parse(root, parseError)) { set_error(error, std::move(parseError)); return false; }
    std::string schema;
    if (!string_field(root, "schema", schema, error) || schema != kMediaSchema) {
        set_error(error, "unsupported or missing media schema"); return false;
    }
    double version = 0.0;
    if (!number_field(root, "version", version, error) || version != static_cast<double>(kMediaSchemaVersion)) {
        set_error(error, "unsupported or missing media schema version"); return false;
    }
    const JsonValue* description = nullptr;
    const JsonValue* playback = nullptr;
    if (!object_field(root, "description", description, error) || !object_field(root, "playback", playback, error)) return false;
    MediaPanelDescription parsedDescription = panel.description();
    MediaPlayback parsedPlayback = panel.playback();
    if (!parse_description(*description, parsedDescription, error) || !parse_playback(*playback, parsedPlayback, error)) return false;
    MediaPanel parsed = panel;
    parsed.set_description(std::move(parsedDescription));
    parsed.playback() = std::move(parsedPlayback);
    panel = std::move(parsed);
    return true;
}

} // namespace shinkou::ui

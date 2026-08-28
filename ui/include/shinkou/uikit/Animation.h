#pragma once

#include "Types.h"
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace shinkou::uikit {

enum class Easing { Linear, EaseIn, EaseOut, EaseInOut };
Easing easing_from_string(const std::string& value);

struct AnimationOptions {
    bool reduceMotion = false;
};

class AnimationTimeline {
public:
    using Value = std::variant<float, Color>;
    using Setter = std::function<void(const Value&)>;

    std::size_t add_float(float from, float to, float durationMs, Setter setter, Easing easing = Easing::EaseOut, int loops = 1);
    std::size_t add_color(Color from, Color to, float durationMs, Setter setter, Easing easing = Easing::EaseOut, int loops = 1);
    void update(float deltaSeconds, AnimationOptions options = {});
    void cancel(std::size_t id);
    void clear();
    std::size_t size() const { return m_tracks.size(); }

private:
    struct Track {
        std::size_t id = 0;
        Value from{};
        Value to{};
        float durationMs = 0.0f;
        float elapsedMs = 0.0f;
        int loops = 1;
        int completedLoops = 0;
        Easing easing = Easing::EaseOut;
        Setter setter;
    };
    static float apply_easing(float value, Easing easing);
    std::vector<Track> m_tracks;
    std::size_t m_nextId = 1;
};

} // namespace shinkou::uikit


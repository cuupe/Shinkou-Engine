#include "shinkou/uikit/Animation.h"
#include <algorithm>
#include <cmath>

namespace shinkou::uikit {

Easing easing_from_string(const std::string& value) {
    if (value == "linear") return Easing::Linear;
    if (value == "ease-in") return Easing::EaseIn;
    if (value == "ease-in-out") return Easing::EaseInOut;
    return Easing::EaseOut;
}

std::size_t AnimationTimeline::add_float(float from, float to, float durationMs, Setter setter, Easing easing, int loops) {
    Track track;
    track.id = m_nextId++;
    track.from = from; track.to = to; track.durationMs = std::max(0.0f, durationMs);
    track.easing = easing; track.loops = std::max(1, loops); track.setter = std::move(setter);
    if (track.setter) track.setter(from);
    m_tracks.push_back(std::move(track));
    return m_tracks.back().id;
}

std::size_t AnimationTimeline::add_color(Color from, Color to, float durationMs, Setter setter, Easing easing, int loops) {
    Track track;
    track.id = m_nextId++;
    track.from = from; track.to = to; track.durationMs = std::max(0.0f, durationMs);
    track.easing = easing; track.loops = std::max(1, loops); track.setter = std::move(setter);
    if (track.setter) track.setter(from);
    m_tracks.push_back(std::move(track));
    return m_tracks.back().id;
}

float AnimationTimeline::apply_easing(float value, Easing easing) {
    value = clamp01(value);
    switch (easing) {
    case Easing::Linear: return value;
    case Easing::EaseIn: return value * value;
    case Easing::EaseOut: return 1.0f - (1.0f - value) * (1.0f - value);
    case Easing::EaseInOut: return value < 0.5f ? 2.0f * value * value : 1.0f - std::pow(-2.0f * value + 2.0f, 2.0f) / 2.0f;
    }
    return value;
}

void AnimationTimeline::update(float deltaSeconds, AnimationOptions options) {
    const float deltaMs = std::max(0.0f, deltaSeconds * 1000.0f);
    for (Track& track : m_tracks) {
        if (options.reduceMotion || track.durationMs <= 0.0f) {
            if (track.setter) track.setter(track.to);
            track.completedLoops = track.loops;
            continue;
        }
        track.elapsedMs += deltaMs;
        const float cycle = std::min(track.elapsedMs / track.durationMs, static_cast<float>(track.loops));
        track.completedLoops = static_cast<int>(cycle);
        float progress = cycle - static_cast<float>(track.completedLoops);
        if (track.elapsedMs >= track.durationMs * track.loops) progress = 1.0f;
        progress = apply_easing(progress, track.easing);
        if (std::holds_alternative<float>(track.from)) {
            const float from = std::get<float>(track.from);
            const float to = std::get<float>(track.to);
            if (track.setter) track.setter(from + (to - from) * progress);
        } else {
            const Color from = std::get<Color>(track.from);
            const Color to = std::get<Color>(track.to);
            Color value;
            value.r = from.r + (to.r - from.r) * progress;
            value.g = from.g + (to.g - from.g) * progress;
            value.b = from.b + (to.b - from.b) * progress;
            value.a = from.a + (to.a - from.a) * progress;
            if (track.setter) track.setter(value);
        }
    }
    m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(), [](const Track& track) {
        return track.completedLoops >= track.loops;
    }), m_tracks.end());
}

void AnimationTimeline::cancel(std::size_t id) {
    m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(), [id](const Track& track) { return track.id == id; }), m_tracks.end());
}

void AnimationTimeline::clear() { m_tracks.clear(); }

} // namespace shinkou::uikit


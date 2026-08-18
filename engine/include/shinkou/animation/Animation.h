#pragma once

#include "shinkou/Math.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shinkou::animation {

using BoneIndex = std::uint16_t;
constexpr BoneIndex InvalidBone = std::numeric_limits<BoneIndex>::max();

struct Skeleton {
    std::vector<BoneIndex> parents;
    std::vector<math::Transform> bindPose;
    std::vector<math::Mat4> inverseBindMatrices;
    std::vector<std::string> names;

    bool valid() const noexcept;
    std::size_t bone_count() const noexcept { return parents.size(); }
    BoneIndex find_bone(std::string_view name) const noexcept;
};

struct Pose {
    std::vector<math::Transform> local;
    std::vector<math::Mat4> model;

    void resize(std::size_t boneCount);
    void reset_to_bind_pose(const Skeleton& skeleton);
    bool compatible(const Skeleton& skeleton) const noexcept;
};

struct Vec3Key {
    using value_type = math::Vec3;
    float time{0.0f};
    math::Vec3 value{};
};

struct QuatKey {
    using value_type = math::Quat;
    float time{0.0f};
    math::Quat value{};
};

struct BoneTrack {
    std::vector<Vec3Key> positions;
    std::vector<QuatKey> rotations;
    std::vector<Vec3Key> scales;
};

struct SampleCursor {
    std::size_t position{0};
    std::size_t rotation{0};
    std::size_t scale{0};
    float lastTime{-std::numeric_limits<float>::infinity()};

    void reset() noexcept { position = rotation = scale = 0; lastTime = -std::numeric_limits<float>::infinity(); }
};

class AnimationClip {
    std::string name_;
    float duration_{0.0f};
    std::vector<BoneTrack> tracks_;

public:
    AnimationClip() = default;
    explicit AnimationClip(std::size_t boneCount) : tracks_(boneCount) {}

    void set_name(std::string name) { name_ = std::move(name); }
    std::string_view name() const noexcept { return name_; }
    void set_duration(float duration) noexcept { duration_ = duration > 0.0f ? duration : 0.0f; }
    float duration() const noexcept { return duration_; }
    void resize_tracks(std::size_t boneCount) { tracks_.resize(boneCount); }
    std::size_t track_count() const noexcept { return tracks_.size(); }
    BoneTrack& track(std::size_t bone) noexcept { return tracks_[bone]; }
    const BoneTrack& track(std::size_t bone) const noexcept { return tracks_[bone]; }
    bool valid_for(const Skeleton& skeleton) const noexcept;

    void sample(const Skeleton& skeleton, float time, Pose& output,
                std::vector<SampleCursor>& cursors) const noexcept;
};

enum class PlaybackMode : std::uint8_t { Once, Loop, PingPong };

class Animator {
    const Skeleton* skeleton_{nullptr};
    const AnimationClip* clip_{nullptr};
    Pose pose_{};
    std::vector<SampleCursor> cursors_;
    float time_{0.0f};
    float speed_{1.0f};
    PlaybackMode mode_{PlaybackMode::Loop};
    bool playing_{true};
    bool forward_{true};

    void sample_current() noexcept;

public:
    Animator() = default;
    explicit Animator(const Skeleton& skeleton) { set_skeleton(skeleton); }

    bool set_skeleton(const Skeleton& skeleton) noexcept;
    bool set_clip(const AnimationClip* clip) noexcept;
    void play() noexcept { playing_ = true; }
    void pause() noexcept { playing_ = false; }
    void stop() noexcept;
    void seek(float time) noexcept;
    void update(float deltaSeconds) noexcept;

    void set_speed(float speed) noexcept { speed_ = speed; }
    void set_playback_mode(PlaybackMode mode) noexcept { mode_ = mode; }
    float time() const noexcept { return time_; }
    bool playing() const noexcept { return playing_; }
    const Skeleton* skeleton() const noexcept { return skeleton_; }
    const AnimationClip* clip() const noexcept { return clip_; }
    const Pose& pose() const noexcept { return pose_; }
    Pose& pose() noexcept { return pose_; }
};

void blend(const Skeleton& skeleton, const Pose& a, const Pose& b, float weight, Pose& output) noexcept;
void build_skin_matrices(const Skeleton& skeleton, const Pose& pose,
                         std::vector<math::Mat4>& output) noexcept;

class AnimationSystem {
    std::vector<Animator*> animators_;

public:
    void add(Animator& animator);
    void remove(Animator& animator) noexcept;
    void update(float deltaSeconds) noexcept;
    std::size_t size() const noexcept { return animators_.size(); }
};

}

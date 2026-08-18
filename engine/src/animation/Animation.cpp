#include "shinkou/animation/Animation.h"

#include <algorithm>
#include <cmath>

namespace shinkou::animation {
namespace {

template<class Key>
typename Key::value_type sample_key(const std::vector<Key>& keys, float time,
                                    std::size_t& cursor, typename Key::value_type fallback) noexcept {
    if (keys.empty()) return fallback;
    if (time < keys.front().time) { cursor = 0; return keys.front().value; }
    if (time >= keys.back().time) { cursor = keys.size() - 1; return keys.back().value; }
    if (cursor >= keys.size() || time < keys[cursor].time) cursor = 0;
    while (cursor + 1 < keys.size() && keys[cursor + 1].time <= time) ++cursor;
    const auto& first = keys[cursor];
    const auto& second = keys[cursor + 1];
    const float span = second.time - first.time;
    const float alpha = span > math::Epsilon ? (time - first.time) / span : 0.0f;
    if constexpr (std::is_same_v<typename Key::value_type, math::Quat>) return math::Slerp(first.value, second.value, alpha);
    else return math::Lerp(first.value, second.value, alpha);
}

float normalized_time(float time, float duration) noexcept {
    if (duration <= math::Epsilon) return 0.0f;
    return std::clamp(time, 0.0f, duration);
}

}

bool Skeleton::valid() const noexcept {
    if (parents.size() != inverseBindMatrices.size() || (!bindPose.empty() && bindPose.size() != parents.size())) return false;
    if (!names.empty() && names.size() != parents.size()) return false;
    for (std::size_t index = 0; index < parents.size(); ++index) {
        if (parents[index] != InvalidBone && (parents[index] >= parents.size() || parents[index] >= index)) return false;
    }
    return true;
}

BoneIndex Skeleton::find_bone(std::string_view name) const noexcept {
    for (std::size_t index = 0; index < names.size(); ++index) if (names[index] == name) return static_cast<BoneIndex>(index);
    return InvalidBone;
}

void Pose::resize(std::size_t boneCount) {
    local.resize(boneCount);
    model.resize(boneCount, math::Mat4::Identity());
}

void Pose::reset_to_bind_pose(const Skeleton& skeleton) {
    resize(skeleton.bone_count());
    if (skeleton.bindPose.size() == skeleton.bone_count()) local = skeleton.bindPose;
    else std::fill(local.begin(), local.end(), math::Transform{});
    std::fill(model.begin(), model.end(), math::Mat4::Identity());
}

bool Pose::compatible(const Skeleton& skeleton) const noexcept { return skeleton.valid() && local.size() == skeleton.bone_count() && model.size() == skeleton.bone_count(); }

bool AnimationClip::valid_for(const Skeleton& skeleton) const noexcept {
    return skeleton.valid() && tracks_.size() == skeleton.bone_count() && duration_ >= 0.0f;
}

void AnimationClip::sample(const Skeleton& skeleton, float time, Pose& output,
                           std::vector<SampleCursor>& cursors) const noexcept {
    if (!valid_for(skeleton) || !output.compatible(skeleton) || cursors.size() != tracks_.size()) return;
    const float sampleTime = normalized_time(time, duration_);
    for (std::size_t bone = 0; bone < tracks_.size(); ++bone) {
        const auto& trackData = tracks_[bone];
        auto& cursor = cursors[bone];
        if (sampleTime < cursor.lastTime) cursor.reset();
        output.local[bone].position = sample_key(trackData.positions, sampleTime, cursor.position, output.local[bone].position);
        output.local[bone].rotation = math::Normalize(sample_key(trackData.rotations, sampleTime, cursor.rotation, output.local[bone].rotation));
        output.local[bone].scale = sample_key(trackData.scales, sampleTime, cursor.scale, output.local[bone].scale);
        cursor.lastTime = sampleTime;
    }
    for (std::size_t bone = 0; bone < skeleton.bone_count(); ++bone) {
        const auto parent = skeleton.parents[bone];
        output.model[bone] = parent == InvalidBone ? math::TransformMatrix(output.local[bone]) : output.model[parent] * math::TransformMatrix(output.local[bone]);
    }
}

bool Animator::set_skeleton(const Skeleton& skeleton) noexcept {
    if (!skeleton.valid()) return false;
    skeleton_ = &skeleton;
    pose_.reset_to_bind_pose(skeleton);
    cursors_.assign(skeleton.bone_count(), SampleCursor{});
    if (clip_ && !clip_->valid_for(skeleton)) clip_ = nullptr;
    sample_current();
    return true;
}

bool Animator::set_clip(const AnimationClip* clip) noexcept {
    if (clip && (!skeleton_ || !clip->valid_for(*skeleton_))) return false;
    clip_ = clip;
    time_ = 0.0f;
    forward_ = true;
    for (auto& cursor : cursors_) cursor.reset();
    sample_current();
    return true;
}

void Animator::sample_current() noexcept { if (clip_ && skeleton_) clip_->sample(*skeleton_, time_, pose_, cursors_); }

void Animator::stop() noexcept { time_ = 0.0f; forward_ = true; playing_ = false; for (auto& cursor : cursors_) cursor.reset(); sample_current(); }

void Animator::seek(float time) noexcept {
    time_ = skeleton_ && clip_ ? normalized_time(time, clip_->duration()) : 0.0f;
    for (auto& cursor : cursors_) cursor.reset();
    sample_current();
}

void Animator::update(float deltaSeconds) noexcept {
    if (!playing_ || !clip_ || !skeleton_ || !math::IsFinite(deltaSeconds) || clip_->duration() <= math::Epsilon) return;
    const float delta = std::abs(deltaSeconds * speed_);
    if (mode_ == PlaybackMode::Once) {
        time_ = std::clamp(time_ + (speed_ >= 0.0f ? delta : -delta), 0.0f, clip_->duration());
        if (time_ == 0.0f || time_ == clip_->duration()) playing_ = false;
    } else if (mode_ == PlaybackMode::Loop) {
        const float direction = speed_ >= 0.0f ? 1.0f : -1.0f;
        time_ = std::fmod(time_ + direction * delta, clip_->duration());
        if (time_ < 0.0f) time_ += clip_->duration();
    } else {
        const float direction = forward_ ? 1.0f : -1.0f;
        time_ += direction * delta;
        while (time_ > clip_->duration() || time_ < 0.0f) {
            if (time_ > clip_->duration()) { time_ = clip_->duration() - (time_ - clip_->duration()); forward_ = false; }
            else { time_ = -time_; forward_ = true; }
        }
    }
    sample_current();
}

void blend(const Skeleton& skeleton, const Pose& a, const Pose& b, float weight, Pose& output) noexcept {
    if (!skeleton.valid() || a.local.size() != b.local.size() || output.local.size() != a.local.size() || output.local.size() != skeleton.bone_count()) return;
    const float alpha = math::Clamp01(weight);
    for (std::size_t index = 0; index < a.local.size(); ++index) output.local[index] = math::Blend(a.local[index], b.local[index], alpha);
    if (output.model.size() != output.local.size()) output.model.resize(output.local.size(), math::Mat4::Identity());
    for (std::size_t index = 0; index < output.local.size(); ++index) {
        const auto parent = skeleton.parents[index];
        const auto localMatrix = math::TransformMatrix(output.local[index]);
        output.model[index] = parent == InvalidBone ? localMatrix : output.model[parent] * localMatrix;
    }
}

void build_skin_matrices(const Skeleton& skeleton, const Pose& pose, std::vector<math::Mat4>& output) noexcept {
    if (!pose.compatible(skeleton)) return;
    output.resize(skeleton.bone_count());
    for (std::size_t index = 0; index < skeleton.bone_count(); ++index) output[index] = pose.model[index] * skeleton.inverseBindMatrices[index];
}

void AnimationSystem::add(Animator& animator) {
    if (std::find(animators_.begin(), animators_.end(), &animator) == animators_.end()) animators_.push_back(&animator);
}

void AnimationSystem::remove(Animator& animator) noexcept {
    animators_.erase(std::remove(animators_.begin(), animators_.end(), &animator), animators_.end());
}

void AnimationSystem::update(float deltaSeconds) noexcept { for (auto* animator : animators_) if (animator) animator->update(deltaSeconds); }

}

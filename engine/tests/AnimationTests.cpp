#include "shinkou/animation/Animation.h"
#include <cmath>
#include <iostream>

namespace {
bool near(float lhs, float rhs, float epsilon = 0.0002f) { return std::abs(lhs - rhs) <= epsilon; }
bool near_vec(shinkou::math::Vec3 lhs, shinkou::math::Vec3 rhs) {
    return near(lhs.x, rhs.x) && near(lhs.y, rhs.y) && near(lhs.z, rhs.z);
}
}

int main() {
    using namespace shinkou;
    using namespace shinkou::animation;

    Skeleton skeleton;
    skeleton.parents = {InvalidBone, 0};
    skeleton.bindPose = {math::Transform{}, math::Transform{math::Vec3{1.0f, 0.0f, 0.0f}, math::Quat{}, math::Vec3{1, 1, 1}}};
    skeleton.inverseBindMatrices = {math::Mat4::Identity(), math::Mat4::Identity()};
    skeleton.names = {"root", "hand"};
    if (!skeleton.valid() || skeleton.find_bone("hand") != 1 || skeleton.find_bone("missing") != InvalidBone) return 1;

    AnimationClip clip(2);
    clip.set_name("move");
    clip.set_duration(1.0f);
    clip.track(0).positions = {{0.0f, {0, 0, 0}}, {1.0f, {10, 0, 0}}};
    clip.track(0).rotations = {{0.0f, math::Quat{}}, {1.0f, math::Quat{}}};
    clip.track(1).positions = {{0.0f, {1, 0, 0}}, {1.0f, {3, 0, 0}}};
    if (!clip.valid_for(skeleton)) return 2;

    Animator animator(skeleton);
    if (!animator.set_clip(&clip)) return 3;
    animator.seek(0.5f);
    if (!near(animator.pose().local[0].position.x, 5.0f) || !near(animator.pose().local[1].position.x, 2.0f)) return 4;
    if (!near(animator.pose().model[1].m[12], 7.0f)) return 5;
    const auto first = math::Slerp(math::Quat{}, math::FromAxisAngle({0, 1, 0}, math::Pi), 0.5f);
    if (!near(std::abs(math::LengthSquared(first)), 1.0f)) return 6;

    animator.set_playback_mode(PlaybackMode::Loop);
    animator.play();
    animator.update(0.75f);
    if (!near(animator.time(), 0.25f)) return 7;
    animator.set_playback_mode(PlaybackMode::Once);
    animator.seek(0.9f);
    animator.update(0.2f);
    if (animator.playing() || !near(animator.time(), 1.0f)) return 8;

    Pose blended;
    blended.reset_to_bind_pose(skeleton);
    Pose second = animator.pose();
    second.local[0].position = {20, 0, 0};
    blend(skeleton, animator.pose(), second, 0.5f, blended);
    if (!near(blended.local[0].position.x, 15.0f) || !near(blended.model[1].m[12], 18.0f)) return 9;

    std::vector<math::Mat4> skin;
    build_skin_matrices(skeleton, blended, skin);
    if (skin.size() != 2 || !near(skin[1].m[12], 18.0f)) return 10;

    AnimationSystem system;
    system.add(animator);
    system.add(animator);
    if (system.size() != 1) return 11;
    system.remove(animator);
    if (system.size() != 0) return 12;

    std::cout << "animation tests passed\n";
    return 0;
}

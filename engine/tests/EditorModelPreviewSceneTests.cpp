#include "shinkou/editor/EditorModelPreviewScene.h"

#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

namespace {

std::shared_ptr<const shinkou::editor::EditorModelPreviewSnapshot> animated_snapshot() {
    auto snapshot = std::make_shared<shinkou::editor::EditorModelPreviewSnapshot>();
    snapshot->revision = 43;
    snapshot->vertexCount = 3;
    snapshot->triangleCount = 1;
    snapshot->minX = -1.0f;
    snapshot->minY = -1.0f;
    snapshot->maxX = 1.0f;
    snapshot->maxY = 1.0f;
    snapshot->vertices = std::make_shared<const std::vector<shinkou::math::Vec3>>(
        std::initializer_list<shinkou::math::Vec3>{{-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}});
    snapshot->indices = std::make_shared<const std::vector<std::uint32_t>>(
        std::initializer_list<std::uint32_t>{0, 1, 2});
    snapshot->wireSegments = std::make_shared<const std::vector<shinkou::ui::Vec2>>(
        std::initializer_list<shinkou::ui::Vec2>{{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {0.5f, 0.0f},
                                                  {0.5f, 0.0f}, {0.0f, 1.0f}});
    auto skeleton = std::make_shared<shinkou::animation::Skeleton>();
    skeleton->parents = {shinkou::animation::InvalidBone};
    skeleton->bindPose = {shinkou::math::Transform{}};
    skeleton->inverseBindMatrices = {shinkou::math::Mat4::Identity()};
    skeleton->names = {"root"};
    snapshot->animationSkeleton = skeleton;
    snapshot->vertexBones = std::make_shared<const std::vector<shinkou::animation::BoneIndex>>(
        std::initializer_list<shinkou::animation::BoneIndex>{0, 0, 0});
    shinkou::animation::AnimationClip clip(1);
    clip.set_name("Spin");
    clip.set_duration(1.0f);
    clip.track(0).rotations = {{0.0f, shinkou::math::Quat{}},
                               {1.0f, shinkou::math::FromAxisAngle({0.0f, 0.0f, 1.0f}, shinkou::math::Pi * 0.5f)}};
    auto animation = shinkou::editor::EditorModelAnimationPreview{};
    animation.name = "Spin";
    animation.duration = 1.0f;
    animation.playableChannelCount = 1;
    animation.cpuPlayable = true;
    animation.cpuClip = std::make_shared<const shinkou::animation::AnimationClip>(std::move(clip));
    snapshot->animations = std::make_shared<const std::vector<shinkou::editor::EditorModelAnimationPreview>>(
        std::initializer_list<shinkou::editor::EditorModelAnimationPreview>{std::move(animation)});
    return snapshot;
}

} // namespace

int main() {
    auto snapshot = std::make_shared<shinkou::editor::EditorModelPreviewSnapshot>();
    snapshot->revision = 42;
    snapshot->vertexCount = 3;
    snapshot->triangleCount = 1;
    snapshot->minX = -1.0f;
    snapshot->minY = -1.0f;
    snapshot->minZ = 0.0f;
    snapshot->maxX = 1.0f;
    snapshot->maxY = 1.0f;
    snapshot->maxZ = 0.0f;
    snapshot->vertices = std::make_shared<const std::vector<shinkou::math::Vec3>>(
        std::initializer_list<shinkou::math::Vec3>{{-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}});
    snapshot->indices = std::make_shared<const std::vector<std::uint32_t>>(
        std::initializer_list<std::uint32_t>{0, 1, 2});
    snapshot->wireSegments = std::make_shared<const std::vector<shinkou::ui::Vec2>>(
        std::initializer_list<shinkou::ui::Vec2>{{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {0.5f, 0.0f},
                                                  {0.5f, 0.0f}, {0.0f, 1.0f}});
    assert(snapshot->valid());

    shinkou::editor::EditorModelPreviewScene scene;
    scene.set_snapshot(snapshot);
    const auto initial = scene.state();
    assert(initial && initial->valid() && initial->geometryRevision == 42);
    assert(scene.snapshot() == snapshot);
    const auto initialProjection = initial->projectionRevision;
    const auto initialYaw = initial->camera.yaw;

    scene.orbit({30.0f, -12.0f});
    const auto orbited = scene.state();
    assert(orbited && orbited->valid());
    assert(orbited->projectionRevision != initialProjection);
    assert(orbited->camera.yaw != initialYaw);
    assert(scene.snapshot() == snapshot);

    scene.zoom(1000.0f);
    assert(scene.camera().distance >= 0.75f && scene.camera().valid());
    scene.zoom(-1000.0f);
    assert(scene.camera().distance <= 8.0f && scene.camera().valid());
    scene.reset_camera();
    const auto reset = scene.state();
    assert(reset && reset->valid());
    assert(reset->camera.yaw == initial->camera.yaw && reset->camera.pitch == initial->camera.pitch &&
           reset->camera.distance == initial->camera.distance);
    scene.clear();
    assert(!scene.state() && !scene.snapshot());

    scene.set_snapshot(animated_snapshot());
    const auto animatedInitial = scene.state();
    assert(animatedInitial && animatedInitial->animationPlayable &&
           animatedInitial->animationIndex == 0 && animatedInitial->animationPlaying &&
           animatedInitial->animationDuration == 1.0f);
    const auto initialAnimationProjection = animatedInitial->projectionRevision;
    scene.advance_animation(0.5f);
    const auto animatedHalf = scene.state();
    assert(animatedHalf && animatedHalf->animationTime > 0.49f && animatedHalf->animationTime < 0.51f &&
           animatedHalf->projectionRevision != initialAnimationProjection);
    scene.pause_animation();
    assert(scene.state() && !scene.state()->animationPlaying);
    scene.seek_animation(0.25f);
    assert(scene.state() && scene.state()->animationTime > 0.24f && scene.state()->animationTime < 0.26f);
    scene.toggle_animation();
    assert(scene.state() && scene.state()->animationPlaying);
    std::cout << "Editor model preview scene passed\n";
    return 0;
}

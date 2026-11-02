// Contract tests for the chunk transform pipeline: depth-ordered parent
// propagation, LocalToWorld storage, authoring mirroring and job parallelism.
#include "shinkou/World.h"
#include "shinkou/jobs/JobSystem.h"

#include <cmath>
#include <cstdio>

namespace {

using shinkou::GameObject;
using shinkou::ObjectStorage;
using shinkou::World;

int fail(const char* what, int code) {
    std::printf("FAIL %s (code %d)\n", what, code);
    return code;
}

bool near(float lhs, float rhs) { return std::abs(lhs - rhs) < 0.001f; }

// Translation column of a world matrix.
bool translation_is(const shinkou::math::Mat4& matrix, float x, float y, float z) {
    return near(matrix.m[12], x) && near(matrix.m[13], y) && near(matrix.m[14], z);
}

const shinkou::math::Mat4& world_matrix(World& world, GameObject& object) {
    return object.get_component<shinkou::components::TransformComponent>()->world_matrix();
}

} // namespace

int main() {
    // ---- 1. ECS parent -> ECS child --------------------------------------
    {
        World world;
        auto& parent = world.create_object("parent", ObjectStorage::Ecs);
        parent.get_component<shinkou::components::TransformComponent>()->set_position({10.0f, 0.0f, 0.0f});
        auto& child = parent.create_child("child", ObjectStorage::Ecs);
        child.get_component<shinkou::components::TransformComponent>()->set_position({0.0f, 5.0f, 0.0f});

        if (child.hierarchy_depth() != 1u) return fail("child depth", 1);
        if (!world.ecs().has<shinkou::ecs::LocalToWorld>(parent.ecs_entity())) return fail("parent chunk matrix", 2);
        if (!world.ecs().has<shinkou::ecs::LocalToWorld>(child.ecs_entity())) return fail("child chunk matrix", 3);

        world.update(0.0f);

        const auto& parentWorld = world.ecs().get<shinkou::ecs::LocalToWorld>(parent.ecs_entity()).matrix;
        const auto& childWorld = world.ecs().get<shinkou::ecs::LocalToWorld>(child.ecs_entity()).matrix;
        if (!translation_is(parentWorld, 10.0f, 0.0f, 0.0f)) return fail("parent world", 4);
        if (!translation_is(childWorld, 10.0f, 5.0f, 0.0f)) return fail("child world", 5);
        if (!translation_is(world_matrix(world, child), 10.0f, 5.0f, 0.0f)) return fail("authoring mirror", 6);
    }

    // ---- 2. reparent back to the root ------------------------------------
    {
        World world;
        auto& parent = world.create_object("parent", ObjectStorage::Ecs);
        parent.get_component<shinkou::components::TransformComponent>()->set_position({10.0f, 0.0f, 0.0f});
        auto& child = parent.create_child("child", ObjectStorage::Ecs);
        child.get_component<shinkou::components::TransformComponent>()->set_position({0.0f, 5.0f, 0.0f});
        world.update(0.0f);

        child.set_parent(nullptr);
        if (child.hierarchy_depth() != 0u) return fail("reparen depth", 7);
        world.update(0.0f);
        const auto& childWorld = world.ecs().get<shinkou::ecs::LocalToWorld>(child.ecs_entity()).matrix;
        if (!translation_is(childWorld, 0.0f, 5.0f, 0.0f)) return fail("reparen world", 8);
    }

    // ---- 3. regular parent -> ECS child ----------------------------------
    {
        World world;
        auto& parent = world.create_object("regular-parent");
        parent.get_component<shinkou::components::TransformComponent>()->set_position({3.0f, 0.0f, 0.0f});
        auto& child = parent.create_child("ecs-child", ObjectStorage::Ecs);
        child.get_component<shinkou::components::TransformComponent>()->set_position({1.0f, 0.0f, 0.0f});
        world.update(0.0f);
        const auto& childWorld = world.ecs().get<shinkou::ecs::LocalToWorld>(child.ecs_entity()).matrix;
        if (!translation_is(childWorld, 4.0f, 0.0f, 0.0f)) return fail("mixed regular->ecs", 9);
    }

    // ---- 4. ECS parent -> regular child ----------------------------------
    {
        World world;
        auto& parent = world.create_object("ecs-parent", ObjectStorage::Ecs);
        parent.get_component<shinkou::components::TransformComponent>()->set_position({2.0f, 0.0f, 0.0f});
        auto& child = parent.create_child("regular-child");
        child.get_component<shinkou::components::TransformComponent>()->set_position({0.0f, 1.0f, 0.0f});
        world.update(0.0f);
        if (!translation_is(world_matrix(world, child), 2.0f, 1.0f, 0.0f)) return fail("mixed ecs->regular", 10);
    }

    // ---- 5. parallel path matches the serial path ------------------------
    {
        shinkou::jobs::JobSystem jobs(4);
        int mismatches = 0;
        for (int mode = 0; mode < 2; ++mode) {
            World world;
            if (mode == 1) world.set_job_system(&jobs);
            GameObject* parent = nullptr;
            for (int row = 0; row < 200; ++row) {
                auto& object = parent ? parent->create_child("node", ObjectStorage::Ecs)
                                      : world.create_object("node", ObjectStorage::Ecs);
                object.get_component<shinkou::components::TransformComponent>()->set_position(
                    {1.0f, 0.5f, 0.25f});
                parent = &object;
            }
            world.update(0.0f);
            float expected = 0.0f;
            for (float step = 0.0f; step < 200.0f; step += 1.0f) expected += 1.0f;
            auto* leaf = parent;
            const auto& leafWorld = world.ecs().get<shinkou::ecs::LocalToWorld>(leaf->ecs_entity()).matrix;
            if (!near(leafWorld.m[12], expected)) ++mismatches;
            if (!near(leafWorld.m[13], 0.5f * 200.0f)) ++mismatches;
        }
        if (mismatches != 0) return fail("parallel equals serial", 11);
    }

    // ---- 6. inactive subtree is not recomputed ---------------------------
    {
        World world;
        auto& parent = world.create_object("parent", ObjectStorage::Ecs);
        parent.get_component<shinkou::components::TransformComponent>()->set_position({7.0f, 0.0f, 0.0f});
        auto& child = parent.create_child("child", ObjectStorage::Ecs);
        child.get_component<shinkou::components::TransformComponent>()->set_position({0.0f, 2.0f, 0.0f});
        world.update(0.0f);

        parent.set_active(false);
        parent.get_component<shinkou::components::TransformComponent>()->set_position({100.0f, 0.0f, 0.0f});
        world.update(0.0f);
        const auto& childWorld = world.ecs().get<shinkou::ecs::LocalToWorld>(child.ecs_entity()).matrix;
        if (!translation_is(childWorld, 7.0f, 2.0f, 0.0f)) return fail("inactive keeps last matrix", 12);

        parent.set_active(true);
        world.update(0.0f);
        const auto& refreshed = world.ecs().get<shinkou::ecs::LocalToWorld>(child.ecs_entity()).matrix;
        if (!translation_is(refreshed, 100.0f, 2.0f, 0.0f)) return fail("reactivated recomputes", 13);
    }

    // Typed chunk systems participate in World updates even when entities do
    // not have authoring objects. Legacy callbacks observe the joined result.
    {
        struct Value { int n{0}; };
        struct Copy { int n{0}; };
        shinkou::jobs::JobSystem jobs(2);
        World world;
        world.set_job_system(&jobs);
        auto& store = world.ecs().store();
        const auto e = store.create(store.archetype<Value, Copy>());
        world.chunk_pipeline().add<Value>(store.query<Value>(), [](auto& c, Value* values) {
            for (unsigned i = 0; i < c.count; ++i) ++values[i].n;
        });
        world.chunk_pipeline().add<const Value, Copy>(store.query<Value, Copy>(),
            [](auto& c, const Value* values, Copy* copies) {
                for (unsigned i = 0; i < c.count; ++i) copies[i].n = values[i].n;
            });
        bool callbackObserved = false;
        world.add_system([&](auto&, auto) { callbackObserved = store.get<Copy>(e).n == 1; });
        world.update(0);
        if (!callbackObserved) return fail("World joins typed chunk systems before legacy callbacks", 14);
        world.chunk_pipeline().clear();
    }

    std::printf("world transform tests passed\n");
    return 0;
}

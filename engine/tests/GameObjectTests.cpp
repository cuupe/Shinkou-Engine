#include "shinkou/World.h"
#include <cmath>
#include <iostream>

namespace {
struct LifecycleCounters {
    int updates{0};
    int creates{0};
    int attaches{0};
    int enables{0};
    int disables{0};
    int destroyRequests{0};
    int destroys{0};
};

struct UpdateComponent final : shinkou::Component {
    LifecycleCounters* counters;
    explicit UpdateComponent(LifecycleCounters* value = nullptr) : counters(value) {}
    std::string_view type_name() const noexcept override { return "Update"; }
protected:
    void on_create() override { if (counters) ++counters->creates; }
    void on_attach() override { if (counters) ++counters->attaches; }
    void on_enable() override { if (counters) ++counters->enables; }
    void on_disable() override { if (counters) ++counters->disables; }
    void on_destroy_requested() override { if (counters) ++counters->destroyRequests; }
    void on_destroy() override { if (counters) ++counters->destroys; }
    void on_update(shinkou::Seconds) override { if (counters) ++counters->updates; }
};

bool near(float lhs, float rhs) { return std::abs(lhs - rhs) < 0.0001f; }
}

int main() {
    shinkou::World world;
    auto& parent = world.create_object("parent");
    auto& child = parent.create_child("child");
    if (world.object_count() != 2 || child.parent() != &parent || parent.children().size() != 1) return 1;

    LifecycleCounters counters;
    auto* update = child.add_component<UpdateComponent>(&counters);
    if (!update || child.add_component<UpdateComponent>() != nullptr) return 2;
    const auto updateHandle = update->handle();
    if (!updateHandle || counters.creates != 1 || counters.attaches != 1 || counters.enables != 1) return 15;
    auto* transform = child.get_component<shinkou::components::TransformComponent>();
    transform->set_position({2.0f, 0.0f, 0.0f});
    parent.get_component<shinkou::components::TransformComponent>()->set_position({3.0f, 0.0f, 0.0f});
    world.update(0.25f);
    if (counters.updates != 1 || !near(transform->world_matrix().m[12], 5.0f)) return 3;

    parent.set_active(false);
    world.update(0.25f);
    if (parent.active_in_hierarchy() || child.active_in_hierarchy() || counters.updates != 1) return 4;
    parent.set_active(true);
    if (!child.active_in_hierarchy()) return 5;
    if (counters.disables != 1 || counters.enables != 2) return 16;

    if (!child.set_property("name", std::string{"edited-child"}) || child.name() != "edited-child") return 17;
    if (!transform->set_property("position", shinkou::math::Vec3{4.0f, 0.0f, 0.0f})) return 18;
    if (transform->set_property("position", std::string{"invalid"})) return 19;

    auto* lifetime = child.add_component<shinkou::components::LifetimeComponent>();
    lifetime->remaining = 0.1f;
    world.update(0.2f);
    if (world.find_object(child.id()) != nullptr || world.object_count() != 1 || updateHandle.valid()) return 6;
    if (counters.destroyRequests != 1 || counters.destroys != 1) return 20;

    auto& ecsObject = world.create_object("ecs");
    const auto entity = ecsObject.enable_ecs();
    if (!entity || !world.ecs().valid(entity) || world.find_object(entity) != &ecsObject) return 7;
    ecsObject.add_ecs_component<shinkou::components::TagComponent>();
    if (!ecsObject.get_ecs_component<shinkou::components::TagComponent>()) return 8;
    ecsObject.disable_ecs();
    if (ecsObject.has_ecs_entity() || world.ecs().valid(entity)) return 9;

    if (!world.register_component_type<UpdateComponent>("Update")) return 21;
    auto& factoryObject = world.create_object("factory");
    auto* factoryComponent = factoryObject.add_component("Update");
    if (!factoryComponent || factoryComponent->type_name() != "Update") return 22;
    if (factoryObject.add_component("Missing") != nullptr) return 23;
    if (factoryObject.properties().empty() || !world.find_component(factoryComponent->id())) return 24;

    const auto staleEntity = world.ecs().create();
    world.ecs().emplace<int>(staleEntity, 42);
    world.ecs().destroy(staleEntity);
    world.ecs().destroy(staleEntity);
    const auto recycledEntity = world.ecs().create();
    if (recycledEntity.id != staleEntity.id || recycledEntity.generation == staleEntity.generation ||
        world.ecs().valid(staleEntity) || !world.ecs().valid(recycledEntity) || world.ecs().try_get<int>(recycledEntity)) return 13;

    const auto nativeEntity = world.ecs().create();
    world.ecs().emplace<int>(nativeEntity, 42);
    auto& nativeRegistry = world.ecs().native_registry();
    const auto nativeView = nativeRegistry.view<int>();
    if (nativeView.size() != 1 || nativeRegistry.get<int>(nativeView.front()) != 42) return 25;

    auto& reparented = world.create_object("reparented");
    reparented.set_parent(&ecsObject);
    if (reparented.parent() != &ecsObject || world.object_count() != 4) return 10;
    reparented.set_parent(nullptr);
    if (reparented.parent() != nullptr || world.object_count() != 4) return 11;

    world.clear_objects();
    if (world.object_count() != 0 || world.ecs().valid(recycledEntity)) return 12;
    std::cout << "game object tests passed\n";
    return 0;
}

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

struct Position {
    float value{0.0f};
};

struct Velocity {
    float value{0.0f};
};

struct CountingEcsSystem final : shinkou::EcsSystem {
    int creates{0};
    int enables{0};
    int disables{0};
    int updates{0};
    int destroys{0};
    int order() const noexcept override { return 7; }
protected:
    void on_create() override { ++creates; }
    void on_enable() override { ++enables; }
    void on_disable() override { ++disables; }
    void on_update(shinkou::Seconds) override { ++updates; }
    void on_destroy() override { ++destroys; }
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

    auto& ecsManaged = world.create_object("ecs-managed", shinkou::ObjectStorage::Ecs);
    if (ecsManaged.storage() != shinkou::ObjectStorage::Ecs || !ecsManaged.has_ecs_entity()) return 26;
    auto* ecsTransform = ecsManaged.get_component<shinkou::components::TransformComponent>();
    if (!ecsTransform || ecsTransform->storage() != shinkou::ObjectStorage::Ecs ||
        ecsManaged.get_ecs_component<shinkou::components::TransformComponent>() != ecsTransform) return 27;
    LifecycleCounters ecsCounters;
    auto* ecsUpdate = ecsManaged.add_component<UpdateComponent>(&ecsCounters);
    if (!ecsUpdate || ecsUpdate->storage() != shinkou::ObjectStorage::Ecs ||
        !world.ecs().has<UpdateComponent>(ecsManaged.ecs_entity()) ||
        ecsManaged.get_ecs_component<UpdateComponent>() != ecsUpdate) return 28;
    auto& ecsSystem = world.add_ecs_system<CountingEcsSystem>();
    if (ecsSystem.creates != 1 || ecsSystem.enables != 1 || world.ecs_system_count() != 1) return 29;
    world.update(0.01f);
    if (ecsCounters.updates != 1 || ecsSystem.updates != 1) return 30;
    ecsSystem.set_enabled(false);
    world.update(0.01f);
    if (ecsSystem.updates != 1 || ecsSystem.disables != 1) return 31;
    ecsSystem.set_enabled(true);
    if (ecsSystem.enables != 2) return 32;
    if (!ecsManaged.remove_component<UpdateComponent>() || world.ecs().has<UpdateComponent>(ecsManaged.ecs_entity()) ||
        ecsCounters.destroys != 1) return 33;
    LifecycleCounters regularPipelineCounters;
    auto& regularPipelineObject = world.create_object("regular-pipeline");
    if (!regularPipelineObject.add_component<UpdateComponent>(&regularPipelineCounters)) return 39;
    auto& ecsPipelineObject = world.create_object("ecs-pipeline", shinkou::ObjectStorage::Ecs);
    LifecycleCounters ecsPipelineCounters;
    if (!ecsPipelineObject.add_component<UpdateComponent>(&ecsPipelineCounters)) return 40;
    world.update(0.01f);
    if (regularPipelineCounters.updates != 1 || ecsPipelineCounters.updates != 1) return 41;
    const auto stats = world.statistics();
    if (stats.objects != world.object_count() || stats.ecsObjects != world.ecs_object_count() ||
        stats.components != world.component_count() || stats.updates != 6) return 34;
    std::size_t flatObjects = 0;
    world.each_game_object([&](const shinkou::GameObject&) { ++flatObjects; });
    if (flatObjects != world.object_count()) return 35;
    world.optimize();

    const auto deferredEntity = world.ecs().create();
    world.ecs().destroy_deferred(deferredEntity);
    if (!world.ecs().valid(deferredEntity) || world.ecs().pending_destroy_count() != 1) return 36;
    world.update(0.0f);
    if (world.ecs().valid(deferredEntity) || world.ecs().pending_destroy_count() != 0) return 37;

    auto& externallyDestroyed = world.create_object("external-destroy", shinkou::ObjectStorage::Ecs);
    LifecycleCounters externalCounters;
    auto* externalComponent = externallyDestroyed.add_component<UpdateComponent>(&externalCounters);
    const auto externalHandle = externalComponent->handle();
    const auto externallyDestroyedEntity = externallyDestroyed.ecs_entity();
    world.ecs().destroy(externallyDestroyedEntity);
    if (world.find_object(externallyDestroyed.id()) != nullptr || externalHandle.valid() || externalCounters.destroys != 1 ||
        world.ecs().valid(externallyDestroyedEntity)) return 38;

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

    const auto viewEntity = world.ecs().create();
    world.ecs().emplace<Position>(viewEntity, Position{1.0f});
    world.ecs().emplace<Velocity>(viewEntity, Velocity{2.0f});
    std::size_t viewHits = 0;
    world.ecs().view<Position, Velocity>().each([&](auto, Position& position, Velocity& velocity) {
        position.value += velocity.value;
        ++viewHits;
    });
    if (viewHits != 1 || world.ecs().get<Position>(viewEntity).value != 3.0f) return 42;
    world.ecs().destroy(viewEntity);

    auto& reparented = world.create_object("reparented");
    reparented.set_parent(&ecsObject);
    if (reparented.parent() != &ecsObject || world.object_count() != 7) return 10;
    reparented.set_parent(nullptr);
    if (reparented.parent() != nullptr || world.object_count() != 7) return 11;

    world.clear_objects();
    if (world.object_count() != 0 || world.ecs().valid(recycledEntity) || ecsSystem.destroys != 0) return 12;
    std::cout << "game object tests passed\n";
    return 0;
}

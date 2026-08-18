#include "shinkou/World.h"
#include "shinkou/reflection/Reflection.h"
#include "shinkou/reflection/Serialization.h"
#include "shinkou/reflection/EngineReflection.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <chrono>
#include <limits>
#include <thread>
#include <vector>

enum class ReflectionState { Idle = 0, Active = 1, Done = 2 };
struct ReflectionPayload {
    std::int32_t count{0};
    std::string label;
    shinkou::math::Vec3 position{};
    ReflectionState state{ReflectionState::Idle};
    std::vector<float> samples;
};
struct ReflectionLeft { std::int32_t left{0}; };
struct ReflectionRight { std::int32_t right{0}; };
struct ReflectionMultiple : ReflectionLeft, ReflectionRight { std::int32_t own{0}; };

namespace {
struct ReflectionBase { virtual ~ReflectionBase() = default; std::int32_t id{0}; };
struct ReflectionActor final : ReflectionBase {
    std::string name{"actor"};
    float health{100.0f};
    std::int32_t damage{0};
    void apply_damage(std::int32_t amount) { damage += amount; health -= static_cast<float>(amount); }
    void scale_health(float& scale) { health *= scale; }
    std::int32_t read_damage() const noexcept { return damage; }
};
}

namespace shinkou::reflection {
SHINKOU_REFLECT_TYPE(ReflectionBase, "tests.ReflectionBase");
SHINKOU_REFLECT_TYPE(ReflectionActor, "tests.ReflectionActor");
SHINKOU_REFLECT_TYPE(::ReflectionState, "tests.ReflectionState");
SHINKOU_REFLECT_TYPE(::ReflectionPayload, "tests.ReflectionPayload");
SHINKOU_REFLECT_TYPE(std::vector<float>, "std.vector.float");
SHINKOU_REFLECT_TYPE(::ReflectionLeft, "tests.ReflectionLeft");
SHINKOU_REFLECT_TYPE(::ReflectionRight, "tests.ReflectionRight");
SHINKOU_REFLECT_TYPE(::ReflectionMultiple, "tests.ReflectionMultiple");
}

int main() {
    shinkou::World world;
    const auto types = world.component_types();
    if (types.size() < 3) return 1;
    auto& object = world.create_object("reflection");
    auto* lifetime = object.add_component("Lifetime");
    if (!lifetime || lifetime->properties().empty()) return 2;
    if (!lifetime->set_property("remaining", 2.0)) return 3;
    if (!object.set_property("name", std::string{"edited"}) || object.name() != "edited") return 4;

    using namespace shinkou::reflection;
    TypeRegistry registry;
    auto base = TypeBuilder<ReflectionBase>("tests.ReflectionBase", TypeKind::Object)
        .field("id", &ReflectionBase::id).take();
    auto actor = TypeBuilder<ReflectionActor>("tests.ReflectionActor", TypeKind::Object, TypeFlags::Serializable)
        .base<ReflectionBase>()
        .field("name", &ReflectionActor::name)
        .field("health", &ReflectionActor::health)
        .field("damage", &ReflectionActor::damage, FieldFlags::ReadOnly)
        .method<&ReflectionActor::apply_damage>("apply_damage")
        .method<&ReflectionActor::scale_health>("scale_health")
        .method<&ReflectionActor::read_damage>("read_damage")
        .take();
    std::string error;
    if (!registry.register_type(std::move(actor), &error) ||
        !registry.register_type(std::move(base), &error) || !registry.freeze(&error)) {
        std::cerr << "reflection registry setup failed: " << error << '\n';
        return 5;
    }
    const auto* descriptor = registry.find("tests.ReflectionActor");
    if (!descriptor || registry.find_field(descriptor->id, "id") == nullptr ||
        registry.find_method(descriptor->id, "read_damage") == nullptr) return 6;
    ReflectionActor value;
    const auto* inheritedId = registry.find_field(descriptor->id, "id");
    auto* inheritedIdStorage = static_cast<std::int32_t*>(inheritedId->address_of(registry, descriptor->id, &value));
    if (!inheritedIdStorage) return 6;
    *inheritedIdStorage = 4;
    const std::int32_t amount = 20;
    Argument argument{type_id<std::int32_t>(), &amount};
    if (!descriptor->find_method("apply_damage")->call(&value, &argument, 1) || value.damage != 20) return 7;
    float scale = 0.5f;
    Argument scaleArgument{type_id<float>(), &scale};
    if (!descriptor->find_method("scale_health")->call(&value, &scaleArgument, 1) || value.health != 40.0f) return 8;
    std::int32_t readDamage = 0;
    if (!registry.invoke(descriptor->id, "read_damage", &value, nullptr, 0, &readDamage) || readDamage != value.damage) return 8;
    auto instance = ReflectedInstance::create(*descriptor);
    if (!instance) return 9;
    std::atomic<bool> failed{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) readers.emplace_back([&] {
        for (int j = 0; j < 5000; ++j) if (registry.find(descriptor->id) == nullptr) failed = true;
    });
    for (auto& reader : readers) reader.join();
    if (failed.load()) return 10;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i) {
        if (registry.find(descriptor->id) == nullptr) return 11;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "reflection lookup benchmark: " << elapsed << " us / 100000\n";
    TypeRegistry serializationRegistry;
    auto state = TypeBuilder<ReflectionState>("tests.ReflectionState", TypeKind::Enum)
        .enum_value("Idle", 0).enum_value("Active", 1).enum_value("Done", 2).take();
    auto samples = make_vector_descriptor<float>("std.vector.float");
    auto payload = TypeBuilder<ReflectionPayload>("tests.ReflectionPayload", TypeKind::Struct, TypeFlags::Serializable)
        .field("count", &ReflectionPayload::count)
        .field("label", &ReflectionPayload::label)
        .field("position", &ReflectionPayload::position)
        .field("state", &ReflectionPayload::state)
        .field("samples", &ReflectionPayload::samples).take();
    if (!serializationRegistry.register_type(std::move(state), &error) ||
        !serializationRegistry.register_type(std::move(samples), &error) ||
        !serializationRegistry.register_type(std::move(payload), &error) ||
        !serializationRegistry.freeze(&error)) return 12;
    ReflectionPayload source;
    source.count = 7;
    source.label = "payload";
    source.position = {1.0f, 2.0f, 3.0f};
    source.state = ReflectionState::Active;
    source.samples = {0.25f, 0.5f, 0.75f};
    std::string json;
    if (!serialize_json(serializationRegistry, type_id<ReflectionPayload>(), &source, json)) return 13;
    ReflectionPayload restored;
    if (!deserialize_json(serializationRegistry, type_id<ReflectionPayload>(), json, &restored) ||
        restored.count != source.count || restored.label != source.label ||
        restored.position.x != source.position.x || restored.state != source.state ||
        restored.samples.size() != 3 || restored.samples[2] != 0.75f) return 14;
    const std::uint64_t maxUnsigned = std::numeric_limits<std::uint64_t>::max();
    if (!serialize_json(serializationRegistry, type_id<std::uint64_t>(), &maxUnsigned, json)) return 14;
    std::uint64_t restoredUnsigned = 0;
    if (!deserialize_json(serializationRegistry, type_id<std::uint64_t>(), json, &restoredUnsigned) ||
        restoredUnsigned != maxUnsigned || deserialize_json(serializationRegistry, type_id<std::uint64_t>(), "01", &restoredUnsigned) ||
        deserialize_json(serializationRegistry, type_id<std::uint64_t>(), "+1", &restoredUnsigned)) return 14;
    auto ownedInstance = serializationRegistry.create_instance(type_id<ReflectionPayload>());
    TypeRegistry movedSerializationRegistry = std::move(serializationRegistry);
    if (!ownedInstance || movedSerializationRegistry.find(type_id<ReflectionPayload>()) == nullptr) return 15;
    TypeRegistry moduleRegistry;
    auto left = TypeBuilder<ReflectionLeft>("tests.ReflectionLeft").field("left", &ReflectionLeft::left).take();
    auto right = TypeBuilder<ReflectionRight>("tests.ReflectionRight").field("right", &ReflectionRight::right).take();
    auto multiple = TypeBuilder<ReflectionMultiple>("tests.ReflectionMultiple")
        .base<ReflectionLeft>().base<ReflectionRight>().field("own", &ReflectionMultiple::own).take();
    std::vector<TypeDescriptor> moduleTypes;
    moduleTypes.push_back(std::move(left));
    moduleTypes.push_back(std::move(right));
    moduleTypes.push_back(std::move(multiple));
    if (!moduleRegistry.register_module("tests.module", 1, std::move(moduleTypes), &error) ||
        moduleRegistry.module_count() != 1 || !moduleRegistry.freeze(&error)) return 15;
    ReflectionMultiple multipleValue;
    multipleValue.left = 1; multipleValue.right = 2; multipleValue.own = 3;
    const auto* multipleDescriptor = moduleRegistry.find(type_id<ReflectionMultiple>());
    if (!multipleDescriptor || moduleRegistry.find_field(multipleDescriptor->id, "right") == nullptr ||
        moduleRegistry.cast_object(multipleDescriptor->id, type_id<ReflectionRight>(), &multipleValue) != static_cast<ReflectionRight*>(&multipleValue)) return 16;
    TypeRegistry engineRegistry;
    if (!register_engine_types(engineRegistry, &error) || !engineRegistry.freeze(&error) ||
        engineRegistry.find("shinkou.components.Lifetime") == nullptr ||
        engineRegistry.find("shinkou.render.LightType")->find_enum_value("Spot") == nullptr) return 17;
    std::cout << "reflection tests passed\n";
    return 0;
}

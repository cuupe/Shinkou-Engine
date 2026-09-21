#include "shinkou/physics/PhysXPhysicsWorld.h"
#include "shinkou/physics/SimplePhysicsWorld.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <functional>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(SHINKOU_WITH_PHYSX)
#include <PxPhysicsAPI.h>
#include <extensions/PxDefaultCpuDispatcher.h>
#if defined(SHINKOU_WITH_PHYSX_EXTENSIONS)
#include <extensions/PxRigidBodyExt.h>
#endif
#if defined(SHINKOU_WITH_PHYSX_COOKING)
#include <cooking/PxCooking.h>
#endif
#endif

namespace shinkou::physics {

#if defined(SHINKOU_WITH_PHYSX)
namespace {

physx::PxVec3 to_px(math::Vec3 value) noexcept { return {value.x, value.y, value.z}; }
physx::PxQuat to_px(math::Quat value) noexcept {
    value = math::Normalize(value);
    return {value.x, value.y, value.z, value.w};
}
math::Vec3 from_px(const physx::PxVec3& value) noexcept { return {value.x, value.y, value.z}; }
math::Quat from_px(const physx::PxQuat& value) noexcept {
    return math::Normalize(math::Quat{value.x, value.y, value.z, value.w});
}

math::Quat rotation_between(math::Vec3 from, math::Vec3 to) noexcept {
    from = math::Normalize(from);
    to = math::Normalize(to);
    const float cosine = math::Dot(from, to);
    if (!math::IsFinite(cosine) || cosine >= 1.0f - math::Epsilon) return math::Quat::Identity();
    if (cosine <= -1.0f + math::Epsilon) {
        math::Vec3 axis = math::Cross(from, {0.0f, 1.0f, 0.0f});
        if (math::LengthSquared(axis) <= math::Epsilon) axis = math::Cross(from, {0.0f, 0.0f, 1.0f});
        return math::FromAxisAngle(axis, math::Pi);
    }
    const math::Vec3 cross = math::Cross(from, to);
    return math::Normalize(math::Quat{cross.x, cross.y, cross.z, 1.0f + cosine});
}

bool finite_vec3(math::Vec3 value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

bool finite_quat(math::Quat value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z) && math::IsFinite(value.w);
}

physx::PxFilterFlags shinkou_filter_shader(
    physx::PxFilterObjectAttributes attributes0, physx::PxFilterData filterData0,
    physx::PxFilterObjectAttributes attributes1, physx::PxFilterData filterData1,
    physx::PxPairFlags& pairFlags, const void*, physx::PxU32) {
    if ((filterData0.word0 & filterData1.word1) == 0u || (filterData1.word0 & filterData0.word1) == 0u)
        return physx::PxFilterFlag::eSUPPRESS;
    pairFlags = physx::PxPairFlag::eCONTACT_DEFAULT |
                physx::PxPairFlag::eNOTIFY_TOUCH_FOUND |
                physx::PxPairFlag::eNOTIFY_TOUCH_PERSISTS |
                physx::PxPairFlag::eNOTIFY_TOUCH_LOST |
                physx::PxPairFlag::eNOTIFY_CONTACT_POINTS;
    if (physx::PxFilterObjectIsTrigger(attributes0) || physx::PxFilterObjectIsTrigger(attributes1))
        pairFlags = physx::PxPairFlag::eTRIGGER_DEFAULT;
    return physx::PxFilterFlag::eDEFAULT;
}

physx::PxFilterFlags shinkou_filter_shader_no_events(
    physx::PxFilterObjectAttributes attributes0, physx::PxFilterData filterData0,
    physx::PxFilterObjectAttributes attributes1, physx::PxFilterData filterData1,
    physx::PxPairFlags& pairFlags, const void*, physx::PxU32) {
    if ((filterData0.word0 & filterData1.word1) == 0u || (filterData1.word0 & filterData0.word1) == 0u)
        return physx::PxFilterFlag::eSUPPRESS;
    pairFlags = physx::PxPairFlag::eCONTACT_DEFAULT;
    if (physx::PxFilterObjectIsTrigger(attributes0) || physx::PxFilterObjectIsTrigger(attributes1))
        pairFlags = physx::PxPairFlag::eTRIGGER_DEFAULT;
    return physx::PxFilterFlag::eDEFAULT;
}

class QueryFilter final : public physx::PxQueryFilterCallback {
    std::uint32_t layerMask_;

public:
    explicit QueryFilter(std::uint32_t layerMask) noexcept : layerMask_(layerMask) {}

    physx::PxQueryHitType::Enum preFilter(const physx::PxFilterData&,
                                           const physx::PxShape* shape, const physx::PxRigidActor*,
                                           physx::PxHitFlags&) override {
        const physx::PxFilterData shapeData = shape ? shape->getQueryFilterData() : physx::PxFilterData{};
        return (shapeData.word0 & layerMask_) != 0u ? physx::PxQueryHitType::eBLOCK : physx::PxQueryHitType::eNONE;
    }
    physx::PxQueryHitType::Enum postFilter(const physx::PxFilterData&, const physx::PxQueryHit&,
                                            const physx::PxShape*, const physx::PxRigidActor*) override {
        return physx::PxQueryHitType::eNONE;
    }
};

BodyId encoded_body_id(const physx::PxActor* actor) noexcept {
    return actor && actor->userData
        ? static_cast<BodyId>(reinterpret_cast<std::uintptr_t>(actor->userData)) : InvalidBodyId;
}

ShapeIndex encoded_shape_index(const physx::PxShape* shape) noexcept {
    if (!shape || !shape->userData) return 0;
    const auto value = reinterpret_cast<std::uintptr_t>(shape->userData);
    return value > 0 ? static_cast<ShapeIndex>(value - 1u) : 0;
}

} // namespace
#endif

struct PhysXPhysicsWorld::Impl {
    explicit Impl(const PhysicsWorldConfig& value)
        : config(value) {
        if (!math::IsFinite(config.gravity.x) || !math::IsFinite(config.gravity.y) || !math::IsFinite(config.gravity.z))
            config.gravity = {0.0f, -9.81f, 0.0f};
        if (!config.defaultMaterial.valid()) config.defaultMaterial = {};
        if (!math::IsFinite(config.fixedTimeStep) || config.fixedTimeStep <= 0.0f) config.fixedTimeStep = 1.0f / 60.0f;
        if (config.maxSubSteps == 0) config.maxSubSteps = 1;
        if (config.enableContactEvents) contactEvents.reserve(config.contactEventCapacity);
    }

    void enable_fallback() {
        if (fallback) return;
        PhysicsWorldConfig fallbackConfig = config;
        fallbackConfig.backend = PhysicsBackend::Simple;
        fallback = std::make_unique<SimplePhysicsWorld>(fallbackConfig);
    }

    PhysicsWorldConfig config{};
    std::unique_ptr<SimplePhysicsWorld> fallback;
    ContactListener contactListener;
    std::vector<ContactEvent> contactEvents;
    PhysicsStatistics statistics{};
    std::string lastError;
    float accumulatorSeconds{0.0f};
    BodyId nextId{1};
    bool physxReady{false};

#if defined(SHINKOU_WITH_PHYSX)
    struct BodyRecord {
        physx::PxRigidActor* actor{nullptr};
        std::uint64_t userData{0};
        std::size_t activeIndex{0};
    };

    struct MaterialKey {
        float staticFriction{0.0f};
        float dynamicFriction{0.0f};
        float restitution{0.0f};

        bool operator==(const MaterialKey& other) const noexcept {
            return staticFriction == other.staticFriction &&
                   dynamicFriction == other.dynamicFriction &&
                   restitution == other.restitution;
        }
    };

    struct MaterialKeyHash {
        std::size_t operator()(const MaterialKey& key) const noexcept {
            const auto hashFloat = [](float value) noexcept {
                return std::hash<float>{}(value);
            };
            std::size_t result = hashFloat(key.staticFriction);
            result ^= hashFloat(key.dynamicFriction) + static_cast<std::size_t>(0x9e3779b9u) +
                      (result << 6u) + (result >> 2u);
            result ^= hashFloat(key.restitution) + static_cast<std::size_t>(0x9e3779b9u) +
                      (result << 6u) + (result >> 2u);
            return result;
        }
    };

    physx::PxDefaultAllocator allocator;
    physx::PxDefaultErrorCallback errorCallback;
    physx::PxFoundation* foundation{nullptr};
    physx::PxPhysics* physics{nullptr};
    physx::PxDefaultCpuDispatcher* dispatcher{nullptr};
    physx::PxScene* scene{nullptr};
    std::vector<BodyRecord> bodyRecords{1};
    std::vector<BodyId> activeBodyIds;
    std::vector<BodyId> freeBodyIds;
    std::unordered_map<MaterialKey, physx::PxMaterial*, MaterialKeyHash> materials;
    struct SimulationCallback;
    std::unique_ptr<SimulationCallback> callback;
#endif
};

#if defined(SHINKOU_WITH_PHYSX)
struct PhysXPhysicsWorld::Impl::SimulationCallback final : physx::PxSimulationEventCallback {
    Impl& owner;
    explicit SimulationCallback(Impl& value) noexcept : owner(value) {}

    void onConstraintBreak(physx::PxConstraintInfo*, physx::PxU32) override {}
    void onWake(physx::PxActor**, physx::PxU32) override {}
    void onSleep(physx::PxActor**, physx::PxU32) override {}
    void onTrigger(physx::PxTriggerPair*, physx::PxU32) override {}
    void onAdvance(const physx::PxRigidBody* const*, const physx::PxTransform*, const physx::PxU32) override {}

    void onContact(const physx::PxContactPairHeader& header, const physx::PxContactPair* pairs,
                   physx::PxU32 pairCount) override {
        if (!header.actors[0] || !header.actors[1] || !pairs) return;
        const bool removedActorA = header.flags & physx::PxContactPairHeaderFlag::eREMOVED_ACTOR_0;
        const bool removedActorB = header.flags & physx::PxContactPairHeaderFlag::eREMOVED_ACTOR_1;
        const BodyId bodyA = removedActorA ? InvalidBodyId : encoded_body_id(header.actors[0]);
        const BodyId bodyB = removedActorB ? InvalidBodyId : encoded_body_id(header.actors[1]);
        for (physx::PxU32 pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
            const physx::PxContactPair& pair = pairs[pairIndex];
            ContactPhase phase;
            if (pair.events & physx::PxPairFlag::eNOTIFY_TOUCH_FOUND) phase = ContactPhase::Begin;
            else if (pair.events & physx::PxPairFlag::eNOTIFY_TOUCH_PERSISTS) phase = ContactPhase::Persist;
            else if (pair.events & physx::PxPairFlag::eNOTIFY_TOUCH_LOST) phase = ContactPhase::End;
            else continue;

            physx::PxContactPairPoint points[8];
            const physx::PxU32 pointCount = owner.config.collectContactPoints
                ? pair.extractContacts(points, 8) : 0u;
            const physx::PxU32 emittedCount = std::max<physx::PxU32>(pointCount, 1u);
            for (physx::PxU32 pointIndex = 0; pointIndex < emittedCount; ++pointIndex) {
                if (owner.contactEvents.size() >= owner.config.contactEventCapacity) return;
                ContactEvent event;
                event.bodyA = bodyA;
                event.bodyB = bodyB;
                event.shapeA = pair.flags & physx::PxContactPairFlag::eREMOVED_SHAPE_0 ? 0 : encoded_shape_index(pair.shapes[0]);
                event.shapeB = pair.flags & physx::PxContactPairFlag::eREMOVED_SHAPE_1 ? 0 : encoded_shape_index(pair.shapes[1]);
                event.phase = phase;
                if (pointCount > 0) {
                    const auto& point = points[pointIndex];
                    event.point = from_px(point.position);
                    event.normal = from_px(point.normal);
                    event.impulse = from_px(point.impulse);
                    event.separation = point.separation;
                }
                owner.contactEvents.push_back(event);
            }
        }
    }
};
#endif

namespace {

#if defined(SHINKOU_WITH_PHYSX)
bool valid_body_description(const BodyDesc& desc) noexcept {
    return math::IsFinite(desc.position.x) && math::IsFinite(desc.position.y) && math::IsFinite(desc.position.z) &&
           math::IsFinite(desc.velocity.x) && math::IsFinite(desc.velocity.y) && math::IsFinite(desc.velocity.z) &&
           math::IsFinite(desc.angularVelocity.x) && math::IsFinite(desc.angularVelocity.y) && math::IsFinite(desc.angularVelocity.z) &&
           math::IsFinite(desc.rotation.x) && math::IsFinite(desc.rotation.y) && math::IsFinite(desc.rotation.z) &&
           math::IsFinite(desc.rotation.w) && math::IsFinite(desc.mass) && desc.mass >= 0.0f;
}

bool valid_shape_description(const ShapeDesc& shape) noexcept {
    if (!finite_vec3(shape.halfExtents) || !finite_vec3(shape.planeNormal) || !finite_vec3(shape.localPosition) ||
        !finite_quat(shape.localRotation) || !math::IsFinite(shape.radius) || !math::IsFinite(shape.halfHeight) ||
        shape.radius < 0.0f || shape.halfHeight < 0.0f) return false;
    for (const math::Vec3 vertex : shape.vertices) if (!finite_vec3(vertex)) return false;
    if (shape.type == ShapeType::Box && (shape.halfExtents.x <= math::Epsilon || shape.halfExtents.y <= math::Epsilon || shape.halfExtents.z <= math::Epsilon)) return false;
    if ((shape.type == ShapeType::Sphere || shape.type == ShapeType::Capsule) && shape.radius <= math::Epsilon) return false;
    if (shape.type == ShapeType::Plane && math::LengthSquared(shape.planeNormal) <= math::Epsilon) return false;
    if ((shape.type == ShapeType::ConvexMesh || shape.type == ShapeType::TriangleMesh) && shape.vertices.empty()) return false;
    if (shape.type == ShapeType::TriangleMesh) {
        if (shape.indices.size() < 3 || shape.indices.size() % 3 != 0) return false;
        for (const std::uint32_t index : shape.indices) if (index >= shape.vertices.size()) return false;
    }
    return true;
}

BodyId allocate_body_id(PhysXPhysicsWorld::Impl& impl) {
    if (!impl.freeBodyIds.empty()) {
        const BodyId id = impl.freeBodyIds.back();
        impl.freeBodyIds.pop_back();
        return id;
    }

    BodyId id = impl.nextId++;
    while (id == InvalidBodyId ||
           (static_cast<std::size_t>(id) < impl.bodyRecords.size() && impl.bodyRecords[id].actor != nullptr)) {
        if (impl.nextId == InvalidBodyId) impl.nextId = 1;
        id = impl.nextId++;
    }
    if (static_cast<std::size_t>(id) >= impl.bodyRecords.size())
        impl.bodyRecords.resize(static_cast<std::size_t>(id) + 1u);
    return id;
}

bool create_mesh_geometry(PhysXPhysicsWorld::Impl& impl, const ShapeDesc& desc,
                          physx::PxGeometryHolder& geometry, physx::PxBase*& ownedMesh) {
#if defined(SHINKOU_WITH_PHYSX_COOKING)
    ownedMesh = nullptr;
    if (!impl.physics || desc.vertices.empty()) return false;
    std::vector<physx::PxVec3> points;
    points.reserve(desc.vertices.size());
    for (const auto vertex : desc.vertices) points.push_back(to_px(vertex));
    if (desc.type == ShapeType::ConvexMesh) {
        physx::PxConvexMeshDesc meshDesc;
        meshDesc.points.count = static_cast<physx::PxU32>(points.size());
        meshDesc.points.stride = sizeof(physx::PxVec3);
        meshDesc.points.data = points.data();
        meshDesc.flags = physx::PxConvexFlag::eCOMPUTE_CONVEX;
        physx::PxDefaultMemoryOutputStream output;
        const physx::PxCookingParams cookingParams(impl.physics->getTolerancesScale());
        auto* mesh = PxCreateConvexMesh(cookingParams, meshDesc, impl.physics->getPhysicsInsertionCallback());
        if (!mesh) return false;
        geometry = physx::PxConvexMeshGeometry(mesh);
        ownedMesh = mesh;
        return true;
    }
    if (desc.indices.size() < 3 || desc.indices.size() % 3 != 0) return false;
    physx::PxTriangleMeshDesc meshDesc;
    meshDesc.points.count = static_cast<physx::PxU32>(points.size());
    meshDesc.points.stride = sizeof(physx::PxVec3);
    meshDesc.points.data = points.data();
    meshDesc.triangles.count = static_cast<physx::PxU32>(desc.indices.size() / 3);
    meshDesc.triangles.stride = sizeof(std::uint32_t) * 3u;
    meshDesc.triangles.data = desc.indices.data();
    const physx::PxCookingParams cookingParams(impl.physics->getTolerancesScale());
    auto* mesh = PxCreateTriangleMesh(cookingParams, meshDesc, impl.physics->getPhysicsInsertionCallback());
    if (!mesh) return false;
    geometry = physx::PxTriangleMeshGeometry(mesh);
    ownedMesh = mesh;
    return true;
#else
    (void)impl;
    (void)desc;
    (void)geometry;
    (void)ownedMesh;
    return false;
#endif
}

physx::PxMaterial* get_or_create_material(PhysXPhysicsWorld::Impl& impl,
                                           const PhysicsMaterialDesc& desc) {
    const PhysXPhysicsWorld::Impl::MaterialKey key{
        desc.staticFriction, desc.dynamicFriction, desc.restitution};
    if (const auto it = impl.materials.find(key); it != impl.materials.end()) return it->second;

    auto* material = impl.physics->createMaterial(desc.staticFriction,
                                                   desc.dynamicFriction,
                                                   desc.restitution);
    if (!material) return nullptr;
    impl.materials.emplace(key, material);
    return material;
}

bool create_shape(PhysXPhysicsWorld::Impl& impl, physx::PxRigidActor& actor,
                  const ShapeDesc& desc, ShapeIndex index) {
    const PhysicsMaterialDesc materialDesc = desc.material.valid() ? desc.material : impl.config.defaultMaterial;
    auto* material = get_or_create_material(impl, materialDesc);
    if (!material) return false;
    physx::PxGeometryHolder geometry;
    physx::PxBase* ownedMesh = nullptr;
    switch (desc.type) {
    case ShapeType::Box:
        geometry.storeAny(physx::PxBoxGeometry(std::abs(desc.halfExtents.x), std::abs(desc.halfExtents.y), std::abs(desc.halfExtents.z)));
        break;
    case ShapeType::Sphere:
        geometry.storeAny(physx::PxSphereGeometry(std::abs(desc.radius)));
        break;
    case ShapeType::Capsule:
        geometry.storeAny(physx::PxCapsuleGeometry(std::abs(desc.radius), std::abs(desc.halfHeight)));
        break;
    case ShapeType::Plane:
        if (!actor.is<physx::PxRigidStatic>()) {
            return false;
        }
        geometry.storeAny(physx::PxPlaneGeometry());
        break;
    case ShapeType::ConvexMesh:
    case ShapeType::TriangleMesh:
        if (!create_mesh_geometry(impl, desc, geometry, ownedMesh)) {
            return false;
        }
        break;
    default:
        return false;
    }
    auto* shape = impl.physics->createShape(geometry.any(), *material, true);
    if (!shape) {
        if (ownedMesh) ownedMesh->release();
        return false;
    }
    shape->userData = reinterpret_cast<void*>(static_cast<std::uintptr_t>(index) + 1u);
    math::Quat localRotation = desc.localRotation;
    if (desc.type == ShapeType::Plane)
        localRotation = math::Normalize(desc.localRotation * rotation_between({1.0f, 0.0f, 0.0f}, desc.planeNormal));
    shape->setLocalPose(physx::PxTransform(to_px(desc.localPosition), to_px(localRotation)));
    shape->setSimulationFilterData({desc.filter.layer, desc.filter.mask, 0u, 0u});
    shape->setQueryFilterData({desc.filter.layer, desc.filter.mask, 0u, 0u});
    shape->setFlag(physx::PxShapeFlag::eSIMULATION_SHAPE, desc.simulationEnabled);
    shape->setFlag(physx::PxShapeFlag::eSCENE_QUERY_SHAPE, desc.queryEnabled);
    if (!actor.attachShape(*shape)) {
        shape->release();
        if (ownedMesh) ownedMesh->release();
        return false;
    }
    shape->release();
    if (ownedMesh) ownedMesh->release();
    return true;
}
#endif

} // namespace

PhysXPhysicsWorld::PhysXPhysicsWorld(const PhysicsWorldConfig& config)
    : impl_(std::make_unique<Impl>(config)) {
#if defined(SHINKOU_WITH_PHYSX)
    const std::uint32_t detectedThreads = std::thread::hardware_concurrency();
    const std::uint32_t workerThreads = config.workerThreads == 0
        ? std::clamp(detectedThreads > 1 ? detectedThreads - 1u : 1u, 1u, 8u)
        : std::max(config.workerThreads, 1u);
    impl_->foundation = PxCreateFoundation(PX_PHYSICS_VERSION, impl_->allocator, impl_->errorCallback);
    if (!impl_->foundation) {
        impl_->lastError = "PxCreateFoundation failed";
        impl_->enable_fallback();
        return;
    }
    impl_->physics = PxCreatePhysics(PX_PHYSICS_VERSION, *impl_->foundation,
                                     physx::PxTolerancesScale(), false);
    if (!impl_->physics) {
        impl_->lastError = "PxCreatePhysics failed";
        impl_->enable_fallback();
        return;
    }
    impl_->dispatcher = physx::PxDefaultCpuDispatcherCreate(workerThreads);
    if (!impl_->dispatcher) {
        impl_->lastError = "PxDefaultCpuDispatcherCreate failed";
        impl_->enable_fallback();
        return;
    }
    physx::PxSceneDesc sceneDesc(impl_->physics->getTolerancesScale());
    sceneDesc.gravity = to_px(impl_->config.gravity);
    sceneDesc.cpuDispatcher = impl_->dispatcher;
    const bool contactEventsEnabled = config.enableContactEvents && config.contactEventCapacity > 0;
    sceneDesc.filterShader = contactEventsEnabled ? shinkou_filter_shader : shinkou_filter_shader_no_events;
    impl_->callback = std::make_unique<Impl::SimulationCallback>(*impl_);
    sceneDesc.simulationEventCallback = contactEventsEnabled ? impl_->callback.get() : nullptr;
    impl_->scene = impl_->physics->createScene(sceneDesc);
    if (!impl_->scene) {
        impl_->lastError = "PxPhysics::createScene failed";
        impl_->enable_fallback();
        return;
    }
    impl_->physxReady = true;
#else
    (void)config;
    impl_->enable_fallback();
    impl_->lastError = "PhysX SDK was not found at configure time; using Simple fallback";
#endif
}

PhysXPhysicsWorld::~PhysXPhysicsWorld() {
#if defined(SHINKOU_WITH_PHYSX)
    for (const BodyId id : impl_->activeBodyIds) {
        if (static_cast<std::size_t>(id) >= impl_->bodyRecords.size()) continue;
        auto* actor = impl_->bodyRecords[id].actor;
        if (!actor) continue;
        if (impl_->scene) impl_->scene->removeActor(*actor);
        actor->release();
    }
    impl_->activeBodyIds.clear();
    impl_->bodyRecords.clear();
    impl_->freeBodyIds.clear();
    for (auto& [key, material] : impl_->materials) {
        (void)key;
        if (material) material->release();
    }
    impl_->materials.clear();
    if (impl_->scene) impl_->scene->release();
    if (impl_->dispatcher) impl_->dispatcher->release();
    if (impl_->physics) impl_->physics->release();
    if (impl_->foundation) impl_->foundation->release();
#endif
}

bool PhysXPhysicsWorld::is_available() const noexcept { return impl_->physxReady; }

const std::string& PhysXPhysicsWorld::last_error() const noexcept {
    if (!impl_->lastError.empty()) return impl_->lastError;
    if (impl_->physxReady || !impl_->fallback) return impl_->lastError;
    return impl_->fallback->last_error();
}

BodyId PhysXPhysicsWorld::create_body(const BodyDesc& input) {
#if defined(SHINKOU_WITH_PHYSX)
    if (impl_->physxReady) {
        if (!valid_body_description(input)) {
            impl_->lastError = "invalid body description";
            return InvalidBodyId;
        }
        BodyDesc desc = input;
        desc.rotation = math::Normalize(desc.rotation);
        if (desc.dynamic && !desc.kinematic && desc.mass <= math::Epsilon) desc.mass = 1.0f;
        if (!desc.dynamic) desc.kinematic = false;
        if (!math::IsFinite(desc.linearDamping) || desc.linearDamping < 0.0f) desc.linearDamping = 0.05f;
        if (!math::IsFinite(desc.angularDamping) || desc.angularDamping < 0.0f) desc.angularDamping = 0.05f;
        if (!math::IsFinite(desc.maxLinearVelocity) || desc.maxLinearVelocity < 0.0f) desc.maxLinearVelocity = 1000.0f;
        if (!math::IsFinite(desc.maxAngularVelocity) || desc.maxAngularVelocity < 0.0f) desc.maxAngularVelocity = 1000.0f;
        if (desc.shapes.empty()) {
            desc.shapes.push_back(ShapeDesc::box(desc.halfExtents));
            desc.shapes.front().filter = desc.filter;
        }
        for (ShapeDesc& shape : desc.shapes) {
            if (!valid_shape_description(shape)) {
                impl_->lastError = "invalid shape description";
                return InvalidBodyId;
            }
            if (desc.dynamic && !desc.kinematic && shape.simulationEnabled &&
                (shape.type == ShapeType::Plane || shape.type == ShapeType::TriangleMesh)) {
                impl_->lastError = "PhysX does not support simulated plane or triangle mesh shapes on non-kinematic dynamics";
                return InvalidBodyId;
            }
            if (shape.filter.layer == 1u && shape.filter.mask == 0xffffffffu) shape.filter = desc.filter;
            else {
                shape.filter.layer &= desc.filter.layer;
                shape.filter.mask &= desc.filter.mask;
            }
            if (!shape.material.valid()) shape.material = impl_->config.defaultMaterial;
        }
        const BodyId id = allocate_body_id(*impl_);
        const auto release_allocated_id = [&]() noexcept {
            impl_->bodyRecords[id] = {};
            impl_->freeBodyIds.push_back(id);
        };
        auto* actor = desc.dynamic
            ? static_cast<physx::PxRigidActor*>(impl_->physics->createRigidDynamic(
                physx::PxTransform(to_px(desc.position), to_px(desc.rotation))))
            : static_cast<physx::PxRigidActor*>(impl_->physics->createRigidStatic(
                physx::PxTransform(to_px(desc.position), to_px(desc.rotation))));
        if (!actor) {
            release_allocated_id();
            impl_->lastError = "PhysX rigid actor creation failed";
            return InvalidBodyId;
        }
        bool valid = true;
        for (std::size_t shapeIndex = 0; shapeIndex < desc.shapes.size(); ++shapeIndex) {
            if (shapeIndex > std::numeric_limits<ShapeIndex>::max() ||
                !create_shape(*impl_, *actor, desc.shapes[shapeIndex], static_cast<ShapeIndex>(shapeIndex))) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            actor->release();
            release_allocated_id();
            impl_->lastError = "PhysX shape creation failed; check geometry and Cooking support";
            return InvalidBodyId;
        }
        actor->userData = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
        actor->setActorFlag(physx::PxActorFlag::eDISABLE_SIMULATION, !desc.enabled);
        if (auto* dynamic = actor->is<physx::PxRigidDynamic>()) {
            dynamic->setLinearVelocity(to_px(desc.velocity));
            dynamic->setAngularVelocity(to_px(desc.angularVelocity));
            dynamic->setLinearDamping(std::max(desc.linearDamping, 0.0f));
            dynamic->setAngularDamping(std::max(desc.angularDamping, 0.0f));
            dynamic->setMaxLinearVelocity(std::max(desc.maxLinearVelocity, 0.0f));
            dynamic->setMaxAngularVelocity(std::max(desc.maxAngularVelocity, 0.0f));
            if (desc.kinematic) dynamic->setRigidBodyFlag(physx::PxRigidBodyFlag::eKINEMATIC, true);
            const float mass = std::max(desc.mass, 1.0e-4f);
#if defined(SHINKOU_WITH_PHYSX_EXTENSIONS)
            if (!physx::PxRigidBodyExt::setMassAndUpdateInertia(*dynamic, mass)) {
                actor->release();
                release_allocated_id();
                impl_->lastError = "PhysX mass and inertia calculation failed";
                return InvalidBodyId;
            }
#else
            dynamic->setMass(mass);
#endif
            if (!desc.startAwake) dynamic->putToSleep();
        }
        if (!impl_->scene->addActor(*actor)) {
            actor->release();
            release_allocated_id();
            impl_->lastError = "PhysX scene rejected rigid actor";
            return InvalidBodyId;
        }
        auto& record = impl_->bodyRecords[id];
        record.actor = actor;
        record.userData = desc.userData;
        record.activeIndex = impl_->activeBodyIds.size();
        impl_->activeBodyIds.push_back(id);
        ++impl_->statistics.bodyCount;
        if (desc.dynamic) ++impl_->statistics.dynamicBodyCount;
        impl_->lastError.clear();
        return id;
    }
#else
    (void)input;
#endif
    return impl_->fallback ? impl_->fallback->create_body(input) : InvalidBodyId;
}

void PhysXPhysicsWorld::destroy_body(BodyId id) {
#if defined(SHINKOU_WITH_PHYSX)
    if (static_cast<std::size_t>(id) < impl_->bodyRecords.size() && impl_->bodyRecords[id].actor) {
        auto& record = impl_->bodyRecords[id];
        auto* actor = record.actor;
        auto* dynamic = actor->is<physx::PxRigidDynamic>();
        const bool awake = dynamic && !dynamic->isSleeping();
        if (impl_->scene) impl_->scene->removeActor(*actor);
        actor->release();
        const std::size_t activeIndex = record.activeIndex;
        const BodyId movedId = impl_->activeBodyIds.back();
        impl_->activeBodyIds[activeIndex] = movedId;
        impl_->bodyRecords[movedId].activeIndex = activeIndex;
        impl_->activeBodyIds.pop_back();
        record = {};
        impl_->freeBodyIds.push_back(id);
        if (impl_->statistics.bodyCount > 0) --impl_->statistics.bodyCount;
        if (dynamic && impl_->statistics.dynamicBodyCount > 0) --impl_->statistics.dynamicBodyCount;
        if (awake && impl_->statistics.activeDynamicBodyCount > 0) --impl_->statistics.activeDynamicBodyCount;
        return;
    }
#endif
    if (impl_->fallback) impl_->fallback->destroy_body(id);
}

bool PhysXPhysicsWorld::is_valid(BodyId id) const noexcept {
#if defined(SHINKOU_WITH_PHYSX)
    if (static_cast<std::size_t>(id) < impl_->bodyRecords.size() && impl_->bodyRecords[id].actor) return true;
#endif
    return impl_->fallback && impl_->fallback->is_valid(id);
}

bool PhysXPhysicsWorld::get_body_state(BodyId id, BodyState& output) const noexcept {
#if defined(SHINKOU_WITH_PHYSX)
    if (impl_->physxReady && static_cast<std::size_t>(id) < impl_->bodyRecords.size() && impl_->bodyRecords[id].actor) {
        const auto* actor = impl_->bodyRecords[id].actor;
        const auto pose = actor->getGlobalPose();
        output.body = id;
        output.position = from_px(pose.p);
        output.rotation = from_px(pose.q);
        output.userData = impl_->bodyRecords[id].userData;
        output.dynamic = actor->is<physx::PxRigidDynamic>() != nullptr;
        output.kinematic = output.dynamic && actor->is<physx::PxRigidDynamic>()->getRigidBodyFlags().isSet(physx::PxRigidBodyFlag::eKINEMATIC);
        output.enabled = !actor->getActorFlags().isSet(physx::PxActorFlag::eDISABLE_SIMULATION);
        output.awake = output.dynamic ? !actor->is<physx::PxRigidDynamic>()->isSleeping() : false;
        output.linearVelocity = {};
        output.angularVelocity = {};
        if (output.dynamic) {
            output.linearVelocity = from_px(actor->is<physx::PxRigidDynamic>()->getLinearVelocity());
            output.angularVelocity = from_px(actor->is<physx::PxRigidDynamic>()->getAngularVelocity());
        }
        return true;
    }
#endif
    return impl_->fallback && impl_->fallback->get_body_state(id, output);
}

void PhysXPhysicsWorld::get_body_ids(std::vector<BodyId>& output) const {
    output.clear();
#if defined(SHINKOU_WITH_PHYSX)
    if (impl_->physxReady) {
        output = impl_->activeBodyIds;
        return;
    }
#endif
    if (impl_->fallback) impl_->fallback->get_body_ids(output);
}

void PhysXPhysicsWorld::get_body_states(std::vector<BodyState>& output) const {
    output.clear();
#if defined(SHINKOU_WITH_PHYSX)
    if (impl_->physxReady) {
        output.reserve(impl_->activeBodyIds.size());
        for (const BodyId id : impl_->activeBodyIds) {
            if (static_cast<std::size_t>(id) >= impl_->bodyRecords.size() || !impl_->bodyRecords[id].actor) continue;
            const auto* actor = impl_->bodyRecords[id].actor;
            const auto pose = actor->getGlobalPose();
            BodyState state;
            state.body = id;
            state.position = from_px(pose.p);
            state.rotation = from_px(pose.q);
            state.userData = impl_->bodyRecords[id].userData;
            state.dynamic = actor->is<physx::PxRigidDynamic>() != nullptr;
            state.kinematic = state.dynamic && actor->is<physx::PxRigidDynamic>()->getRigidBodyFlags().isSet(physx::PxRigidBodyFlag::eKINEMATIC);
            state.enabled = !actor->getActorFlags().isSet(physx::PxActorFlag::eDISABLE_SIMULATION);
            state.awake = state.dynamic && !actor->is<physx::PxRigidDynamic>()->isSleeping();
            if (state.dynamic) {
                const auto* dynamic = actor->is<physx::PxRigidDynamic>();
                state.linearVelocity = from_px(dynamic->getLinearVelocity());
                state.angularVelocity = from_px(dynamic->getAngularVelocity());
            }
            output.push_back(state);
        }
        return;
    }
#endif
    if (impl_->fallback) impl_->fallback->get_body_states(output);
}

bool PhysXPhysicsWorld::set_body_transform(BodyId id, math::Vec3 position, math::Quat rotation, bool wake) {
#if defined(SHINKOU_WITH_PHYSX)
    if (static_cast<std::size_t>(id) < impl_->bodyRecords.size() && impl_->bodyRecords[id].actor) {
        auto* actor = impl_->bodyRecords[id].actor;
        if (!finite_vec3(position) || !finite_quat(rotation)) return false;
        actor->setGlobalPose(physx::PxTransform(to_px(position), to_px(rotation)), wake);
        if (wake) if (auto* dynamic = actor->is<physx::PxRigidDynamic>()) dynamic->wakeUp();
        return true;
    }
#else
    (void)position;
    (void)rotation;
    (void)wake;
#endif
    return impl_->fallback && impl_->fallback->set_body_transform(id, position, rotation, wake);
}

bool PhysXPhysicsWorld::set_linear_velocity(BodyId id, math::Vec3 velocity) {
#if defined(SHINKOU_WITH_PHYSX)
    if (!finite_vec3(velocity)) return false;
    if (static_cast<std::size_t>(id) < impl_->bodyRecords.size() && impl_->bodyRecords[id].actor)
        if (auto* dynamic = impl_->bodyRecords[id].actor->is<physx::PxRigidDynamic>()) {
            dynamic->setLinearVelocity(to_px(velocity));
            dynamic->wakeUp();
            return true;
        }
#endif
    return impl_->fallback && impl_->fallback->set_linear_velocity(id, velocity);
}

bool PhysXPhysicsWorld::set_angular_velocity(BodyId id, math::Vec3 velocity) {
#if defined(SHINKOU_WITH_PHYSX)
    if (!finite_vec3(velocity)) return false;
    if (static_cast<std::size_t>(id) < impl_->bodyRecords.size() && impl_->bodyRecords[id].actor)
        if (auto* dynamic = impl_->bodyRecords[id].actor->is<physx::PxRigidDynamic>()) {
            dynamic->setAngularVelocity(to_px(velocity));
            dynamic->wakeUp();
            return true;
        }
#endif
    return impl_->fallback && impl_->fallback->set_angular_velocity(id, velocity);
}

bool PhysXPhysicsWorld::add_force(BodyId id, math::Vec3 force) {
#if defined(SHINKOU_WITH_PHYSX)
    if (!finite_vec3(force)) return false;
    if (static_cast<std::size_t>(id) < impl_->bodyRecords.size() && impl_->bodyRecords[id].actor)
        if (auto* dynamic = impl_->bodyRecords[id].actor->is<physx::PxRigidDynamic>()) {
            dynamic->addForce(to_px(force));
            return true;
        }
#endif
    return impl_->fallback && impl_->fallback->add_force(id, force);
}

bool PhysXPhysicsWorld::add_torque(BodyId id, math::Vec3 torque) {
#if defined(SHINKOU_WITH_PHYSX)
    if (!finite_vec3(torque)) return false;
    if (static_cast<std::size_t>(id) < impl_->bodyRecords.size() && impl_->bodyRecords[id].actor)
        if (auto* dynamic = impl_->bodyRecords[id].actor->is<physx::PxRigidDynamic>()) {
            dynamic->addTorque(to_px(torque));
            return true;
        }
#endif
    return impl_->fallback && impl_->fallback->add_torque(id, torque);
}

bool PhysXPhysicsWorld::set_kinematic_target(BodyId id, math::Vec3 position, math::Quat rotation) {
#if defined(SHINKOU_WITH_PHYSX)
    if (static_cast<std::size_t>(id) < impl_->bodyRecords.size() && impl_->bodyRecords[id].actor)
        if (auto* dynamic = impl_->bodyRecords[id].actor->is<physx::PxRigidDynamic>()) {
            if (!dynamic->getRigidBodyFlags().isSet(physx::PxRigidBodyFlag::eKINEMATIC) || !finite_vec3(position) || !finite_quat(rotation)) return false;
            dynamic->setKinematicTarget(physx::PxTransform(to_px(position), to_px(rotation)));
            return true;
        }
#endif
    return impl_->fallback && impl_->fallback->set_kinematic_target(id, position, rotation);
}

void PhysXPhysicsWorld::set_gravity(math::Vec3 gravity) noexcept {
    if (!math::IsFinite(gravity.x) || !math::IsFinite(gravity.y) || !math::IsFinite(gravity.z)) return;
    impl_->config.gravity = gravity;
#if defined(SHINKOU_WITH_PHYSX)
    if (impl_->scene) impl_->scene->setGravity(to_px(gravity));
#endif
    if (impl_->fallback) impl_->fallback->set_gravity(gravity);
}

math::Vec3 PhysXPhysicsWorld::gravity() const noexcept {
#if defined(SHINKOU_WITH_PHYSX)
    if (impl_->scene) return from_px(impl_->scene->getGravity());
#endif
    return impl_->fallback ? impl_->fallback->gravity() : impl_->config.gravity;
}

namespace {

void update_statistics(PhysXPhysicsWorld::Impl& impl) {
#if defined(SHINKOU_WITH_PHYSX)
    if (impl.physxReady) {
        std::size_t active = 0;
        for (const BodyId id : impl.activeBodyIds) {
            if (static_cast<std::size_t>(id) >= impl.bodyRecords.size()) continue;
            const auto* actor = impl.bodyRecords[id].actor;
            if (!actor) continue;
            if (const auto* dynamic = actor->is<physx::PxRigidDynamic>(); dynamic && !dynamic->isSleeping()) ++active;
        }
        impl.statistics.activeDynamicBodyCount = active;
        impl.statistics.lastContactEventCount = impl.contactEvents.size();
        return;
    }
#endif
    if (impl.fallback) impl.statistics = impl.fallback->statistics();
}

bool simulate_once(PhysXPhysicsWorld::Impl& impl, float dt) {
#if defined(SHINKOU_WITH_PHYSX)
    if (impl.physxReady) {
        if (!impl.scene->simulate(dt) || !impl.scene->fetchResults(true)) {
            impl.lastError = "PhysX scene simulation failed";
            return false;
        }
        ++impl.statistics.simulationSteps;
        impl.statistics.simulatedSeconds += dt;
        return true;
    }
#else
    (void)dt;
#endif
    if (!impl.fallback) return false;
    impl.fallback->step(dt);
    return true;
}

} // namespace

void PhysXPhysicsWorld::step(float dt) {
    impl_->contactEvents.clear();
    if (!math::IsFinite(dt) || dt <= 0.0f) {
        impl_->lastError = "step duration must be finite and positive";
        return;
    }
    const float fixedStep = impl_->config.fixedTimeStep;
    const float clampedDt = std::min(dt, fixedStep * static_cast<float>(impl_->config.maxSubSteps));
    bool simulationOk = true;
    if (!impl_->physxReady) {
        if (impl_->fallback) {
            impl_->fallback->step(dt);
            impl_->statistics = impl_->fallback->statistics();
        } else {
            simulationOk = false;
            impl_->lastError = "physics backend is unavailable";
        }
    } else if (!impl_->config.useFixedTimeStep) {
        simulationOk = simulate_once(*impl_, clampedDt);
    } else {
        impl_->accumulatorSeconds += clampedDt;
        std::uint32_t subSteps = 0;
        while (impl_->accumulatorSeconds + 1.0e-7f >= fixedStep && subSteps++ < impl_->config.maxSubSteps) {
            if (!simulate_once(*impl_, fixedStep)) {
                simulationOk = false;
                break;
            }
            impl_->accumulatorSeconds -= fixedStep;
        }
    }
    impl_->statistics.accumulatorSeconds = impl_->accumulatorSeconds;
    update_statistics(*impl_);
    if (impl_->contactListener) for (const ContactEvent& event : impl_->contactEvents) impl_->contactListener(event);
    if (simulationOk && impl_->physxReady) impl_->lastError.clear();
}

bool PhysXPhysicsWorld::raycast(const RaycastQuery& query, RaycastHit& hit) const {
#if defined(SHINKOU_WITH_PHYSX)
    if (impl_->physxReady) {
        if (!finite_vec3(query.origin) || !finite_vec3(query.direction) || !math::IsFinite(query.maxDistance) || query.maxDistance <= 0.0f || query.layerMask == 0u) return false;
        const float length = math::Length(query.direction);
        if (!math::IsFinite(length) || length <= math::Epsilon) return false;
        if (!query.includeStatic && !query.includeDynamic) return false;
        physx::PxQueryFlags flags(physx::PxQueryFlag::ePREFILTER);
        if (query.includeStatic) flags |= physx::PxQueryFlag::eSTATIC;
        if (query.includeDynamic) flags |= physx::PxQueryFlag::eDYNAMIC;
        QueryFilter filter(query.layerMask);
        physx::PxRaycastBufferN<64> buffer;
        const math::Vec3 direction = query.direction / length;
        if (!impl_->scene->raycast(to_px(query.origin), to_px(direction), query.maxDistance, buffer,
                                   physx::PxHitFlag::ePOSITION | physx::PxHitFlag::eNORMAL,
                                   physx::PxQueryFilterData(physx::PxFilterData{}, flags), &filter) || !buffer.hasBlock || !buffer.block.actor) return false;
        const auto& block = buffer.block;
        hit.body = encoded_body_id(block.actor);
        hit.point = from_px(block.position);
        hit.normal = from_px(block.normal);
        hit.distance = block.distance;
        hit.shape = encoded_shape_index(block.shape);
        hit.userData = static_cast<std::size_t>(hit.body) < impl_->bodyRecords.size() && impl_->bodyRecords[hit.body].actor
            ? impl_->bodyRecords[hit.body].userData
            : 0;
        return true;
    }
#endif
    return impl_->fallback && impl_->fallback->raycast(query, hit);
}

void PhysXPhysicsWorld::set_contact_listener(ContactListener listener) { impl_->contactListener = std::move(listener); }

void PhysXPhysicsWorld::drain_contact_events(std::vector<ContactEvent>& output) {
    output = std::move(impl_->contactEvents);
    impl_->contactEvents.clear();
    if (impl_->config.enableContactEvents) impl_->contactEvents.reserve(impl_->config.contactEventCapacity);
}

const PhysicsStatistics& PhysXPhysicsWorld::statistics() const noexcept {
    return impl_->physxReady || !impl_->fallback ? impl_->statistics : impl_->fallback->statistics();
}

} // namespace shinkou::physics

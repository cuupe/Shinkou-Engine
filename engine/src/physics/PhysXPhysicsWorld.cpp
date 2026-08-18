#include "shinkou/physics/PhysXPhysicsWorld.h"
#include "shinkou/physics/SimplePhysicsWorld.h"

#if defined(SHINKOU_WITH_PHYSX)
#include <PxPhysicsAPI.h>
#include <unordered_map>
#endif

namespace shinkou::physics {
struct PhysXPhysicsWorld::Impl {
    SimplePhysicsWorld fallback;
#if defined(SHINKOU_WITH_PHYSX)
    physx::PxDefaultAllocator allocator;
    physx::PxDefaultErrorCallback errorCallback;
    physx::PxFoundation* foundation{nullptr};
    physx::PxPhysics* physics{nullptr};
    physx::PxDefaultCpuDispatcher* dispatcher{nullptr};
    physx::PxScene* scene{nullptr};
    physx::PxMaterial* material{nullptr};
    std::unordered_map<BodyId, physx::PxRigidActor*> actors;
#endif
};

PhysXPhysicsWorld::PhysXPhysicsWorld() : impl_(std::make_unique<Impl>()) {
#if defined(SHINKOU_WITH_PHYSX)
    using namespace physx;
    foundation = PxCreateFoundation(PX_PHYSICS_VERSION, allocator, errorCallback);
    if (!foundation) return;
    physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, PxTolerancesScale(), false);
    if (!physics) return;
    PxSceneDesc sceneDesc(physics->getTolerancesScale());
    sceneDesc.gravity = PxVec3(0, -9.81f, 0);
    dispatcher = PxDefaultCpuDispatcherCreate(2);
    sceneDesc.cpuDispatcher = dispatcher;
    sceneDesc.filterShader = PxDefaultSimulationFilterShader;
    scene = physics->createScene(sceneDesc);
    material = physics->createMaterial(0.5f, 0.5f, 0.35f);
#endif
}
PhysXPhysicsWorld::~PhysXPhysicsWorld() {
#if defined(SHINKOU_WITH_PHYSX)
    if (scene) scene->release();
    if (dispatcher) dispatcher->release();
    if (material) material->release();
    if (physics) physics->release();
    if (foundation) foundation->release();
#endif
}
BodyId PhysXPhysicsWorld::create_body(const BodyDesc& desc) {
#if defined(SHINKOU_WITH_PHYSX)
    if (impl_->scene && impl_->physics && impl_->material) {
        const BodyId id = static_cast<BodyId>(impl_->actors.size() + 1);
        const physx::PxTransform transform(physx::PxVec3(desc.position.x, desc.position.y, desc.position.z));
        physx::PxRigidActor* actor = desc.dynamic
            ? static_cast<physx::PxRigidActor*>(impl_->physics->createRigidDynamic(transform))
            : impl_->physics->createRigidStatic(transform);
        if (!actor) return 0;
        const physx::PxBoxGeometry box(desc.halfExtents.x, desc.halfExtents.y, desc.halfExtents.z);
        actor->createShape(box, *impl_->material);
        if (auto* dynamic = actor->is<physx::PxRigidDynamic>()) {
            dynamic->setLinearVelocity({desc.velocity.x, desc.velocity.y, desc.velocity.z});
            dynamic->setMass(desc.mass);
        }
        actor->userData = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
        impl_->scene->addActor(*actor);
        impl_->actors.emplace(id, actor);
        return id;
    }
#endif
    return impl_->fallback.create_body(desc);
}
void PhysXPhysicsWorld::destroy_body(BodyId id) {
#if defined(SHINKOU_WITH_PHYSX)
    const auto it = impl_->actors.find(id);
    if (it != impl_->actors.end()) {
        if (impl_->scene) impl_->scene->removeActor(*it->second);
        it->second->release();
        impl_->actors.erase(it);
        return;
    }
#endif
    impl_->fallback.destroy_body(id);
}
void PhysXPhysicsWorld::step(float dt) {
#if defined(SHINKOU_WITH_PHYSX)
    if (impl_->scene) {
        impl_->scene->simulate(dt);
        impl_->scene->fetchResults(true);
        return;
    }
#endif
    impl_->fallback.step(dt);
}
bool PhysXPhysicsWorld::raycast(math::Vec3 o, math::Vec3 d, float r, RaycastHit& h) const {
#if defined(SHINKOU_WITH_PHYSX)
    if (impl_->scene) {
        physx::PxRaycastBuffer hit;
        if (!impl_->scene->raycast({o.x, o.y, o.z}, {d.x, d.y, d.z}, r, hit) || !hit.hasBlock) return false;
        h.body = static_cast<BodyId>(reinterpret_cast<std::uintptr_t>(hit.block.actor->userData));
        h.point = {hit.block.position.x, hit.block.position.y, hit.block.position.z};
        h.normal = {hit.block.normal.x, hit.block.normal.y, hit.block.normal.z};
        h.distance = hit.block.distance;
        return true;
    }
#endif
    return impl_->fallback.raycast(o, d, r, h);
}
}

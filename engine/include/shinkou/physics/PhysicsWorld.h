#pragma once

#include "shinkou/Math.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace shinkou::physics {

using BodyId = std::uint32_t;
using ShapeIndex = std::uint16_t;

constexpr BodyId InvalidBodyId = 0;

enum class PhysicsBackend : std::uint8_t {
    Auto,
    Simple,
    PhysX
};

enum class ShapeType : std::uint8_t {
    Box,
    Sphere,
    Capsule,
    Plane,
    ConvexMesh,
    TriangleMesh
};

enum class ContactPhase : std::uint8_t {
    Begin,
    Persist,
    End
};

struct CollisionFilter {
    std::uint32_t layer{1u};
    std::uint32_t mask{0xffffffffu};
};

struct PhysicsMaterialDesc {
    float staticFriction{0.5f};
    float dynamicFriction{0.5f};
    float restitution{0.35f};

    bool valid() const noexcept;
};

// ShapeDesc is intentionally backend-neutral. Primitive geometry is cheap to
// create; mesh vectors are consumed during body creation and can then be
// released by the caller. Mesh cooking is available only when the PhysX
// Cooking library is found by CMake.
struct ShapeDesc {
    ShapeType type{ShapeType::Box};
    math::Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    float radius{0.5f};
    float halfHeight{0.5f};
    math::Vec3 planeNormal{0.0f, 1.0f, 0.0f};
    math::Vec3 localPosition{};
    math::Quat localRotation{};
    PhysicsMaterialDesc material{};
    CollisionFilter filter{};
    bool simulationEnabled{true};
    bool queryEnabled{true};
    std::vector<math::Vec3> vertices;
    std::vector<std::uint32_t> indices;

    static ShapeDesc box(math::Vec3 halfExtents) noexcept {
        ShapeDesc result;
        result.type = ShapeType::Box;
        result.halfExtents = halfExtents;
        return result;
    }
    static ShapeDesc sphere(float radius) noexcept {
        ShapeDesc result;
        result.type = ShapeType::Sphere;
        result.radius = radius;
        return result;
    }
    static ShapeDesc capsule(float radius, float halfHeight) noexcept {
        ShapeDesc result;
        result.type = ShapeType::Capsule;
        result.radius = radius;
        result.halfHeight = halfHeight;
        return result;
    }
};

struct BodyDesc {
    // The first five fields retain the original aggregate construction order
    // used by samples and gameplay code.
    math::Vec3 position{};
    math::Vec3 velocity{};
    math::Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    float mass{1.0f};
    bool dynamic{true};

    math::Quat rotation{};
    math::Vec3 angularVelocity{};
    bool kinematic{false};
    bool startAwake{true};
    bool enabled{true};
    float linearDamping{0.05f};
    float angularDamping{0.05f};
    float maxLinearVelocity{1000.0f};
    float maxAngularVelocity{1000.0f};
    CollisionFilter filter{};
    std::uint64_t userData{0};
    std::vector<ShapeDesc> shapes;
};

struct BodyState {
    BodyId body{InvalidBodyId};
    math::Vec3 position{};
    math::Quat rotation{};
    math::Vec3 linearVelocity{};
    math::Vec3 angularVelocity{};
    std::uint64_t userData{0};
    bool dynamic{false};
    bool kinematic{false};
    bool awake{false};
    bool enabled{false};
};

struct RaycastQuery {
    math::Vec3 origin{};
    math::Vec3 direction{0.0f, 0.0f, 1.0f};
    float maxDistance{1000.0f};
    std::uint32_t layerMask{0xffffffffu};
    bool includeStatic{true};
    bool includeDynamic{true};
};

struct RaycastHit {
    BodyId body{InvalidBodyId};
    math::Vec3 point{};
    math::Vec3 normal{};
    float distance{0.0f};
    ShapeIndex shape{0};
    std::uint64_t userData{0};
};

struct ContactEvent {
    BodyId bodyA{InvalidBodyId};
    BodyId bodyB{InvalidBodyId};
    ShapeIndex shapeA{0};
    ShapeIndex shapeB{0};
    math::Vec3 point{};
    math::Vec3 normal{};
    math::Vec3 impulse{};
    float separation{0.0f};
    ContactPhase phase{ContactPhase::Begin};
};

struct PhysicsStatistics {
    std::size_t bodyCount{0};
    std::size_t dynamicBodyCount{0};
    std::size_t activeDynamicBodyCount{0};
    std::size_t lastContactEventCount{0};
    std::uint64_t simulationSteps{0};
    float simulatedSeconds{0.0f};
    float accumulatorSeconds{0.0f};
};

struct PhysicsWorldConfig {
    PhysicsBackend backend{PhysicsBackend::Auto};
    math::Vec3 gravity{0.0f, -9.81f, 0.0f};
    std::uint32_t workerThreads{0};
    bool useFixedTimeStep{true};
    float fixedTimeStep{1.0f / 60.0f};
    std::uint32_t maxSubSteps{8};
    // Contact reporting is intentionally opt-in for high-throughput worlds.
    // Collision solving remains enabled when this is false.
    bool enableContactEvents{false};
    bool collectContactPoints{true};
    std::size_t contactEventCapacity{4096};
    PhysicsMaterialDesc defaultMaterial{};
};

using ContactListener = std::function<void(const ContactEvent&)>;

class IPhysicsWorld {
public:
    virtual ~IPhysicsWorld() = default;

    virtual PhysicsBackend backend() const noexcept = 0;
    virtual bool is_available() const noexcept = 0;
    virtual const char* backend_name() const noexcept = 0;
    virtual const std::string& last_error() const noexcept = 0;

    virtual BodyId create_body(const BodyDesc&) = 0;
    virtual void destroy_body(BodyId) = 0;
    virtual bool is_valid(BodyId) const noexcept = 0;
    virtual bool get_body_state(BodyId, BodyState&) const noexcept = 0;
    virtual void get_body_ids(std::vector<BodyId>&) const = 0;
    // Bulk state extraction is the boundary used by render/gameplay systems.
    // Backends may override it to avoid one virtual call and one lookup per
    // body; the default keeps the API source-compatible for small backends.
    virtual void get_body_states(std::vector<BodyState>& output) const {
        std::vector<BodyId> ids;
        get_body_ids(ids);
        output.clear();
        output.reserve(ids.size());
        for (const BodyId id : ids) {
            BodyState state;
            if (get_body_state(id, state)) output.push_back(state);
        }
    }

    virtual bool set_body_transform(BodyId, math::Vec3 position, math::Quat rotation,
                                    bool wake = true) = 0;
    virtual bool set_linear_velocity(BodyId, math::Vec3 velocity) = 0;
    virtual bool set_angular_velocity(BodyId, math::Vec3 velocity) = 0;
    virtual bool add_force(BodyId, math::Vec3 force) = 0;
    virtual bool add_torque(BodyId, math::Vec3 torque) = 0;
    virtual bool set_kinematic_target(BodyId, math::Vec3 position, math::Quat rotation) = 0;

    virtual void set_gravity(math::Vec3 gravity) noexcept = 0;
    virtual math::Vec3 gravity() const noexcept = 0;
    virtual void step(float dt) = 0;

    virtual bool raycast(const RaycastQuery&, RaycastHit&) const = 0;
    bool raycast(math::Vec3 origin, math::Vec3 direction, float distance,
                 RaycastHit& hit) const {
        return raycast(RaycastQuery{origin, direction, distance}, hit);
    }

    virtual void set_contact_listener(ContactListener listener) = 0;
    virtual void drain_contact_events(std::vector<ContactEvent>& output) = 0;
    virtual const PhysicsStatistics& statistics() const noexcept = 0;
};

// Selects PhysX for Auto when it was found at configure time, otherwise the
// deterministic fallback. Explicit PhysX still returns a usable fallback if
// the SDK is unavailable, with the reason exposed through last_error().
std::unique_ptr<IPhysicsWorld> create_physics_world(const PhysicsWorldConfig& config = {});
bool physx_available() noexcept;

} // namespace shinkou::physics

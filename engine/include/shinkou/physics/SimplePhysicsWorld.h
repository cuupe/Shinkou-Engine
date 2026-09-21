#pragma once

#include "shinkou/physics/PhysicsWorld.h"
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace shinkou::physics {

// Deterministic fallback used when the optional PhysX SDK is unavailable.
// It intentionally implements the same orchestration contract, while keeping
// its collision model small enough to remain a dependable bootstrap backend.
class SimplePhysicsWorld final : public IPhysicsWorld {
    struct Body {
        BodyId id{InvalidBodyId};
        BodyDesc desc{};
        math::Vec3 force{};
        math::Vec3 torque{};
        bool alive{true};
        bool awake{true};
    };

    std::vector<Body> bodies_;
    std::unordered_map<BodyId, std::size_t> bodyLookup_;
    BodyId nextId_{1};
    math::Vec3 gravity_{0.0f, -9.81f, 0.0f};
    PhysicsWorldConfig config_{};
    ContactListener contactListener_;
    std::vector<ContactEvent> contactEvents_;
    std::unordered_set<std::uint64_t> activeContactPairs_;
    PhysicsStatistics statistics_{};
    std::string lastError_;
    float accumulatorSeconds_{0.0f};

    Body* find_body(BodyId id) noexcept;
    const Body* find_body(BodyId id) const noexcept;
    void simulate(float dt);
    void publish_contacts();

public:
    explicit SimplePhysicsWorld(const PhysicsWorldConfig& config = {});

    PhysicsBackend backend() const noexcept override { return PhysicsBackend::Simple; }
    bool is_available() const noexcept override { return true; }
    const char* backend_name() const noexcept override { return "Simple"; }
    const std::string& last_error() const noexcept override { return lastError_; }

    BodyId create_body(const BodyDesc&) override;
    void destroy_body(BodyId) override;
    bool is_valid(BodyId) const noexcept override;
    bool get_body_state(BodyId, BodyState&) const noexcept override;
    void get_body_ids(std::vector<BodyId>&) const override;
    void get_body_states(std::vector<BodyState>&) const override;

    bool set_body_transform(BodyId, math::Vec3 position, math::Quat rotation,
                            bool wake = true) override;
    bool set_linear_velocity(BodyId, math::Vec3 velocity) override;
    bool set_angular_velocity(BodyId, math::Vec3 velocity) override;
    bool add_force(BodyId, math::Vec3 force) override;
    bool add_torque(BodyId, math::Vec3 torque) override;
    bool set_kinematic_target(BodyId, math::Vec3 position, math::Quat rotation) override;

    void set_gravity(math::Vec3 gravity) noexcept override {
        if (math::IsFinite(gravity.x) && math::IsFinite(gravity.y) && math::IsFinite(gravity.z)) gravity_ = gravity;
    }
    math::Vec3 gravity() const noexcept override { return gravity_; }
    void step(float dt) override;

    bool raycast(const RaycastQuery&, RaycastHit&) const override;
    void set_contact_listener(ContactListener listener) override { contactListener_ = std::move(listener); }
    void drain_contact_events(std::vector<ContactEvent>& output) override;
    const PhysicsStatistics& statistics() const noexcept override { return statistics_; }
};

} // namespace shinkou::physics

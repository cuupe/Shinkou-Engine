#pragma once

#include "shinkou/physics/PhysicsWorld.h"
#include <memory>

namespace shinkou::physics {

class PhysXPhysicsWorld final : public IPhysicsWorld {
public:
    // Definition remains private to the implementation file; the forward
    // declaration is public only so internal helper functions can be kept
    // out of the public API without exposing PhysX headers.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;

public:
    explicit PhysXPhysicsWorld(const PhysicsWorldConfig& config = {});
    ~PhysXPhysicsWorld() override;

    PhysXPhysicsWorld(const PhysXPhysicsWorld&) = delete;
    PhysXPhysicsWorld& operator=(const PhysXPhysicsWorld&) = delete;

    PhysicsBackend backend() const noexcept override { return PhysicsBackend::PhysX; }
    bool is_available() const noexcept override;
    const char* backend_name() const noexcept override { return "PhysX"; }
    const std::string& last_error() const noexcept override;

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

    void set_gravity(math::Vec3 gravity) noexcept override;
    math::Vec3 gravity() const noexcept override;
    void step(float dt) override;

    bool raycast(const RaycastQuery&, RaycastHit&) const override;
    void set_contact_listener(ContactListener listener) override;
    void drain_contact_events(std::vector<ContactEvent>& output) override;
    const PhysicsStatistics& statistics() const noexcept override;
};

} // namespace shinkou::physics

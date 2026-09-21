#include "shinkou/physics/SimplePhysicsWorld.h"
#include "shinkou/MathGeometry.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace shinkou::physics {
namespace {

bool finite_vec3(math::Vec3 value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

bool finite_quat(math::Quat value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z) && math::IsFinite(value.w);
}

bool valid_shape(const ShapeDesc& shape) noexcept {
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

const ShapeDesc& shape_at(const BodyDesc& body, std::size_t index, ShapeDesc& fallback) noexcept {
    if (!body.shapes.empty()) return body.shapes[index];
    fallback = ShapeDesc::box(body.halfExtents);
    fallback.filter = body.filter;
    return fallback;
}

std::uint64_t contact_pair_key(BodyId left, BodyId right) noexcept {
    const auto first = static_cast<std::uint64_t>(std::min(left, right));
    const auto second = static_cast<std::uint64_t>(std::max(left, right));
    return (first << 32u) | second;
}

BodyId contact_pair_left(std::uint64_t key) noexcept {
    return static_cast<BodyId>(key >> 32u);
}

BodyId contact_pair_right(std::uint64_t key) noexcept {
    return static_cast<BodyId>(key & 0xffffffffu);
}

bool filters_allow(const ShapeDesc& left, const ShapeDesc& right) noexcept {
    return (left.filter.layer & right.filter.mask) != 0u &&
           (right.filter.layer & left.filter.mask) != 0u &&
           left.simulationEnabled && right.simulationEnabled;
}

struct BoxContact {
    bool touching{false};
    math::Vec3 normal{};
    float penetration{0.0f};
    math::Vec3 point{};
};

BoxContact box_contact(math::Vec3 leftPosition, const ShapeDesc& leftShape,
                       math::Vec3 rightPosition, const ShapeDesc& rightShape) noexcept {
    const math::Vec3 leftExtents = {
        std::abs(leftShape.halfExtents.x), std::abs(leftShape.halfExtents.y),
        std::abs(leftShape.halfExtents.z)};
    const math::Vec3 rightExtents = {
        std::abs(rightShape.halfExtents.x), std::abs(rightShape.halfExtents.y),
        std::abs(rightShape.halfExtents.z)};
    const math::Vec3 delta = rightPosition - leftPosition;
    const math::Vec3 distance = {std::abs(delta.x), std::abs(delta.y), std::abs(delta.z)};
    const math::Vec3 overlap = {
        leftExtents.x + rightExtents.x - distance.x,
        leftExtents.y + rightExtents.y - distance.y,
        leftExtents.z + rightExtents.z - distance.z};
    if (overlap.x < -1.0e-4f || overlap.y < -1.0e-4f || overlap.z < -1.0e-4f) return {};

    std::uint32_t axis = 0;
    if (overlap.y < overlap.x && overlap.y <= overlap.z) axis = 1;
    else if (overlap.z < overlap.x && overlap.z < overlap.y) axis = 2;
    math::Vec3 normal{};
    if (axis == 0) normal = {delta.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f};
    else if (axis == 1) normal = {0.0f, delta.y >= 0.0f ? 1.0f : -1.0f, 0.0f};
    else normal = {0.0f, 0.0f, delta.z >= 0.0f ? 1.0f : -1.0f};
    return {true, normal, std::max(0.0f, axis == 0 ? overlap.x : axis == 1 ? overlap.y : overlap.z),
            (leftPosition + rightPosition) * 0.5f};
}

} // namespace

SimplePhysicsWorld::SimplePhysicsWorld(const PhysicsWorldConfig& config)
    : gravity_(config.gravity), config_(config) {
    if (!finite_vec3(gravity_)) gravity_ = {0.0f, -9.81f, 0.0f};
    if (!config_.defaultMaterial.valid()) config_.defaultMaterial = {};
    if (!math::IsFinite(config_.fixedTimeStep) || config_.fixedTimeStep <= 0.0f) config_.fixedTimeStep = 1.0f / 60.0f;
    if (config_.maxSubSteps == 0) config_.maxSubSteps = 1;
    if (config_.enableContactEvents) contactEvents_.reserve(config_.contactEventCapacity);
}

SimplePhysicsWorld::Body* SimplePhysicsWorld::find_body(BodyId id) noexcept {
    const auto it = bodyLookup_.find(id);
    if (it == bodyLookup_.end() || it->second >= bodies_.size()) return nullptr;
    Body& body = bodies_[it->second];
    return body.alive ? &body : nullptr;
}

const SimplePhysicsWorld::Body* SimplePhysicsWorld::find_body(BodyId id) const noexcept {
    const auto it = bodyLookup_.find(id);
    if (it == bodyLookup_.end() || it->second >= bodies_.size()) return nullptr;
    const Body& body = bodies_[it->second];
    return body.alive ? &body : nullptr;
}

BodyId SimplePhysicsWorld::create_body(const BodyDesc& input) {
    if (!finite_vec3(input.position) || !finite_vec3(input.velocity) || !finite_vec3(input.halfExtents) ||
        !finite_quat(input.rotation) || !finite_vec3(input.angularVelocity) || !math::IsFinite(input.mass) ||
        input.mass < 0.0f) {
        lastError_ = "invalid body description";
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
    for (const ShapeDesc& shape : desc.shapes) {
        if (!valid_shape(shape)) {
            lastError_ = "invalid shape description";
            return InvalidBodyId;
        }
    }
    for (ShapeDesc& shape : desc.shapes) {
        if (shape.filter.layer == 1u && shape.filter.mask == 0xffffffffu) shape.filter = desc.filter;
        else {
            shape.filter.layer &= desc.filter.layer;
            shape.filter.mask &= desc.filter.mask;
        }
        if (!shape.material.valid()) shape.material = config_.defaultMaterial;
    }

    BodyId id = nextId_++;
    while (id == InvalidBodyId || bodyLookup_.find(id) != bodyLookup_.end()) {
        if (nextId_ == InvalidBodyId) nextId_ = 1;
        id = nextId_++;
    }
    bodyLookup_[id] = bodies_.size();
    bodies_.push_back(Body{id, std::move(desc), {}, {}, true, input.startAwake});
    statistics_.bodyCount++;
    if (input.dynamic) statistics_.dynamicBodyCount++;
    lastError_.clear();
    return id;
}

void SimplePhysicsWorld::destroy_body(BodyId id) {
    const auto it = bodyLookup_.find(id);
    if (it == bodyLookup_.end()) return;
    Body& body = bodies_[it->second];
    if (body.alive) {
        body.alive = false;
        if (body.desc.dynamic) {
            if (statistics_.dynamicBodyCount > 0) --statistics_.dynamicBodyCount;
            if (body.awake && statistics_.activeDynamicBodyCount > 0) --statistics_.activeDynamicBodyCount;
        }
        if (statistics_.bodyCount > 0) --statistics_.bodyCount;
    }
    bodyLookup_.erase(it);
    for (auto contact = activeContactPairs_.begin(); contact != activeContactPairs_.end();) {
        if (contact_pair_left(*contact) == id || contact_pair_right(*contact) == id)
            contact = activeContactPairs_.erase(contact);
        else
            ++contact;
    }
}

bool SimplePhysicsWorld::is_valid(BodyId id) const noexcept { return find_body(id) != nullptr; }

bool SimplePhysicsWorld::get_body_state(BodyId id, BodyState& output) const noexcept {
    const Body* body = find_body(id);
    if (!body) return false;
    output.body = body->id;
    output.position = body->desc.position;
    output.rotation = body->desc.rotation;
    output.linearVelocity = body->desc.velocity;
    output.angularVelocity = body->desc.angularVelocity;
    output.userData = body->desc.userData;
    output.dynamic = body->desc.dynamic;
    output.kinematic = body->desc.kinematic;
    output.awake = body->awake;
    output.enabled = body->desc.enabled;
    return true;
}

void SimplePhysicsWorld::get_body_ids(std::vector<BodyId>& output) const {
    output.clear();
    output.reserve(statistics_.bodyCount);
    for (const Body& body : bodies_) if (body.alive) output.push_back(body.id);
}

void SimplePhysicsWorld::get_body_states(std::vector<BodyState>& output) const {
    output.clear();
    output.reserve(statistics_.bodyCount);
    for (const Body& body : bodies_) {
        if (!body.alive) continue;
        BodyState state;
        state.body = body.id;
        state.position = body.desc.position;
        state.rotation = body.desc.rotation;
        state.linearVelocity = body.desc.velocity;
        state.angularVelocity = body.desc.angularVelocity;
        state.userData = body.desc.userData;
        state.dynamic = body.desc.dynamic;
        state.kinematic = body.desc.kinematic;
        state.awake = body.awake;
        state.enabled = body.desc.enabled;
        output.push_back(state);
    }
}

bool SimplePhysicsWorld::set_body_transform(BodyId id, math::Vec3 position, math::Quat rotation, bool wake) {
    if (!finite_vec3(position) || !finite_quat(rotation)) {
        lastError_ = "invalid body transform";
        return false;
    }
    Body* body = find_body(id);
    if (!body) return false;
    body->desc.position = position;
    body->desc.rotation = math::Normalize(rotation);
    if (wake && body->desc.dynamic && !body->awake) {
        body->awake = true;
        ++statistics_.activeDynamicBodyCount;
    }
    return true;
}

bool SimplePhysicsWorld::set_linear_velocity(BodyId id, math::Vec3 velocity) {
    if (!finite_vec3(velocity)) return false;
    Body* body = find_body(id);
    if (!body || !body->desc.dynamic || body->desc.kinematic) return false;
    body->desc.velocity = velocity;
    body->awake = true;
    return true;
}

bool SimplePhysicsWorld::set_angular_velocity(BodyId id, math::Vec3 velocity) {
    if (!finite_vec3(velocity)) return false;
    Body* body = find_body(id);
    if (!body || !body->desc.dynamic || body->desc.kinematic) return false;
    body->desc.angularVelocity = velocity;
    body->awake = true;
    return true;
}

bool SimplePhysicsWorld::add_force(BodyId id, math::Vec3 force) {
    if (!finite_vec3(force)) return false;
    Body* body = find_body(id);
    if (!body || !body->desc.dynamic || body->desc.kinematic) return false;
    body->force += force;
    body->awake = true;
    return true;
}

bool SimplePhysicsWorld::add_torque(BodyId id, math::Vec3 torque) {
    if (!finite_vec3(torque)) return false;
    Body* body = find_body(id);
    if (!body || !body->desc.dynamic || body->desc.kinematic) return false;
    body->torque += torque;
    body->awake = true;
    return true;
}

bool SimplePhysicsWorld::set_kinematic_target(BodyId id, math::Vec3 position, math::Quat rotation) {
    Body* body = find_body(id);
    if (!body || !body->desc.dynamic || !body->desc.kinematic || !finite_vec3(position) || !finite_quat(rotation)) return false;
    body->desc.position = position;
    body->desc.rotation = math::Normalize(rotation);
    body->awake = true;
    return true;
}

void SimplePhysicsWorld::simulate(float dt) {
    for (Body& body : bodies_) {
        if (!body.alive || !body.desc.enabled || !body.desc.dynamic) continue;
        if (!body.desc.kinematic) {
            const float inverseMass = body.desc.mass > math::Epsilon ? 1.0f / body.desc.mass : 0.0f;
            body.desc.velocity += (gravity_ + body.force * inverseMass) * dt;
            body.desc.position += body.desc.velocity * dt;
            body.desc.angularVelocity += body.torque * (inverseMass * dt);
            const math::Quat angular{body.desc.angularVelocity.x, body.desc.angularVelocity.y,
                                     body.desc.angularVelocity.z, 0.0f};
            body.desc.rotation = math::Normalize(body.desc.rotation + math::Multiply(angular, body.desc.rotation) * (0.5f * dt));

            const float linearDamping = std::exp(-body.desc.linearDamping * dt);
            const float angularDamping = std::exp(-body.desc.angularDamping * dt);
            body.desc.velocity *= linearDamping;
            body.desc.angularVelocity *= angularDamping;

            ShapeDesc fallback;
            const ShapeDesc& shape = shape_at(body.desc, 0, fallback);
            if (shape.type == ShapeType::Box && body.desc.position.y - std::abs(shape.halfExtents.y) < 0.0f) {
                body.desc.position.y = std::abs(shape.halfExtents.y);
                if (body.desc.velocity.y < 0.0f) body.desc.velocity.y *= -shape.material.restitution;
            }
            const float speedSquared = math::LengthSquared(body.desc.velocity);
            if (speedSquared <= 1.0e-8f && math::LengthSquared(body.force) <= 1.0e-8f) body.awake = false;
        }
        body.force = {};
        body.torque = {};
    }

    std::unordered_set<std::uint64_t> currentContactPairs;
    for (std::size_t leftIndex = 0; leftIndex < bodies_.size(); ++leftIndex) {
        Body& left = bodies_[leftIndex];
        if (!left.alive || !left.desc.enabled) continue;
        ShapeDesc leftFallback;
        const ShapeDesc& leftShape = shape_at(left.desc, 0, leftFallback);
        if (leftShape.type != ShapeType::Box) continue;

        for (std::size_t rightIndex = leftIndex + 1; rightIndex < bodies_.size(); ++rightIndex) {
            Body& right = bodies_[rightIndex];
            if (!right.alive || !right.desc.enabled ||
                (!left.desc.dynamic && !right.desc.dynamic)) continue;
            ShapeDesc rightFallback;
            const ShapeDesc& rightShape = shape_at(right.desc, 0, rightFallback);
            if (rightShape.type != ShapeType::Box || !filters_allow(leftShape, rightShape)) continue;

            const BoxContact contact = box_contact(left.desc.position, leftShape,
                                                   right.desc.position, rightShape);
            if (!contact.touching) continue;
            const auto pairKey = contact_pair_key(left.id, right.id);
            if (config_.enableContactEvents) currentContactPairs.insert(pairKey);
            const auto phase = config_.enableContactEvents && activeContactPairs_.count(pairKey) != 0
                ? ContactPhase::Persist : ContactPhase::Begin;

            const bool leftDynamic = left.desc.dynamic && !left.desc.kinematic;
            const bool rightDynamic = right.desc.dynamic && !right.desc.kinematic;
            const float leftInverseMass = leftDynamic && left.desc.mass > math::Epsilon
                ? 1.0f / left.desc.mass : 0.0f;
            const float rightInverseMass = rightDynamic && right.desc.mass > math::Epsilon
                ? 1.0f / right.desc.mass : 0.0f;
            const float inverseMassSum = leftInverseMass + rightInverseMass;
            const float relativeNormalVelocity = math::Dot(right.desc.velocity - left.desc.velocity,
                                                           contact.normal);
            float impulseMagnitude = 0.0f;
            if (inverseMassSum > math::Epsilon && relativeNormalVelocity < 0.0f) {
                const float restitution = std::clamp(
                    std::min(leftShape.material.restitution, rightShape.material.restitution),
                    0.0f, 1.0f);
                impulseMagnitude = -(1.0f + restitution) * relativeNormalVelocity / inverseMassSum;
                const math::Vec3 impulse = contact.normal * impulseMagnitude;
                if (leftDynamic) left.desc.velocity -= impulse * leftInverseMass;
                if (rightDynamic) right.desc.velocity += impulse * rightInverseMass;
            }

            if (inverseMassSum > math::Epsilon && contact.penetration > 0.0f) {
                const math::Vec3 correction = contact.normal * (contact.penetration / inverseMassSum);
                if (leftDynamic) left.desc.position -= correction * leftInverseMass;
                if (rightDynamic) right.desc.position += correction * rightInverseMass;
            }
            if (leftDynamic) left.awake = true;
            if (rightDynamic) right.awake = true;
            if (config_.enableContactEvents && contactEvents_.size() < config_.contactEventCapacity) {
                ContactEvent event;
                event.bodyA = left.id;
                event.bodyB = right.id;
                event.point = contact.point;
                event.normal = contact.normal;
                event.impulse = contact.normal * impulseMagnitude;
                event.separation = -contact.penetration;
                event.phase = phase;
                contactEvents_.push_back(event);
            }
        }
    }

    for (const auto pairKey : activeContactPairs_) {
        if (!config_.enableContactEvents || currentContactPairs.count(pairKey) != 0 ||
            contactEvents_.size() >= config_.contactEventCapacity) continue;
        ContactEvent event;
        event.bodyA = contact_pair_left(pairKey);
        event.bodyB = contact_pair_right(pairKey);
        event.phase = ContactPhase::End;
        contactEvents_.push_back(event);
    }
    if (config_.enableContactEvents) activeContactPairs_ = std::move(currentContactPairs);
    else activeContactPairs_.clear();

    std::size_t activeDynamic = 0;
    for (const Body& body : bodies_) {
        if (body.alive && body.desc.enabled && body.desc.dynamic && body.awake) ++activeDynamic;
    }
    statistics_.activeDynamicBodyCount = activeDynamic;
    ++statistics_.simulationSteps;
    statistics_.simulatedSeconds += dt;
}

void SimplePhysicsWorld::publish_contacts() {
    statistics_.lastContactEventCount = contactEvents_.size();
    if (!config_.enableContactEvents || !contactListener_) return;
    for (const ContactEvent& event : contactEvents_) contactListener_(event);
}

void SimplePhysicsWorld::step(float dt) {
    contactEvents_.clear();
    statistics_.lastContactEventCount = 0;
    if (!math::IsFinite(dt) || dt <= 0.0f) {
        lastError_ = "step duration must be finite and positive";
        return;
    }
    const float clampedDt = std::min(dt, config_.fixedTimeStep * static_cast<float>(config_.maxSubSteps));
    if (!config_.useFixedTimeStep) {
        simulate(clampedDt);
    } else {
        accumulatorSeconds_ += clampedDt;
        std::uint32_t subSteps = 0;
        while (accumulatorSeconds_ + 1.0e-7f >= config_.fixedTimeStep && subSteps++ < config_.maxSubSteps) {
            simulate(config_.fixedTimeStep);
            accumulatorSeconds_ -= config_.fixedTimeStep;
        }
    }
    statistics_.accumulatorSeconds = accumulatorSeconds_;
    publish_contacts();
    lastError_.clear();
}

bool SimplePhysicsWorld::raycast(const RaycastQuery& query, RaycastHit& hit) const {
    if (!finite_vec3(query.origin) || !finite_vec3(query.direction) || !math::IsFinite(query.maxDistance) ||
        query.maxDistance <= 0.0f) return false;
    const float directionLength = math::Length(query.direction);
    if (!math::IsFinite(directionLength) || directionLength <= math::Epsilon) return false;
    const math::Vec3 direction = query.direction / directionLength;
    float closest = query.maxDistance;
    bool found = false;

    for (const Body& body : bodies_) {
        if (!body.alive || !body.desc.enabled ||
            (body.desc.dynamic ? !query.includeDynamic : !query.includeStatic)) continue;
        const std::size_t shapeCount = body.desc.shapes.empty() ? 1u : body.desc.shapes.size();
        for (std::size_t shapeIndex = 0; shapeIndex < shapeCount; ++shapeIndex) {
            ShapeDesc fallback;
            const ShapeDesc& shape = shape_at(body.desc, shapeIndex, fallback);
            if (!shape.queryEnabled || (shape.filter.layer & query.layerMask) == 0u) continue;
            const math::Vec3 center = body.desc.position + math::Rotate(body.desc.rotation, shape.localPosition);
            const math::Quat rotation = math::Normalize(body.desc.rotation * shape.localRotation);
            float distance = 0.0f;
            math::Vec3 point{}, normal{};
            bool intersects = false;
            switch (shape.type) {
            case ShapeType::Box:
                intersects = math::RaycastObb({query.origin, direction}, {{center, rotation, {1.0f, 1.0f, 1.0f}}, shape.halfExtents}, distance, &point, &normal);
                break;
            case ShapeType::Sphere:
                intersects = math::RaycastSphere({query.origin, direction}, {center, std::abs(shape.radius)}, distance, &point);
                normal = math::Normalize(point - center);
                break;
            case ShapeType::Capsule:
                intersects = math::RaycastObb({query.origin, direction}, {{center, rotation, {1.0f, 1.0f, 1.0f}}, {std::abs(shape.radius), std::abs(shape.halfHeight) + std::abs(shape.radius), std::abs(shape.radius)}}, distance, &point, &normal);
                break;
            case ShapeType::Plane: {
                const math::Vec3 planeNormal = math::Normalize(math::Rotate(rotation, shape.planeNormal));
                intersects = math::RaycastPlane({query.origin, direction}, math::Plane::FromPointNormal(center, planeNormal), distance, &point);
                normal = planeNormal;
                break;
            }
            case ShapeType::ConvexMesh:
            case ShapeType::TriangleMesh: {
                if (shape.vertices.empty()) break;
                math::Vec3 minimum = shape.vertices.front();
                math::Vec3 maximum = minimum;
                for (const math::Vec3 vertex : shape.vertices) {
                    minimum = {std::min(minimum.x, vertex.x), std::min(minimum.y, vertex.y), std::min(minimum.z, vertex.z)};
                    maximum = {std::max(maximum.x, vertex.x), std::max(maximum.y, vertex.y), std::max(maximum.z, vertex.z)};
                }
                const math::Vec3 boundsCenter = center + math::Rotate(rotation, (minimum + maximum) * 0.5f);
                const math::Vec3 extents = (maximum - minimum) * 0.5f;
                intersects = math::RaycastObb({query.origin, direction}, {{boundsCenter, rotation, {1.0f, 1.0f, 1.0f}}, extents}, distance, &point, &normal);
                break;
            }
            }
            if (intersects && distance >= 0.0f && distance <= closest) {
                closest = distance;
                hit = {body.id, point, normal, distance, static_cast<ShapeIndex>(shapeIndex), body.desc.userData};
                found = true;
            }
        }
    }
    return found;
}

void SimplePhysicsWorld::drain_contact_events(std::vector<ContactEvent>& output) {
    output = std::move(contactEvents_);
    contactEvents_.clear();
    if (config_.enableContactEvents) contactEvents_.reserve(config_.contactEventCapacity);
}

} // namespace shinkou::physics

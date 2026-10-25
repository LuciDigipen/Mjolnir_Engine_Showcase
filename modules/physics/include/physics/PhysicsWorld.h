/**
 * @file PhysicsWorld.h
 * @author Jackey Cen
 * @brief A rigid body simulation: create bodies, then Step() it.
 *
 * Jolt lives entirely behind the pimpl. Several worlds can exist at once; the
 * process-wide Jolt globals behind them are reference counted (JoltRuntime.h).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics/CollisionEvents.h>
#include <physics/HeightField.h>
#include <physics/PhysicsTypes.h>

#include <core/ecs/Entity.h>
#include <core/math/WorldCoordinates.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace mir
{
    struct BodyDesc
    {
        PhysicsMotion mMotion = PhysicsMotion::Dynamic;
        PhysicsShape mShape = PhysicsShape::Box;
        WorldPosition mPosition;
        glm::quat mRotation = kIdentityRotation;
        // Only the fields the shape uses are read.
        glm::vec3 mHalfExtent{0.5f};
        float mRadius = 0.5f;
        // Capsule: half the total height, caps included. Raised to the radius when smaller.
        float mHalfHeight = 1.0f;
        PhysicsAxis mAxis = PhysicsAxis::Y;
        // Where the shape sits in the body's own frame.
        glm::vec3 mOffset{0.0f};
        glm::quat mRotationOffset = kIdentityRotation;
        // Never blocks motion; reports contacts like any other body.
        bool mIsTrigger = false;
        // Pairs this body touches also get Stay events.
        bool mDetectStay = false;
        // Reads only for Dynamic: a mass at or below 0 leaves the shape's own, from its volume.
        float mMass = 0.0f;
        float mGravityFactor = 1.0f;
        float mFriction = 0.2f;
        float mRestitution = 0.0f;
        bool mAllowSleeping = true;
    };

    struct RayHit
    {
        BodyHandle mBody;
        WorldPosition mPoint;
        glm::vec3 mNormal{0.0f};
        float mDistance = 0.0f;
    };

    struct PhysicsWorldDesc
    {
        glm::vec3 mGravity{0.0f, -9.81f, 0.0f};

        uint32_t mMaxBodies = 1024;
        uint32_t mMaxBodyPairs = 1024;
        uint32_t mMaxContactConstraints = 1024;

        uint32_t mWorkerThreads = 0;

        int mCollisionSteps = 1;
    };

    class PhysicsWorld
    {
      public:
        explicit PhysicsWorld(const PhysicsWorldDesc &desc = {});
        ~PhysicsWorld();

        PhysicsWorld(const PhysicsWorld &) = delete;
        PhysicsWorld &operator=(const PhysicsWorld &) = delete;
        PhysicsWorld(PhysicsWorld &&) = delete;
        PhysicsWorld &operator=(PhysicsWorld &&) = delete;

        // Bodies live in the fixed Scene world coordinate system. Invalid handle (logged) on a non-positive size.
        BodyHandle CreateBody(const BodyDesc &desc);

        // Builds the shape once; Step never rebuilds it. Invalid handle (logged) on bad input.
        BodyHandle CreateStaticHeightField(const HeightFieldDesc &desc);
        // A static body at origin for a shape built elsewhere; between steps only.
        BodyHandle CreateStaticHeightField(const SharedHeightField &shape, WorldPosition origin);

        // Between steps only. Wakes bodies resting on it.
        void DestroyBody(BodyHandle body);

        // A restricted static body collides only with the dynamic bodies allowed to touch it.
        void RestrictBody(BodyHandle staticBody);
        // The restricted bodies this dynamic body may touch from the next step on; it passes through the others.
        void RestrictContacts(BodyHandle dynamicBody, std::span<const BodyHandle> allowed);
        // Lets the body touch every restricted body again.
        void ClearContactRestriction(BodyHandle dynamicBody);

        // Takes a dynamic body out of the simulation: steps keep its pose and its velocities wait for
        // ResumeBody. While held, velocity changes and impulses land in the kept velocities; forces are dropped.
        void HoldBody(BodyHandle body);
        void ResumeBody(BodyHandle body);
        bool IsBodyHeld(BodyHandle body) const;

        glm::vec3 Gravity() const;

        // nullopt for an invalid handle, or if the simulation left the body at
        // a non-finite position; there is no default place to report instead.
        std::optional<WorldPosition> BodyPosition(BodyHandle body) const;
        glm::quat BodyRotation(BodyHandle body) const;
        glm::vec3 BodyLinearVelocity(BodyHandle body) const;
        glm::vec3 BodyAngularVelocity(BodyHandle body) const;
        void SetBodyTransform(BodyHandle body, WorldPosition position, const glm::quat &rotation);
        // A Dynamic or Kinematic body that rests, or that has not been touched for a while, falls asleep.
        void WakeBody(BodyHandle body);
        // Whether the pairs this body touches get Stay events, once per frame.
        void SetBodyDetectsStay(BodyHandle body, bool detects);
        bool IsBodySleeping(BodyHandle body) const;
        // Kinematic only: sets the velocity that reaches the pose after dt, so dynamic bodies in the way are pushed
        // and not skipped over. The pose is reached at the end of the next step; the velocity is left behind.
        void MoveBodyKinematic(BodyHandle body, WorldPosition position, const glm::quat &rotation, float dt);
        void SetBodyLinearVelocity(BodyHandle body, const glm::vec3 &velocity);
        void SetBodyAngularVelocity(BodyHandle body, const glm::vec3 &velocity);
        void AddLinearVelocity(BodyHandle body, const glm::vec3 &velocity);
        void AddAngularVelocity(BodyHandle body, const glm::vec3 &velocity);

        // Force and torque accumulate until the next Step() consumes and clears
        // them, so a continuous push has to be re-applied every step.
        void AddForce(BodyHandle body, const glm::vec3 &force);
        void AddTorque(BodyHandle body, const glm::vec3 &torque);
        void AddImpulse(BodyHandle body, const glm::vec3 &impulse);
        void AddAngularImpulse(BodyHandle body, const glm::vec3 &angularImpulse);
        void AddForceAtOffset(BodyHandle body, const glm::vec3 &force, const glm::vec3 &localOffset);
        void AddImpulseAtOffset(BodyHandle body, const glm::vec3 &impulse, const glm::vec3 &localOffset);

        // kNullEntity for an untagged body or a bad handle; the entity is not checked for liveness.
        void SetBodyEntity(BodyHandle body, Entity entity);
        Entity BodyEntity(BodyHandle body) const;

        // Closest hit within maxDistance. Jolt's float fraction resolves ~6e-8 * maxDistance.
        // nullopt, logged, for a zero or non-finite direction or a non-positive maxDistance.
        std::optional<RayHit> RayCast(WorldPosition origin, const glm::vec3 &direction, float maxDistance) const;

        // nullopt unless body is a live height field.
        std::optional<HeightFieldInfo> HeightFieldInfoOf(BodyHandle body) const;

        std::size_t HeightFieldsBuilt() const;

        std::size_t BodyCount() const;

        // True from the PlayBegin pass until PlayEnd; the RigidBody add hook creates bodies only while it is.
        bool IsSimulationLive() const { return mSimulationLive; }
        void SetSimulationLive(bool live) { mSimulationLive = live; }

        // True only while Step runs, when bodies must not be created.
        bool IsStepping() const { return mStepping; }

        // Advances by dt seconds. Non-positive dt is a no-op, not an error.
        void Step(float dt);

        // Oldest first; events pile up until drained.
        std::vector<CollisionEvent> DrainCollisionEvents();

      private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
        bool mSimulationLive = false;
        bool mStepping = false;
    };
}

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
#include <physics_layers/PhysicsLayers.h>
#include <physics/PhysicsTypes.h>

#include <core/ecs/Entity.h>
#include <core/math/WorldCoordinates.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
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
        // Which layer the body is on, 0 to 31; out of range is refused with a logged reason.
        int mLayer = 0;
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
        // Jolt's own defaults, so an authored body keeps the motion it had before they were exposed.
        float mLinearDamping = 0.05f;
        float mAngularDamping = 0.05f;
        bool mGyroscopicForce = false;
        PhysicsMotionQuality mMotionQuality = PhysicsMotionQuality::Discrete;
        // Takes the inertia tensor from mInertiaDiagonal and mInertiaProducts instead of the shape's.
        bool mCustomInertia = false;
        // Moments about the body's own axes: Ixx, Iyy, Izz, in kg m^2.
        glm::vec3 mInertiaDiagonal{1.0f, 1.0f, 1.0f};
        // Products of inertia Ixy, Ixz, Iyz; the tensor is [[Ixx,-Ixy,-Ixz],[-Ixy,Iyy,-Iyz],[-Ixz,-Iyz,Izz]].
        glm::vec3 mInertiaProducts{0.0f};
        // Places the centre of mass away from the shape's centre, with the shape and the Transform staying where they are.
        bool mCustomCenterOfMass = false;
        // Centre of mass from the body's origin in its own axes, in metres; the shape's offset does not move it.
        glm::vec3 mCenterOfMassOffset{0.0f};
    };

    struct ColliderView;

    // The collider CreateBody would build from desc, posed in world space; nullopt where it would build an empty
    // one. For drawing colliders that have no body yet.
    std::optional<ColliderView> ColliderViewOf(const BodyDesc &desc);

    // What a ray cast may hit. The defaults hit every layer and motion type, skip triggers and exclude nothing.
    struct RayFilter
    {
        // Bit i asks for layer i.
        uint32_t mLayers = 0xFFFFFFFFu;
        // MotionBit of each motion type asked for.
        uint32_t mMotions = kAllMotions;
        bool mHitTriggers = false;
        // Bodies the cast passes through, the caster's own among them.
        std::span<const BodyHandle> mExclude;
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

        // -1 selects the bounded engine default; 0 runs on the calling thread.
        int mWorkerThreads = -1;

        int mCollisionSteps = 1;
    };

    class JobExecutor;

    struct BodyPoseSnapshot
    {
        std::optional<WorldPosition> position;
        glm::quat rotation = kIdentityRotation;
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

        // Bodies live in the fixed Scene world coordinate system. A size that is not above 0 gives a body with an empty collider (logged).
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
        uint32_t WorkerCount() const;

        // Between steps, with body lifetime/mutations paused until return. Reads
        // independent bodies under Jolt read locks; joins before returning.
        // Invalid/stale handles produce an empty position; spans must match size.
        void ReadBodyPoses(std::span<const BodyHandle> bodies, std::span<BodyPoseSnapshot> poses,
            JobExecutor *jobs = nullptr) const;

        // nullopt for an invalid handle, or if the simulation left the body at
        // a non-finite position; there is no default place to report instead.
        std::optional<WorldPosition> BodyPosition(BodyHandle body) const;
        glm::quat BodyRotation(BodyHandle body) const;
        glm::vec3 BodyLinearVelocity(BodyHandle body) const;
        glm::vec3 BodyAngularVelocity(BodyHandle body) const;
        void SetBodyTransform(BodyHandle body, WorldPosition position, const glm::quat &rotation);
        // In-place edits: the handle, the contacts and the Restrict lists all stay. Velocity follows Jolt, so it is kept
        // going to Kinematic or Dynamic and dropped going to Static. A held body is left alone.
        void SetBodyMotion(BodyHandle body, PhysicsMotion motion);
        // Replaces the shape from the shape fields of `desc` (kind, sizes, axis, offset, rotation offset); a Dynamic
        // body keeps desc.mMass.
        void SetBodyShape(BodyHandle body, const BodyDesc &desc);
        void SetBodyTrigger(BodyHandle body, bool trigger);
        // Moves the body to another layer in place, keeping whether it moves.
        void SetBodyLayer(BodyHandle body, int layer);
        void SetBodyMaterial(BodyHandle body, float friction, float restitution);
        void SetBodyGravityFactor(BodyHandle body, float factor);
        // Scales the inertia with it; a mass at or below 0 is ignored. A supplied inertia tensor is absolute and
        // does not scale, so only the mass changes.
        void SetBodyMass(BodyHandle body, float mass);
        // Puts the mass rules of desc on the body's current shape, without rebuilding it. A supplied inertia tensor
        // replaces the shape's; without one the shape's inertia is scaled to desc.mMass, as SetBodyMass does.
        void SetBodyMassProperties(BodyHandle body, const BodyDesc &desc);
        void SetBodyAllowSleeping(BodyHandle body, bool allow);
        // How fast a body loses velocity on its own; a negative value is taken as 0. A Static body has none until it
        // moves.
        void SetBodyDamping(BodyHandle body, float linear, float angular);
        // Keeps a spinning body's angular momentum fixed in world space instead of letting its angular velocity drift.
        void SetBodyGyroscopicForce(BodyHandle body, bool apply);
        // LinearCast sweeps the body between steps instead of only sampling its ends.
        void SetBodyMotionQuality(BodyHandle body, PhysicsMotionQuality quality);

        // What the body holds now; a Static body has no motion state and answers the defaults.
        float BodyLinearDamping(BodyHandle body) const;
        float BodyAngularDamping(BodyHandle body) const;
        bool BodyGyroscopicForce(BodyHandle body) const;
        PhysicsMotionQuality BodyMotionQuality(BodyHandle body) const;

        // A Dynamic or Kinematic body that rests, or that has not been touched for a while, falls asleep.
        void WakeBody(BodyHandle body);
        // Whether the pairs this body touches get Stay events, once per frame.
        void SetBodyDetectsStay(BodyHandle body, bool detects);
        bool IsBodySleeping(BodyHandle body) const;
        // Kinematic only: sets the velocity that reaches the pose after dt, so dynamic bodies in the way are pushed
        // and not skipped over. The pose is reached at the end of the next step; the velocity is left behind.
        void MoveBodyKinematic(BodyHandle body, WorldPosition position, const glm::quat &rotation, float dt);
        // These return whether the body took the call. False, with the reason logged, for a stale handle, a vector
        // that is not finite, a force, torque or impulse on a body that is not Dynamic (a held body takes impulses and
        // velocities into the velocities it keeps, not forces), or a velocity on a Static body.
        bool SetBodyLinearVelocity(BodyHandle body, const glm::vec3 &velocity);
        bool SetBodyAngularVelocity(BodyHandle body, const glm::vec3 &velocity);
        bool AddLinearVelocity(BodyHandle body, const glm::vec3 &velocity);
        bool AddAngularVelocity(BodyHandle body, const glm::vec3 &velocity);

        // Force and torque accumulate until the next Step() consumes and clears
        // them, so a continuous push has to be re-applied every step.
        bool AddForce(BodyHandle body, const glm::vec3 &force);
        bool AddTorque(BodyHandle body, const glm::vec3 &torque);
        bool AddImpulse(BodyHandle body, const glm::vec3 &impulse);
        bool AddAngularImpulse(BodyHandle body, const glm::vec3 &angularImpulse);
        // The offset is in the body's own axes, measured from the centre of mass.
        bool AddForceAtOffset(BodyHandle body, const glm::vec3 &force, const glm::vec3 &localOffset);
        bool AddImpulseAtOffset(BodyHandle body, const glm::vec3 &impulse, const glm::vec3 &localOffset);

        // kNullEntity for an untagged body or a bad handle; the entity is not checked for liveness.
        void SetBodyEntity(BodyHandle body, Entity entity);
        Entity BodyEntity(BodyHandle body) const;

        // Closest hit within maxDistance. Jolt's float fraction resolves ~6e-8 * maxDistance.
        // nullopt, logged, for a zero or non-finite direction or a non-positive maxDistance.
        // An empty layer or motion set is refused with a logged reason, like a bad ray.
        std::optional<RayHit> RayCast(WorldPosition origin, const glm::vec3 &direction, float maxDistance,
            const RayFilter &filter = {}) const;

        // nullopt unless body is a live height field.
        std::optional<HeightFieldInfo> HeightFieldInfoOf(BodyHandle body) const;

        std::size_t HeightFieldsBuilt() const;

        std::size_t BodyCount() const;

        // Every body's collider as Jolt holds it, at its simulated pose; height fields come thinned to a grid of at
        // most kColliderGridLines per side, whose points live only for the visit. Between steps only. An empty shape
        // is skipped. visit runs under the body's read lock, so it must not call back into this world.
        void ForEachCollider(const std::function<void(const ColliderView &)> &visit) const;
        static constexpr uint32_t kColliderGridLines = 64;

        // Which layers collide. Between steps only; every body is woken, since a pair that rested may not any more.
        void SetLayerTable(const PhysicsLayerTable &table);
        const PhysicsLayerTable &LayerTable() const;

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

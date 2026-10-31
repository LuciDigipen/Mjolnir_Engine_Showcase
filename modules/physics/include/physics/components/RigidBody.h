/**
 * @file RigidBody.h
 * @author Jackey Cen
 * @brief Gives an entity a body in the World's PhysicsWorld.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics/PhysicsTypes.h>

#include <core/ecs/ComponentAsserts.h>
#include <core/ecs/ComponentFields.h>
#include <core/math/WorldReal.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cfloat>
#include <cstddef>
#include <cstdint>

namespace mir
{
    // The authored fields a Jolt body was last built or updated from, one per field so that comparing
    // two of them cannot trip over padding. `interpolate` is not here: the interpolation system reads it live.
    struct AppliedBody
    {
        PhysicsMotion motion = PhysicsMotion::Dynamic;
        PhysicsShape shape = PhysicsShape::Box;
        glm::vec3 halfExtent{ 0.5f, 0.5f, 0.5f };
        float radius = 0.5f;
        float halfHeight = 1.0f;
        PhysicsAxis axis = PhysicsAxis::Y;
        glm::vec3 offset{ 0.0f };
        glm::quat rotationOffset = kIdentityRotation;
        bool isTrigger = false;
        bool detectStay = false;
        float mass = 1.0f;
        float gravityFactor = 1.0f;
        float friction = 0.2f;
        float restitution = 0.0f;
        bool allowSleeping = true;
        int layer = 0;

        friend bool operator==(const AppliedBody &, const AppliedBody &) = default;
    };

    struct RigidBody
    {
        PhysicsMotion motion = PhysicsMotion::Dynamic;
        PhysicsShape shape = PhysicsShape::Box;
        glm::vec3 halfExtent{ 0.5f, 0.5f, 0.5f };
        float radius = 0.5f;
        // Capsule: half the total height, caps included, along `axis`.
        float halfHeight = 1.0f;
        PhysicsAxis axis = PhysicsAxis::Y;
        // Where the collider sits relative to the entity's Transform, in the entity's own axes.
        glm::vec3 offset{ 0.0f };
        glm::quat rotationOffset = kIdentityRotation;
        bool interpolate = false;
        // Reports contacts but never blocks motion.
        bool isTrigger = false;
        // Also report Stay events for the pairs this body touches.
        bool detectStay = false;
        float mass = 1.0f;
        float gravityFactor = 1.0f;
        float friction = 0.2f;
        float restitution = 0.0f;
        bool allowSleeping = true;
        // Which layer the collider is on, 0 to 31: what it may collide with, and what a ray cast can ask for.
        int layer = 0;

        // Runtime state PhysicsSystem keeps; not in the field table, so never saved.
        struct Runtime
        {
            BodyHandle body;
            // The last two fixed-step poses in world space, which interpolation blends.
            WorldVec3 previousPosition{ 0 };
            WorldVec3 currentPosition{ 0 };
            glm::quat previousRotation = kIdentityRotation;
            glm::quat currentRotation = kIdentityRotation;
            // The world pose the entity had when physics last placed it. A different resolved pose
            // later means something else moved it, its own Transform or any ancestor's.
            WorldVec3 stampedPosition{ 0 };
            glm::quat stampedRotation = kIdentityRotation;
            // What the body was last built or updated from, so an edit shows as a difference.
            AppliedBody applied;
            // The world scale the shape was built from, so a scale change is a shape change.
            glm::vec3 builtScale{ 1.0f };
            // A Kinematic body's first Transform write teleports it, later ones move it to the target.
            bool hasMoved = false;
            // Moved to a target this step, so its velocity is cleared once the step has reached it.
            bool movedToTarget = false;
        };

        Runtime runtime;
    };

    MIR_ASSERT_COMPONENT(RigidBody);

    inline AppliedBody AppliedOf(const RigidBody &rigidBody)
    {
        return { rigidBody.motion, rigidBody.shape, rigidBody.halfExtent, rigidBody.radius, rigidBody.halfHeight,
            rigidBody.axis, rigidBody.offset, rigidBody.rotationOffset, rigidBody.isTrigger, rigidBody.detectStay,
            rigidBody.mass, rigidBody.gravityFactor, rigidBody.friction, rigidBody.restitution, rigidBody.allowSleeping,
            rigidBody.layer };
    }

    // A body the simulation moves, as opposed to one it only collides against.
    inline bool HasDynamicBody(const RigidBody &rigidBody)
    {
        return rigidBody.runtime.body.IsValid() && rigidBody.motion == PhysicsMotion::Dynamic;
    }

    // A body the step moves, dynamic or kinematic, whose pose is written back to the Transform.
    inline bool HasMovingBody(const RigidBody &rigidBody)
    {
        return rigidBody.runtime.body.IsValid()
            && (rigidBody.motion == PhysicsMotion::Dynamic || rigidBody.motion == PhysicsMotion::Kinematic);
    }

    // `runtime` is deliberately not listed: it is this run's live state, never saved or edited.
    inline constexpr FieldDesc kRigidBodyFields[] = {
        { .mName = "motion", .mKind = FieldKind::Int, .mOffset = offsetof(RigidBody, motion),
         .mWidget = FieldWidget::Enum, .mEnumNames = kPhysicsMotionNames },
        { .mName = "shape", .mKind = FieldKind::Int, .mOffset = offsetof(RigidBody, shape),
         .mWidget = FieldWidget::Enum, .mEnumNames = kPhysicsShapeNames },
        { .mName = "halfExtent", .mKind = FieldKind::Vec3, .mOffset = offsetof(RigidBody, halfExtent) },
        { .mName = "radius", .mKind = FieldKind::Float, .mOffset = offsetof(RigidBody, radius),
         .mClamped = true, .mMin = 0.01f, .mMax = FLT_MAX }, // 0 would mean a degenerate, collision-less sphere
        { .mName = "halfHeight", .mKind = FieldKind::Float, .mOffset = offsetof(RigidBody, halfHeight),
         .mClamped = true, .mMin = 0.01f, .mMax = FLT_MAX }, // a body clamps it up to the radius when it is built
        { .mName = "axis", .mKind = FieldKind::Int, .mOffset = offsetof(RigidBody, axis),
         .mWidget = FieldWidget::Enum, .mEnumNames = kPhysicsAxisNames },
        { .mName = "offset", .mKind = FieldKind::Vec3, .mOffset = offsetof(RigidBody, offset) },
        { .mName = "rotationOffset", .mKind = FieldKind::Quat, .mOffset = offsetof(RigidBody, rotationOffset) },
        { .mName = "interpolate", .mKind = FieldKind::Bool, .mOffset = offsetof(RigidBody, interpolate) },
        { .mName = "isTrigger", .mKind = FieldKind::Bool, .mOffset = offsetof(RigidBody, isTrigger) },
        { .mName = "detectStay", .mKind = FieldKind::Bool, .mOffset = offsetof(RigidBody, detectStay) },
        { .mName = "mass", .mKind = FieldKind::Float, .mOffset = offsetof(RigidBody, mass),
         .mClamped = true, .mMin = 0.001f, .mMax = FLT_MAX }, // Jolt cannot build a dynamic body of no mass
        { .mName = "gravityFactor", .mKind = FieldKind::Float, .mOffset = offsetof(RigidBody, gravityFactor) },
        { .mName = "friction", .mKind = FieldKind::Float, .mOffset = offsetof(RigidBody, friction),
         .mClamped = true, .mMin = 0.0f, .mMax = FLT_MAX },
        { .mName = "restitution", .mKind = FieldKind::Float, .mOffset = offsetof(RigidBody, restitution),
         .mClamped = true, .mMin = 0.0f, .mMax = 1.0f },
        { .mName = "allowSleeping", .mKind = FieldKind::Bool, .mOffset = offsetof(RigidBody, allowSleeping) },
        { .mName = "layer", .mKind = FieldKind::Int, .mOffset = offsetof(RigidBody, layer) },
    };

    inline constexpr TransientField kRigidBodyTransient[] = {
        { offsetof(RigidBody, runtime), sizeof(RigidBody::runtime) },
    };
}

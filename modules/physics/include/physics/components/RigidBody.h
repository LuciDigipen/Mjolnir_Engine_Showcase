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
    struct RigidBody
    {
        PhysicsMotion motion = PhysicsMotion::Dynamic;
        PhysicsShape shape = PhysicsShape::Box;
        glm::vec3 halfExtent{ 0.5f, 0.5f, 0.5f };
        float radius = 0.5f;
        bool interpolate = false;

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
            // A Kinematic body's first Transform write teleports it, later ones move it to the target.
            bool hasMoved = false;
            // Moved to a target this step, so its velocity is cleared once the step has reached it.
            bool movedToTarget = false;
        };

        Runtime runtime;
    };

    MIR_ASSERT_COMPONENT(RigidBody);

    // A body the simulation moves, as opposed to one it only collides against.
    inline bool HasDynamicBody(const RigidBody &rigidBody)
    {
        return rigidBody.runtime.body.IsValid() && rigidBody.motion == PhysicsMotion::Dynamic;
    }

    // `runtime` is deliberately not listed: it is this run's live state, never saved or edited.
    // A body the step moves, dynamic or kinematic, whose pose is written back to the Transform.
    inline bool HasMovingBody(const RigidBody &rigidBody)
    {
        return rigidBody.runtime.body.IsValid()
            && (rigidBody.motion == PhysicsMotion::Dynamic || rigidBody.motion == PhysicsMotion::Kinematic);
    }

    inline constexpr FieldDesc kRigidBodyFields[] = {
        { .mName = "motion", .mKind = FieldKind::Int, .mOffset = offsetof(RigidBody, motion),
         .mWidget = FieldWidget::Enum, .mEnumNames = kPhysicsMotionNames },
        { .mName = "shape", .mKind = FieldKind::Int, .mOffset = offsetof(RigidBody, shape),
         .mWidget = FieldWidget::Enum, .mEnumNames = kPhysicsShapeNames },
        { .mName = "halfExtent", .mKind = FieldKind::Vec3, .mOffset = offsetof(RigidBody, halfExtent) },
        { .mName = "radius", .mKind = FieldKind::Float, .mOffset = offsetof(RigidBody, radius),
         .mClamped = true, .mMin = 0.01f, .mMax = FLT_MAX }, // 0 would mean a degenerate, collision-less sphere
        { .mName = "interpolate", .mKind = FieldKind::Bool, .mOffset = offsetof(RigidBody, interpolate) },
    };
}

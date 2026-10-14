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
        BodyHandle body;

        uint32_t transformVersion = 0;

        PhysicsMotion motion = PhysicsMotion::Dynamic;
        PhysicsShape shape = PhysicsShape::Box;

        glm::vec3 halfExtent{ 0.5f, 0.5f, 0.5f };
        float radius = 0.5f;


        bool interpolate = false;


        WorldVec3 previousPosition{ 0 };
        WorldVec3 currentPosition{ 0 };
        glm::quat previousRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::quat currentRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    };

    MIR_ASSERT_COMPONENT(RigidBody);

    inline constexpr FieldDesc kRigidBodyFields[] = {
        {.mName = "motion", .mKind = FieldKind::Int, .mOffset = offsetof(RigidBody, motion),
         .mWidget = FieldWidget::Enum, .mEnumNames = kPhysicsMotionNames},
        {.mName = "shape", .mKind = FieldKind::Int, .mOffset = offsetof(RigidBody, shape),
         .mWidget = FieldWidget::Enum, .mEnumNames = kPhysicsShapeNames},
        {.mName = "halfExtent", .mKind = FieldKind::Vec3, .mOffset = offsetof(RigidBody, halfExtent)},
        {.mName = "radius", .mKind = FieldKind::Float, .mOffset = offsetof(RigidBody, radius),
         .mClamped = true, .mMin = 0.01f, .mMax = FLT_MAX}, // 0 would mean a degenerate, collision-less sphere
        {.mName = "interpolate", .mKind = FieldKind::Bool, .mOffset = offsetof(RigidBody, interpolate)},
    };
}

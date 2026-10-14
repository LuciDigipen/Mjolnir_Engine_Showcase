/**
 * @file CameraFollow.h
 * @author Quy Dong Ta
 * @brief Makes an entity (usually the Camera) trail a target entity's pose.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/ComponentAsserts.h"
#include "core/ecs/ComponentFields.h"
#include "core/ecs/Entity.h"

#include <glm/vec3.hpp>

#include <cfloat>
#include <cstddef>

namespace mir
{
    struct CameraFollow
    {
        Entity mTarget = kNullEntity;


        glm::vec3 mOffset{ 0.0f, 3.0f, 8.0f };


        bool mOffsetInTargetSpace = false;


        float mStiffness = 6.0f;


        float mSnapDistance = 50.0f;

        bool mLookAtTarget = true;


        bool mPlaced = false;
    };

    MIR_ASSERT_COMPONENT(CameraFollow);

    inline constexpr FieldDesc kCameraFollowFields[] = {
        {.mName = "target", .mKind = FieldKind::EntityRef, .mOffset = offsetof(CameraFollow, mTarget)},
        {.mName = "offset", .mKind = FieldKind::Vec3, .mOffset = offsetof(CameraFollow, mOffset)},
        {.mName = "offsetInTargetSpace", .mKind = FieldKind::Bool,
         .mOffset = offsetof(CameraFollow, mOffsetInTargetSpace)},
        {.mName = "stiffness", .mKind = FieldKind::Float, .mOffset = offsetof(CameraFollow, mStiffness),
         .mClamped = true, .mMin = 0.0f, .mMax = FLT_MAX},
        {.mName = "snapDistance", .mKind = FieldKind::Float, .mOffset = offsetof(CameraFollow, mSnapDistance),
         .mClamped = true, .mMin = 0.0f, .mMax = FLT_MAX},
        {.mName = "lookAtTarget", .mKind = FieldKind::Bool, .mOffset = offsetof(CameraFollow, mLookAtTarget)},
    };
}

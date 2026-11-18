/**
 * @file CameraController.h
 * @author Quy Dong Ta
 * @brief Drives an entity (usually the Camera) by following a target or by free-fly input.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <core/ecs/ComponentAsserts.h>
#include <core/ecs/ComponentFields.h>
#include <core/ecs/Entity.h>

#include <glm/vec3.hpp>

#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mir
{
    enum class CameraMode : int32_t
    {
        Follow,
        FreeFly,
    };

    inline constexpr std::string_view kCameraModeNames[] = { "Follow", "FreeFly" };

    inline constexpr float kFreeFlyMaxPitchDegrees = 89.0f;

    struct CameraController
    {
        CameraMode mMode = CameraMode::Follow;


        Entity mTarget = kNullEntity;
        glm::vec3 mOffset{ 0.0f, 3.0f, 8.0f };
        bool mOffsetInTargetSpace = false;
        bool mOffsetHeadingOnly = false; // yaw only, together with mOffsetInTargetSpace
        float mStiffness = 6.0f;
        float mSnapDistance = 50.0f; //0 = never snaps
        bool mLookAtTarget = true;


        float mMoveSpeed = 5.0f;
        float mFastMultiplier = 4.0f;
        float mLookSensitivity = 0.15f; //degrees per pixel
        float mStickLookRate = 120.0f;  //degrees per second

        bool mPlaced = false;
        bool mReturning = false; 
        CameraMode mLastMode = CameraMode::Follow;
        float mYawDegrees = -90.0f;
        float mPitchDegrees = 0.0f;
    };

    MIR_ASSERT_COMPONENT(CameraController);

    inline constexpr FieldDesc kCameraControllerFields[] = {
        {.mName = "mode", .mKind = FieldKind::Int, .mOffset = offsetof(CameraController, mMode),
         .mWidget = FieldWidget::Enum, .mEnumNames = kCameraModeNames},
        {.mName = "target", .mKind = FieldKind::EntityRef, .mOffset = offsetof(CameraController, mTarget)},
        {.mName = "offset", .mKind = FieldKind::Vec3, .mOffset = offsetof(CameraController, mOffset)},
        {.mName = "offsetInTargetSpace", .mKind = FieldKind::Bool,
         .mOffset = offsetof(CameraController, mOffsetInTargetSpace)},
        {.mName = "offsetHeadingOnly", .mKind = FieldKind::Bool,
         .mOffset = offsetof(CameraController, mOffsetHeadingOnly)},
        {.mName = "stiffness", .mKind = FieldKind::Float, .mOffset = offsetof(CameraController, mStiffness),
         .mClamped = true, .mMin = 0.0f, .mMax = FLT_MAX},
        {.mName = "snapDistance", .mKind = FieldKind::Float, .mOffset = offsetof(CameraController, mSnapDistance),
         .mClamped = true, .mMin = 0.0f, .mMax = FLT_MAX},
        {.mName = "lookAtTarget", .mKind = FieldKind::Bool, .mOffset = offsetof(CameraController, mLookAtTarget)},
        {.mName = "moveSpeed", .mKind = FieldKind::Float, .mOffset = offsetof(CameraController, mMoveSpeed),
         .mClamped = true, .mMin = 0.0f, .mMax = FLT_MAX},
        {.mName = "fastMultiplier", .mKind = FieldKind::Float, .mOffset = offsetof(CameraController, mFastMultiplier),
         .mClamped = true, .mMin = 1.0f, .mMax = FLT_MAX},
        {.mName = "lookSensitivity", .mKind = FieldKind::Float,
         .mOffset = offsetof(CameraController, mLookSensitivity), .mClamped = true, .mMin = 0.0f, .mMax = FLT_MAX},
        {.mName = "stickLookRate", .mKind = FieldKind::Float, .mOffset = offsetof(CameraController, mStickLookRate),
         .mClamped = true, .mMin = 0.0f, .mMax = FLT_MAX},
        // free-fly look state, for scripts; switching into FreeFly re-seeds both from the Transform
        {.mName = "yawDegrees", .mKind = FieldKind::Float, .mOffset = offsetof(CameraController, mYawDegrees),
         .mHidden = true},
        {.mName = "pitchDegrees", .mKind = FieldKind::Float, .mOffset = offsetof(CameraController, mPitchDegrees),
         .mClamped = true, .mMin = -kFreeFlyMaxPitchDegrees, .mMax = kFreeFlyMaxPitchDegrees, .mHidden = true},
    };
}

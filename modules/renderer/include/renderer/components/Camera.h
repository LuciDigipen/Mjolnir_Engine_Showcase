/**
 * @file Camera.h
 * @author Rahul Nair
 * @brief Marks an entity as the viewpoint RenderSystem projects from.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/ComponentAsserts.h"
#include "core/ecs/ComponentFields.h"

#include <cmath>
#include <cstddef>

namespace mir
{

    struct Camera
    {
        float mFovYDegrees = 60.0f;
        float mNearZ = 0.1f;
        // 0 means no far plane. Depth is reverse-Z, so a large or missing far
        // plane costs no precision; it only decides what gets clipped.
        float mFarZ = 100.0f;
        // Play renders through the one active Camera; ActiveCameraSystem keeps it to one.
        bool mActive = false;

        // Read by HDR, Depth of Field and Motion Blur while those effects are enabled.
        float mExposure = 1.0f;
        float mFocusDistance = 10.0f;
        float mAperture = 0.1f;
        float mShutterStrength = 0.5f;

        // Anything not beyond the near plane counts as "none", so a bad value
        // cannot make a degenerate projection.
        bool HasFarPlane() const { return mFarZ > mNearZ && std::isfinite(mFarZ); }
    };

    MIR_ASSERT_COMPONENT(Camera);

    inline constexpr FieldDesc kCameraFields[] = {
        {"active", FieldKind::Bool, offsetof(Camera, mActive)},
        {"fovYDegrees", FieldKind::Float, offsetof(Camera, mFovYDegrees)},
        {"nearZ", FieldKind::Float, offsetof(Camera, mNearZ)},
        {"farZ", FieldKind::Float, offsetof(Camera, mFarZ)},
        {.mName = "exposure", .mKind = FieldKind::Float, .mOffset = offsetof(Camera, mExposure),
         .mClamped = true, .mMin = 0.01f, .mMax = 16.0f},
        {.mName = "focusDistance", .mKind = FieldKind::Float, .mOffset = offsetof(Camera, mFocusDistance),
         .mClamped = true, .mMin = 0.0f, .mMax = 1000.0f},
        {.mName = "aperture", .mKind = FieldKind::Float, .mOffset = offsetof(Camera, mAperture),
         .mClamped = true, .mMin = 0.0f, .mMax = 2.0f},
        {.mName = "shutterStrength", .mKind = FieldKind::Float, .mOffset = offsetof(Camera, mShutterStrength),
         .mClamped = true, .mMin = 0.0f, .mMax = 2.0f},
    };
}

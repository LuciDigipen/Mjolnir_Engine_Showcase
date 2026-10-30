/**
 * @file ColliderLines.h
 * @author Rahul Nair
 * @brief A collider's shape in world space, and the debug line segments that
 *        outline it - boxes, spheres, capsules and height fields.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics/PhysicsTypes.h>

#include <core/debug/DebugLines.h>
#include <core/math/WorldCoordinates.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace mir
{
    struct ColliderView
    {
        PhysicsShape mShape = PhysicsShape::Box;
        // a height field reads mHeightGrid, not the shape sizes
        bool mHeightField = false;

        // The shape's own center and axes in world space: the body's pose with the collider offset applied.
        // A capsule stands along its local Y.
        WorldPosition mPosition;
        glm::quat mRotation = kIdentityRotation;

        glm::vec3 mHalfExtent{ 0.0f };
        float mRadius = 0.0f;
        // the capsule's cylinder between its caps, half of it
        float mCylinderHalfHeight = 0.0f;

        PhysicsMotion mMotion = PhysicsMotion::Static;
        bool mTrigger = false;
        bool mSleeping = false;

        // mGridSize * mGridSize points in the shape's frame, x fastest; a hole has a NaN y. Not owned: whoever
        // hands out the view says how long they live.
        std::span<const glm::vec3> mHeightGrid;
        uint32_t mGridSize = 0;
    };

    // Trigger over sleeping over the motion type, so the state that changes what a collider does wins.
    uint32_t ColliderColor(const ColliderView &view);

    // Segments circling a sphere or capsule; each circle has this many.
    inline constexpr uint32_t kColliderCircleSegments = 24;

    // Appends vertex pairs, one segment each, in the view's color. A height field skips any cell edge
    // that touches a hole.
    void AppendColliderLines(const ColliderView &view, std::vector<DebugLineVertex> &out);
}

/**
 * @file UIWorldProjection.h
 * @author Quy Dong Ta
 * @brief Where a point in the world lands in the game UI, through the view the frame renders, and where a marker
 *        for one off screen goes on the edge.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UITypes.h"

#include <core/ecs/PrimaryView.h>
#include <core/math/WorldCoordinates.h>

#include <optional>

namespace mir
{
    struct UIWorldPoint
    {
        // in UI units. Behind the camera it is mirrored back, so it still lies the way the target does from the
        // centre
        UIPoint mPoint;
        // meters along the view axis, negative behind the camera
        float mDepth = 0.0f;
        bool mInFront = false;
        // in front and inside the bounds
        bool mOnScreen = false;
    };

    struct UIEdgePoint
    {
        UIPoint mPoint;
        // radians from the bounds' centre toward the target, 0 to the right and clockwise, as UITransform turns
        float mAngle = 0.0f;
    };

    // none without a valid view, or for a point too far from the camera for float
    std::optional<UIWorldPoint> ProjectToUI(const PrimaryView &view, WorldPosition point, const UIRect &bounds);

    // a point in front and inside @p edge stays where it is, any other goes to @p edge's border, on the line from
    // the bounds' centre through it
    UIEdgePoint EdgePoint(const UIWorldPoint &point, const UIRect &bounds, const UIRect &edge);
}

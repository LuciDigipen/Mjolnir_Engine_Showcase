/**
 * @file UIWorldProjection.cpp
 * @author Quy Dong Ta
 * @brief Where a point in the world lands in the game UI, through the view the frame renders, and where a marker
 *        for one off screen goes on the edge.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIWorldProjection.h>

#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace mir
{
    namespace
    {
        // a point on the camera plane still gets a direction
        constexpr float kNearestDepth = 1e-6f;
        // far enough past any edge, and short of overflowing a float
        constexpr float kFarthestNdc = 1e4f;
    }

    std::optional<UIWorldPoint> ProjectToUI(const PrimaryView &view, WorldPosition point, const UIRect &bounds)
    {
        if (!view.mValid)
            return std::nullopt;
        const std::optional<glm::vec3> relative = TryCameraRelativeFloat(point, view.mCameraPosition);
        if (!relative)
            return std::nullopt;
        const glm::vec4 clip = view.mViewProjection * glm::vec4(*relative, 1.0f);
        if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.w))
            return std::nullopt;

        // dividing by |w| keeps a point behind the camera on its own side of the centre
        const float w = std::max(std::abs(clip.w), kNearestDepth);
        float x = clip.x / w;
        float y = clip.y / w;
        const float largest = std::max(std::abs(x), std::abs(y));
        if (!std::isfinite(largest))
            return std::nullopt;
        if (largest > kFarthestNdc)
        {
            x *= kFarthestNdc / largest;
            y *= kFarthestNdc / largest;
        }

        UIWorldPoint out;
        out.mPoint = UIPoint{ bounds.x + (x * 0.5f + 0.5f) * bounds.width, bounds.y + (y * 0.5f + 0.5f) * bounds.height };
        out.mDepth = clip.w;
        out.mInFront = clip.w > 0.0f;
        out.mOnScreen = out.mInFront && Contains(bounds, out.mPoint.x, out.mPoint.y);
        return out;
    }

    UIEdgePoint EdgePoint(const UIWorldPoint &point, const UIRect &bounds, const UIRect &edge)
    {
        const UIPoint center{ bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f };
        float dx = point.mPoint.x - center.x;
        float dy = point.mPoint.y - center.y;
        // dead ahead or dead behind has no side, so it points down
        if (dx == 0.0f && dy == 0.0f)
            dy = 1.0f;
        const float angle = std::atan2(dy, dx);

        if (point.mInFront && Contains(edge, point.mPoint.x, point.mPoint.y))
            return UIEdgePoint{ point.mPoint, angle };

        float t = std::numeric_limits<float>::max();
        if (dx > 0.0f)
            t = std::min(t, (edge.Right() - center.x) / dx);
        else if (dx < 0.0f)
            t = std::min(t, (edge.x - center.x) / dx);
        if (dy > 0.0f)
            t = std::min(t, (edge.Bottom() - center.y) / dy);
        else if (dy < 0.0f)
            t = std::min(t, (edge.y - center.y) / dy);
        t = std::max(t, 0.0f);

        const UIPoint onEdge{ std::clamp(center.x + dx * t, edge.x, std::max(edge.x, edge.Right())),
            std::clamp(center.y + dy * t, edge.y, std::max(edge.y, edge.Bottom())) };
        return UIEdgePoint{ onEdge, angle };
    }
}

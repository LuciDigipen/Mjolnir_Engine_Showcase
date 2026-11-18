/**
 * @file FlightCurve.cpp
 * @author Jackey Cen
 * @brief Evaluates a piecewise-linear curve.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightCurve.h>

#include <cstddef>

namespace mir
{
    float EvaluateCurve(const std::vector<CurvePoint> &curve, float x)
    {
        if (curve.empty())
            return 0.0f;
        if (x <= curve.front().x)
            return curve.front().y;
        if (x >= curve.back().x)
            return curve.back().y;

        for (std::size_t i = 1; i < curve.size(); ++i)
        {
            if (x > curve[i].x)
                continue;

            const CurvePoint &lower = curve[i - 1];
            const CurvePoint &upper = curve[i];
            const float t = (x - lower.x) / (upper.x - lower.x);
            return lower.y + t * (upper.y - lower.y);
        }
        return curve.back().y;
    }
}

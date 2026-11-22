/**
 * @file FlightCurve.h
 * @author Jackey Cen
 * @brief Piecewise-linear curves with clamped ends.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <vector>

namespace mir
{
    struct CurvePoint
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    // Linear between the points, clamped to the first and last y outside their x range.
    float EvaluateCurve(const std::vector<CurvePoint> &curve, float x);
}

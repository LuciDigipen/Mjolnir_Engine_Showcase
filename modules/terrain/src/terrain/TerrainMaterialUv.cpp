/**
 * @file TerrainMaterialUv.cpp
 * @author Jackey Cen
 * @brief Stable terrain-local material UVs and the tile-period rule.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain/TerrainMaterialUv.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace mir
{
    namespace
    {
        // A tile period arrives as a float32 material parameter, then the
        // quotient is a double division. The float conversion and the double
        // operation bound the quotient's error to a few ulps.
        // Must match the shader guard; a smaller period would be clamped there.
        constexpr double kMinTilePeriodMeters = 0.001;
        // P and the packed float period are exact inputs; the only remaining
        // error is a few double operations around their product.
        constexpr double kPeriodRemainderTolerance =
            8.0 * std::numeric_limits<double>::epsilon() * kTerrainMaterialUvPeriodMeters;

        double ReduceOrigin(double meters)
        {
            double reduced = std::fmod(meters, kTerrainMaterialUvPeriodMeters);
            if (reduced < 0.0)
                reduced += kTerrainMaterialUvPeriodMeters;
            if (reduced >= kTerrainMaterialUvPeriodMeters)
                reduced -= kTerrainMaterialUvPeriodMeters;
            return reduced;
        }
    }

    bool TerrainMaterialTilePeriodSupported(double tilePeriodMeters, double *remainderMeters)
    {
        if (!std::isfinite(tilePeriodMeters))
            return false;

        const float packed = static_cast<float>(tilePeriodMeters);
        const double period = static_cast<double>(packed);
        if (!std::isfinite(period) || period < kMinTilePeriodMeters)
            return false;

        const double ratio = kTerrainMaterialUvPeriodMeters / period;
        if (!std::isfinite(ratio) || ratio < 1.0)
            return false;

        const double repeats = std::round(ratio);
        const double remainder = std::abs(kTerrainMaterialUvPeriodMeters - repeats * period);
        if (remainderMeters != nullptr)
            *remainderMeters = remainder;

        if (repeats < 1.0 || !std::isfinite(remainder) || remainder > kPeriodRemainderTolerance)
            return false;
        return std::abs(ratio - repeats) <=
            8.0 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(ratio));
    }

    glm::dvec2 TerrainMaterialUvOrigin(glm::dvec3 terrainLocalOriginMeters)
    {
        return {ReduceOrigin(terrainLocalOriginMeters.x), ReduceOrigin(terrainLocalOriginMeters.z)};
    }

    glm::vec2 TerrainMaterialUv(glm::dvec3 terrainLocalOriginMeters, glm::vec3 localVertexMeters)
    {
        const glm::dvec2 origin = TerrainMaterialUvOrigin(terrainLocalOriginMeters);
        return {static_cast<float>(origin.x + static_cast<double>(localVertexMeters.x)),
                static_cast<float>(origin.y + static_cast<double>(localVertexMeters.z))};
    }
}

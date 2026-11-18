/**
 * @file FlightEnvironment.h
 * @author Jackey Cen
 * @brief The world's air: sea level, density model and global wind.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <glm/vec3.hpp>

namespace mir
{
    enum class DensityModel
    {
        Constant,
        Isa,
    };

    struct FlightEnvironment
    {
        double seaLevelY = 0.0;
        DensityModel densityModel = DensityModel::Isa;
        float constantDensity = 1.225f;
        // The velocity of the air itself, world frame.
        glm::vec3 wind{ 0.0f };
    };

    struct AtmosphereSample
    {
        double altitude = 0.0;
        float density = 0.0f;
        float speedOfSound = 0.0f;
    };

    AtmosphereSample SampleAtmosphere(const FlightEnvironment &environment, double worldY);
}

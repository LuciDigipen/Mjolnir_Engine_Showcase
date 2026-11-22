/**
 * @file FlightEnvironment.cpp
 * @author Jackey Cen
 * @brief The world's air: sea level, density model and global wind.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightEnvironment.h>

#include <algorithm>
#include <cmath>

namespace
{
    constexpr double kTroposphereTop = 11000.0;
    constexpr double kSeaLevelTemperature = 288.15;
    constexpr double kLapseRate = 0.0065;
    constexpr double kTropopauseTemperature = 216.65;
    constexpr double kScaleHeight = 6341.6;
    constexpr double kSeaLevelDensity = 1.225;
    constexpr double kDensityExponent = 4.2559;
    constexpr double kMinAltitude = -500.0;
    constexpr double kMaxAltitude = 20000.0;
    constexpr double kGamma = 1.4;
    constexpr double kGasConstant = 287.05;
    constexpr float kConstantSpeedOfSound = 340.29f;
}

namespace mir
{
    AtmosphereSample SampleAtmosphere(const FlightEnvironment &environment, double worldY)
    {
        const double altitude = worldY - environment.seaLevelY;
        const double formulaAltitude = std::clamp(altitude, kMinAltitude, kMaxAltitude);

        AtmosphereSample sample{ .altitude = altitude };
        if (environment.densityModel == DensityModel::Constant)
        {
            sample.density = environment.constantDensity;
            sample.speedOfSound = kConstantSpeedOfSound;
            return sample;
        }

        double temperature = kSeaLevelTemperature - kLapseRate * formulaAltitude;
        double density = kSeaLevelDensity * std::pow(temperature / kSeaLevelTemperature, kDensityExponent);
        if (formulaAltitude > kTroposphereTop)
        {
            temperature = kTropopauseTemperature;
            density = kSeaLevelDensity *
                      std::pow(kTropopauseTemperature / kSeaLevelTemperature, kDensityExponent) *
                      std::exp(-(formulaAltitude - kTroposphereTop) / kScaleHeight);
        }

        sample.density = static_cast<float>(density);
        sample.speedOfSound = static_cast<float>(std::sqrt(kGamma * kGasConstant * temperature));
        return sample;
    }
}

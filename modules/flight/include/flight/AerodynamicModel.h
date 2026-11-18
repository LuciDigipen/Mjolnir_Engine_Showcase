/**
 * @file AerodynamicModel.h
 * @author Jackey Cen
 * @brief The pure aerodynamic interface and the per-tier profiles that drive it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <flight/AirData.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>

namespace mir
{
    enum class AerodynamicTier : std::int32_t;

    struct WindAxisForces
    {
        glm::vec3 lift{0.0f};
        glm::vec3 drag{0.0f};
        glm::vec3 side{0.0f};
    };

    // Wind-axis force magnitudes to body-frame vectors; all zero below kAirDataMinSpeed.
    WindAxisForces WindAxesToBody(const AirData &air, float lift, float drag, float side);

    struct ForceMomentSources
    {
        glm::vec3 lift{0.0f};
        glm::vec3 drag{0.0f};
        glm::vec3 side{0.0f};
        glm::vec3 controlMoment{0.0f};
        glm::vec3 dampingMoment{0.0f};
        glm::vec3 stabilityMoment{0.0f};
    };

    struct ForceMoment
    {
        // Body frame, about the centre of mass.
        glm::vec3 force{0.0f};
        glm::vec3 moment{0.0f};
        ForceMomentSources sources;
    };

    struct AeroInput
    {
        AirData air;
        glm::vec3 angularVelocityBody{0.0f};
        // Actual deflections in radians.
        float elevator = 0.0f;
        float aileron = 0.0f;
        float rudder = 0.0f;
        float flaps = 0.0f;
        // Surface travel in radians, for the normalised deflection.
        float elevatorMax = 0.0f;
        float aileronMax = 0.0f;
        float rudderMax = 0.0f;
        float area = 0.0f;
        float span = 0.0f;
        float chord = 0.0f;
    };

    struct L1Profile
    {
        float CL = 0.0f;
        float CD = 0.0f;
        float Ka = 0.0f;
        float Ke = 0.0f;
        float Kr = 0.0f;
        float Kp = 0.0f;
        float Kq = 0.0f;
        float Kn = 0.0f;
    };

    struct AerodynamicProfile
    {
        std::optional<L1Profile> l1;
    };

    bool HasAerodynamicProfile(const AerodynamicProfile &profile, AerodynamicTier tier);

    class IAerodynamicModel
    {
      public:
        virtual ~IAerodynamicModel() = default;
        virtual ForceMoment Evaluate(const AeroInput &input, const AerodynamicProfile &profile) const = 0;
    };

    // Stateless models; L2 and L3 yield zero until their issues land.
    const IAerodynamicModel &AerodynamicModelFor(AerodynamicTier tier);
}

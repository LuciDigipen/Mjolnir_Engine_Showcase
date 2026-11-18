/**
 * @file AerodynamicModel.cpp
 * @author Jackey Cen
 * @brief The L1 aerodynamic model and the per-tier model lookup.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AerodynamicModel.h>

#include <flight/components/Aircraft.h>

#include <glm/geometric.hpp>

namespace mir
{
    namespace
    {
        float NormalisedDeflection(float actual, float max)
        {
            return max > 0.0f ? actual / max : 0.0f;
        }

        ForceMoment EvaluateL1(const AeroInput &input, const AerodynamicProfile &profile)
        {
            ForceMoment result;
            if (!profile.l1)
                return result;

            const L1Profile &l1 = *profile.l1;
            const float dynamicPressure = input.air.dynamicPressure;
            const WindAxisForces axes =
                WindAxesToBody(input.air, dynamicPressure * input.area * l1.CL,
                    dynamicPressure * input.area * l1.CD, 0.0f);

            const float aileron = NormalisedDeflection(input.aileron, input.aileronMax);
            const float elevator = NormalisedDeflection(input.elevator, input.elevatorMax);
            const float rudder = NormalisedDeflection(input.rudder, input.rudderMax);
            const glm::vec3 controlMoment{dynamicPressure * l1.Ka * aileron,
                dynamicPressure * l1.Ke * elevator, dynamicPressure * l1.Kr * rudder};

            const glm::vec3 angularVelocity = input.angularVelocityBody;
            const glm::vec3 dampingMoment{-l1.Kp * angularVelocity.x, -l1.Kq * angularVelocity.y,
                -l1.Kn * angularVelocity.z};

            result.sources.lift = axes.lift;
            result.sources.drag = axes.drag;
            result.sources.side = axes.side;
            result.sources.controlMoment = controlMoment;
            result.sources.dampingMoment = dampingMoment;
            result.force = axes.lift + axes.drag + axes.side;
            result.moment = controlMoment + dampingMoment;
            return result;
        }

        class ZeroAerodynamicModel final : public IAerodynamicModel
        {
          public:
            ForceMoment Evaluate(const AeroInput &, const AerodynamicProfile &) const override { return {}; }
        };

        class L1AerodynamicModel final : public IAerodynamicModel
        {
          public:
            ForceMoment Evaluate(const AeroInput &input, const AerodynamicProfile &profile) const override
            {
                return EvaluateL1(input, profile);
            }
        };
    }

    WindAxisForces WindAxesToBody(const AirData &air, float lift, float drag, float side)
    {
        WindAxisForces result;
        if (air.trueAirspeed < kAirDataMinSpeed)
            return result;

        const glm::vec3 flow = air.relativeVelocityBody / air.trueAirspeed;
        const glm::vec3 liftAxis = glm::cross(glm::vec3{0.0f, 1.0f, 0.0f}, flow);
        const float liftAxisLength = glm::length(liftAxis);
        if (liftAxisLength > 0.0f)
            result.lift = liftAxis * (lift / liftAxisLength);
        result.drag = -flow * drag;
        result.side = glm::vec3{0.0f, side, 0.0f};
        return result;
    }

    bool HasAerodynamicProfile(const AerodynamicProfile &profile, AerodynamicTier tier)
    {
        switch (tier)
        {
        case AerodynamicTier::L1:
            return profile.l1.has_value();
        case AerodynamicTier::L2:
        case AerodynamicTier::L3:
            return false;
        }
        return false;
    }

    const IAerodynamicModel &AerodynamicModelFor(AerodynamicTier tier)
    {
        static const ZeroAerodynamicModel kZero;
        static const L1AerodynamicModel kL1;

        switch (tier)
        {
        case AerodynamicTier::L1:
            return kL1;
        case AerodynamicTier::L2:
        case AerodynamicTier::L3:
            return kZero;
        }
        return kZero;
    }
}

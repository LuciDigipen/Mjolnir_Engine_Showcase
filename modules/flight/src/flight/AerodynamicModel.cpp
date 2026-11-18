/**
 * @file AerodynamicModel.cpp
 * @author Jackey Cen
 * @brief The L1 and L2 aerodynamic models and the per-tier model lookup.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AerodynamicModel.h>

#include <flight/components/Aircraft.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace mir
{
    namespace
    {
        float NormalisedDeflection(float actual, float max)
        {
            return max > 0.0f ? actual / max : 0.0f;
        }

        float SmoothStep(float edge0, float edge1, float x)
        {
            if (edge1 <= edge0)
                return x >= edge1 ? 1.0f : 0.0f;
            const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        L2FlapPoint InterpolateFlaps(const std::vector<L2FlapPoint> &table, float flap)
        {
            if (table.empty())
                return {};
            if (flap <= table.front().flap)
                return table.front();
            if (flap >= table.back().flap)
                return table.back();

            for (std::size_t i = 1; i < table.size(); ++i)
            {
                if (flap > table[i].flap)
                    continue;

                const L2FlapPoint &a = table[i - 1];
                const L2FlapPoint &b = table[i];
                const float t = (flap - a.flap) / (b.flap - a.flap);
                return L2FlapPoint{ flap, std::lerp(a.dCL, b.dCL, t), std::lerp(a.dCD, b.dCD, t),
                    std::lerp(a.dCm, b.dCm, t), std::lerp(a.stallShift, b.stallShift, t) };
            }
            return table.back();
        }

        L2PostStallPoint InterpolatePostStall(const std::vector<L2PostStallPoint> &table, float alpha)
        {
            if (table.empty())
                return {};
            if (alpha <= table.front().alpha)
                return table.front();
            if (alpha >= table.back().alpha)
                return table.back();

            for (std::size_t i = 1; i < table.size(); ++i)
            {
                if (alpha > table[i].alpha)
                    continue;

                const L2PostStallPoint &a = table[i - 1];
                const L2PostStallPoint &b = table[i];
                const float t = (alpha - a.alpha) / (b.alpha - a.alpha);
                return L2PostStallPoint{ alpha, std::lerp(a.CL, b.CL, t), std::lerp(a.CD, b.CD, t),
                    std::lerp(a.Cm, b.Cm, t) };
            }
            return table.back();
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

        ForceMoment EvaluateL2(const AeroInput &input, const AerodynamicProfile &profile)
        {
            ForceMoment result;
            if (!profile.l2)
                return result;

            const L2Profile &l2 = *profile.l2;
            const float speed = input.air.trueAirspeed;
            if (speed < kAirDataMinSpeed)
                return result;

            const float alpha = input.air.angleOfAttack;
            const float beta = input.air.sideslip;
            const L2FlapPoint flap = InterpolateFlaps(l2.flaps, input.flaps);
            const float stallFraction = L2StallFraction(l2, alpha, input.flaps);
            const float controlScale = std::lerp(1.0f, l2.stall.controlScale, stallFraction);
            const float stabilityScale = std::lerp(1.0f, l2.stall.stabilityScale, stallFraction);
            const L2PostStallPoint postStall = InterpolatePostStall(l2.stall.postStall, alpha);

            const float liftLinear = l2.CL0 + l2.CLalpha * alpha + flap.dCL;
            const float dragLinear = l2.CD0 + l2.k * liftLinear * liftLinear + flap.dCD;
            const float pitchLinear = l2.Cm0 + l2.Cmalpha * stabilityScale * alpha + flap.dCm;
            const float lift = std::lerp(liftLinear, postStall.CL, stallFraction);
            const float drag = std::lerp(dragLinear, postStall.CD, stallFraction);
            const float pitch = std::lerp(pitchLinear, postStall.Cm, stallFraction);

            const float dynamicPressure = 0.5f * input.air.density * speed * speed;
            const float area = input.area;
            const float span = input.span;
            const float chord = input.chord;

            const WindAxisForces axes = WindAxesToBody(input.air, dynamicPressure * area * lift,
                dynamicPressure * area * drag,
                dynamicPressure * area * (l2.CYbeta * beta + l2.CYdr * input.rudder));

            const float effectiveSpeed = std::max(speed, l2.minAirspeed);
            const float rollRate = input.angularVelocityBody.x * span / (2.0f * effectiveSpeed);
            const float pitchRate = input.angularVelocityBody.y * chord / (2.0f * effectiveSpeed);
            const float yawRate = input.angularVelocityBody.z * span / (2.0f * effectiveSpeed);

            const float rollScale = dynamicPressure * area * span;
            const float yawScale = rollScale;
            const float pitchScale = dynamicPressure * area * chord;
            const float rollStability = l2.Clbeta * stabilityScale * beta;
            const float yawStability = l2.Cnbeta * stabilityScale * beta;

            result.sources.controlMoment = glm::vec3{
                rollScale * controlScale * l2.Clda * input.aileron,
                pitchScale * controlScale * l2.Cmde * input.elevator,
                yawScale * controlScale * (l2.Cndr * input.rudder + l2.Cnda * input.aileron)};
            result.sources.dampingMoment = glm::vec3{rollScale * l2.Clp * rollRate,
                pitchScale * l2.Cmq * pitchRate, yawScale * l2.Cnr * yawRate};
            result.sources.stabilityMoment =
                glm::vec3{rollScale * rollStability, pitchScale * pitch, yawScale * yawStability};

            result.sources.lift = axes.lift;
            result.sources.drag = axes.drag;
            result.sources.side = axes.side;
            result.force = axes.lift + axes.drag + axes.side;
            result.moment =
                result.sources.controlMoment + result.sources.dampingMoment + result.sources.stabilityMoment;
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

        class L2AerodynamicModel final : public IAerodynamicModel
        {
          public:
            ForceMoment Evaluate(const AeroInput &input, const AerodynamicProfile &profile) const override
            {
                return EvaluateL2(input, profile);
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

    float L2StallFraction(const L2Profile &profile, float alpha, float flaps)
    {
        const float positive = L2CriticalAngleOfAttack(profile, flaps);
        const float negative = -profile.stall.negative;
        const float blend = profile.stall.blend;
        return std::max(SmoothStep(positive, positive + blend, alpha),
            SmoothStep(negative, negative + blend, -alpha));
    }

    float L2CriticalAngleOfAttack(const L2Profile &profile, float flaps)
    {
        return profile.stall.positive + InterpolateFlaps(profile.flaps, flaps).stallShift;
    }

    bool HasAerodynamicProfile(const AerodynamicProfile &profile, AerodynamicTier tier)
    {
        switch (tier)
        {
        case AerodynamicTier::L1:
            return profile.l1.has_value();
        case AerodynamicTier::L2:
            return profile.l2.has_value();
        case AerodynamicTier::L3:
            return false;
        }
        return false;
    }

    const IAerodynamicModel &AerodynamicModelFor(AerodynamicTier tier)
    {
        static const ZeroAerodynamicModel kZero;
        static const L1AerodynamicModel kL1;
        static const L2AerodynamicModel kL2;

        switch (tier)
        {
        case AerodynamicTier::L1:
            return kL1;
        case AerodynamicTier::L2:
            return kL2;
        case AerodynamicTier::L3:
            return kZero;
        }
        return kZero;
    }
}

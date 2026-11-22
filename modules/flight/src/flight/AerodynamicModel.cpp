/**
 * @file AerodynamicModel.cpp
 * @author Jackey Cen
 * @brief The L1 and L2 aerodynamic models and the per-tier model lookup.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AerodynamicModel.h>

#include <flight/AircraftConfig.h>
#include <flight/components/Aircraft.h>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

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

        ForceMoment EvaluateL2(const AeroInput &input, const L2Profile &l2)
        {
            ForceMoment result;
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

        constexpr float kL3ReferenceAirspeed = 50.0f;
        constexpr float kL3ReferenceDensity = 1.225f;
        constexpr float kL3DefaultAlphaMinDegrees = -10.0f;
        constexpr float kL3DefaultAlphaMaxDegrees = 20.0f;
        constexpr float kL3DefaultAlphaStepDegrees = 2.5f;
        constexpr float kL3DefaultPostStallLimitDegrees = 30.0f;

        struct AxisSample
        {
            std::size_t lower = 0;
            std::size_t upper = 0;
            float fraction = 0.0f;
            bool outOfTable = false;
        };

        AxisSample SampleAxis(const std::vector<float> &axis, float value)
        {
            AxisSample sample;
            if (axis.empty())
            {
                sample.outOfTable = true;
                return sample;
            }
            if (value <= axis.front())
            {
                sample.outOfTable = value < axis.front();
                return sample;
            }
            if (value >= axis.back())
            {
                sample.lower = axis.size() - 1;
                sample.upper = sample.lower;
                sample.outOfTable = value > axis.back();
                return sample;
            }

            std::size_t upper = 1;
            while (upper + 1 < axis.size() && axis[upper] <= value)
                ++upper;
            sample.lower = upper - 1;
            sample.upper = upper;
            const float span = axis[upper] - axis[upper - 1];
            sample.fraction = span > 0.0f ? (value - axis[upper - 1]) / span : 0.0f;
            return sample;
        }

        void ResizeTable(L3Table &table, std::size_t count)
        {
            table.CX.assign(count, 0.0f);
            table.CY.assign(count, 0.0f);
            table.CZ.assign(count, 0.0f);
            table.Cl.assign(count, 0.0f);
            table.Cm.assign(count, 0.0f);
            table.Cn.assign(count, 0.0f);
        }

        void WriteCoefficients(L3Table &table, std::size_t index, const L3Coefficients &coefficients)
        {
            table.CX[index] = coefficients.CX;
            table.CY[index] = coefficients.CY;
            table.CZ[index] = coefficients.CZ;
            table.Cl[index] = coefficients.Cl;
            table.Cm[index] = coefficients.Cm;
            table.Cn[index] = coefficients.Cn;
        }

        L3Coefficients ReadCoefficients(const L3Table &table, std::size_t index)
        {
            return L3Coefficients{ table.CX[index], table.CY[index], table.CZ[index], table.Cl[index],
                table.Cm[index], table.Cn[index] };
        }

        bool TableHasSize(const L3Table &table, std::size_t count)
        {
            return count > 0 && table.CX.size() == count && table.CY.size() == count && table.CZ.size() == count
                && table.Cl.size() == count && table.Cm.size() == count && table.Cn.size() == count;
        }

        void AddScaled(L3Coefficients &sum, const L3Coefficients &value, float weight)
        {
            sum.CX += value.CX * weight;
            sum.CY += value.CY * weight;
            sum.CZ += value.CZ * weight;
            sum.Cl += value.Cl * weight;
            sum.Cm += value.Cm * weight;
            sum.Cn += value.Cn * weight;
        }

        L3Coefficients InterpolateBase(const L3Profile &profile, const AxisSample &alpha, const AxisSample &beta,
            const AxisSample &mach, const AxisSample &flaps)
        {
            const std::size_t nBeta = profile.grid.beta.size();
            const std::size_t nMach = profile.grid.mach.size();
            const std::size_t nFlaps = profile.grid.flaps.size();
            const std::size_t cells = profile.grid.alpha.size() * nBeta * nMach * nFlaps;
            if (!TableHasSize(profile.base, cells))
                return {};

            const std::size_t alphaIndices[2] = { alpha.lower, alpha.upper };
            const std::size_t betaIndices[2] = { beta.lower, beta.upper };
            const std::size_t machIndices[2] = { mach.lower, mach.upper };
            const std::size_t flapIndices[2] = { flaps.lower, flaps.upper };
            const float alphaWeights[2] = { 1.0f - alpha.fraction, alpha.fraction };
            const float betaWeights[2] = { 1.0f - beta.fraction, beta.fraction };
            const float machWeights[2] = { 1.0f - mach.fraction, mach.fraction };
            const float flapWeights[2] = { 1.0f - flaps.fraction, flaps.fraction };

            L3Coefficients result;
            for (std::size_t ia = 0; ia < 2; ++ia)
            {
                for (std::size_t ib = 0; ib < 2; ++ib)
                {
                    for (std::size_t im = 0; im < 2; ++im)
                    {
                        for (std::size_t iFlap = 0; iFlap < 2; ++iFlap)
                        {
                            const std::size_t index =
                                ((alphaIndices[ia] * nBeta + betaIndices[ib]) * nMach + machIndices[im]) * nFlaps
                                + flapIndices[iFlap];
                            const float weight =
                                alphaWeights[ia] * betaWeights[ib] * machWeights[im] * flapWeights[iFlap];
                            if (weight != 0.0f)
                                AddScaled(result, ReadCoefficients(profile.base, index), weight);
                        }
                    }
                }
            }
            return result;
        }

        L3Coefficients InterpolateControl(const L3Profile &profile, const L3ControlTable &table,
            const AxisSample &alpha, float deflection)
        {
            const std::size_t nDeflection = table.deflection.size();
            const std::size_t cells = profile.grid.alpha.size() * nDeflection;
            if (!TableHasSize(table.increments, cells))
                return {};

            const AxisSample axis = SampleAxis(table.deflection, deflection);
            const std::size_t alphaIndices[2] = { alpha.lower, alpha.upper };
            const std::size_t deflectionIndices[2] = { axis.lower, axis.upper };
            const float alphaWeights[2] = { 1.0f - alpha.fraction, alpha.fraction };
            const float deflectionWeights[2] = { 1.0f - axis.fraction, axis.fraction };

            L3Coefficients result;
            for (std::size_t ia = 0; ia < 2; ++ia)
            {
                for (std::size_t id = 0; id < 2; ++id)
                {
                    const std::size_t index = alphaIndices[ia] * nDeflection + deflectionIndices[id];
                    const float weight = alphaWeights[ia] * deflectionWeights[id];
                    if (weight != 0.0f)
                        AddScaled(result, ReadCoefficients(table.increments, index), weight);
                }
            }
            return result;
        }

        float InterpolateRate(const std::vector<float> &values, const AxisSample &alpha)
        {
            if (values.empty())
                return 0.0f;
            const std::size_t lower = std::min(alpha.lower, values.size() - 1);
            const std::size_t upper = std::min(alpha.upper, values.size() - 1);
            return std::lerp(values[lower], values[upper], alpha.fraction);
        }

        AirData ReferenceAir(float alpha, float beta)
        {
            AirData air;
            const float cosAlpha = std::cos(alpha);
            const float cosBeta = std::cos(beta);
            air.relativeVelocityBody = kL3ReferenceAirspeed
                * glm::vec3{ cosAlpha * cosBeta, std::sin(beta), std::sin(alpha) * cosBeta };
            air.trueAirspeed = kL3ReferenceAirspeed;
            air.angleOfAttack = alpha;
            air.sideslip = beta;
            air.dynamicPressure = 0.5f * kL3ReferenceDensity * kL3ReferenceAirspeed * kL3ReferenceAirspeed;
            air.density = kL3ReferenceDensity;
            return air;
        }

        L3Coefficients L2Coefficients(const L2Profile &l2, const AircraftConfig &config, float alpha, float beta,
            float flaps, const glm::vec3 &deflections)
        {
            AeroInput input;
            input.air = ReferenceAir(alpha, beta);
            input.flaps = flaps;
            input.elevator = deflections.x;
            input.aileron = deflections.y;
            input.rudder = deflections.z;
            input.area = config.reference.area;
            input.span = config.reference.span;
            input.chord = config.reference.chord;

            const ForceMoment result = EvaluateL2(input, l2);
            const float scale = input.air.dynamicPressure * input.area;
            if (scale <= 0.0f)
                return {};
            return L3Coefficients{ result.force.x / scale, result.force.y / scale, result.force.z / scale,
                result.moment.x / (scale * input.span), result.moment.y / (scale * input.chord),
                result.moment.z / (scale * input.span) };
        }

        L3ControlTable GenerateControlTable(const L2Profile &l2, const AircraftConfig &config,
            const std::vector<float> &alphaGrid, const std::vector<float> &deflections, std::size_t axis)
        {
            L3ControlTable table;
            table.deflection = deflections;
            ResizeTable(table.increments, alphaGrid.size() * deflections.size());

            const float flaps = l2.flaps.empty() ? 0.0f : l2.flaps.front().flap;
            for (std::size_t ia = 0; ia < alphaGrid.size(); ++ia)
            {
                const L3Coefficients neutral = L2Coefficients(l2, config, alphaGrid[ia], 0.0f, flaps, glm::vec3{0.0f});
                for (std::size_t id = 0; id < deflections.size(); ++id)
                {
                    glm::vec3 deflection{ 0.0f };
                    deflection[axis] = deflections[id];
                    const L3Coefficients deflected =
                        L2Coefficients(l2, config, alphaGrid[ia], 0.0f, flaps, deflection);
                    WriteCoefficients(table.increments, ia * deflections.size() + id,
                        L3Coefficients{ deflected.CX - neutral.CX, deflected.CY - neutral.CY,
                            deflected.CZ - neutral.CZ, deflected.Cl - neutral.Cl, deflected.Cm - neutral.Cm,
                            deflected.Cn - neutral.Cn });
                }
            }
            return table;
        }

        void SortUnique(std::vector<float> &values)
        {
            std::sort(values.begin(), values.end());
            values.erase(std::unique(values.begin(), values.end()), values.end());
        }

        ForceMoment EvaluateL3(const AeroInput &input, const L3Profile &l3)
        {
            ForceMoment result;
            const float speed = input.air.trueAirspeed;
            if (speed < kAirDataMinSpeed)
                return result;

            const L3Sample sample = SampleL3(l3, input.air.angleOfAttack, input.air.sideslip, input.air.mach,
                input.flaps);
            result.outOfTable = sample.outOfTable;

            const AxisSample alpha = SampleAxis(l3.grid.alpha, input.air.angleOfAttack);
            const L3Coefficients control = InterpolateControl(l3, l3.controls.elevator, alpha, input.elevator);
            const L3Coefficients aileron = InterpolateControl(l3, l3.controls.aileron, alpha, input.aileron);
            const L3Coefficients rudder = InterpolateControl(l3, l3.controls.rudder, alpha, input.rudder);

            const float dynamicPressure = input.air.dynamicPressure;
            const float area = input.area;
            const float span = input.span;
            const float chord = input.chord;
            const float effectiveSpeed = std::max(speed, l3.minAirspeed);
            const float rollRate = input.angularVelocityBody.x * span / (2.0f * effectiveSpeed);
            const float pitchRate = input.angularVelocityBody.y * chord / (2.0f * effectiveSpeed);
            const float yawRate = input.angularVelocityBody.z * span / (2.0f * effectiveSpeed);

            const float rollScale = dynamicPressure * area * span;
            const float pitchScale = dynamicPressure * area * chord;

            result.sources.controlMoment = glm::vec3{ rollScale * (control.Cl + aileron.Cl + rudder.Cl),
                pitchScale * (control.Cm + aileron.Cm + rudder.Cm),
                rollScale * (control.Cn + aileron.Cn + rudder.Cn) };
            result.sources.dampingMoment = glm::vec3{
                rollScale * InterpolateRate(l3.rates.Clp, alpha) * rollRate,
                pitchScale * InterpolateRate(l3.rates.Cmq, alpha) * pitchRate,
                rollScale * InterpolateRate(l3.rates.Cnr, alpha) * yawRate };
            result.sources.stabilityMoment = glm::vec3{ rollScale * sample.base.Cl,
                pitchScale * sample.base.Cm, rollScale * sample.base.Cn };
            result.moment = result.sources.controlMoment + result.sources.dampingMoment
                + result.sources.stabilityMoment;

            const std::size_t cells = l3.grid.alpha.size() * l3.grid.beta.size() * l3.grid.mach.size()
                * l3.grid.flaps.size();
            if (TableHasSize(l3.base, cells))
            {
                const L3Coefficients coefficients{ sample.base.CX + control.CX + aileron.CX + rudder.CX,
                    sample.base.CY + control.CY + aileron.CY + rudder.CY,
                    sample.base.CZ + control.CZ + aileron.CZ + rudder.CZ, sample.base.Cl, sample.base.Cm,
                    sample.base.Cn };
                result.force =
                    dynamicPressure * area * glm::vec3{ coefficients.CX, coefficients.CY, coefficients.CZ };
            }

            const glm::vec3 flow = input.air.relativeVelocityBody / speed;
            const glm::vec3 liftAxis = glm::cross(glm::vec3{ 0.0f, 1.0f, 0.0f }, flow);
            const float liftAxisLength = glm::length(liftAxis);
            const float lift = liftAxisLength > 0.0f ? glm::dot(result.force, liftAxis) / liftAxisLength : 0.0f;
            const WindAxisForces axes = WindAxesToBody(input.air, lift, -glm::dot(result.force, flow),
                result.force.y);
            result.sources.lift = axes.lift;
            result.sources.drag = axes.drag;
            result.sources.side = axes.side;
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
                return profile.l2 ? EvaluateL2(input, *profile.l2) : ForceMoment{};
            }
        };

        class L3AerodynamicModel final : public IAerodynamicModel
        {
          public:
            ForceMoment Evaluate(const AeroInput &input, const AerodynamicProfile &profile) const override
            {
                return profile.l3 ? EvaluateL3(input, *profile.l3) : ForceMoment{};
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

    L3Sample SampleL3(const L3Profile &profile, float alpha, float beta, float mach, float flaps)
    {
        const AxisSample alphaAxis = SampleAxis(profile.grid.alpha, alpha);
        const AxisSample betaAxis = SampleAxis(profile.grid.beta, beta);
        const AxisSample machAxis = SampleAxis(profile.grid.mach, mach);
        const AxisSample flapAxis = SampleAxis(profile.grid.flaps, flaps);

        L3Sample sample;
        sample.alphaIndex = alphaAxis.lower;
        sample.betaIndex = betaAxis.lower;
        sample.machIndex = machAxis.lower;
        sample.flapIndex = flapAxis.lower;
        sample.outOfTable =
            alphaAxis.outOfTable || betaAxis.outOfTable || machAxis.outOfTable || flapAxis.outOfTable;
        sample.base = InterpolateBase(profile, alphaAxis, betaAxis, machAxis, flapAxis);
        return sample;
    }

    L3Grid DefaultL3Grid(const AircraftConfig &config)
    {
        L3Grid grid;
        for (float degrees = kL3DefaultAlphaMinDegrees; degrees <= kL3DefaultAlphaMaxDegrees + 1e-3f;
             degrees += kL3DefaultAlphaStepDegrees)
            grid.alpha.push_back(glm::radians(degrees));

        if (config.aero.l2.has_value())
        {
            const L2Profile &l2 = *config.aero.l2;
            for (const L2PostStallPoint &point : l2.stall.postStall)
                if (point.alpha <= glm::radians(kL3DefaultPostStallLimitDegrees))
                    grid.alpha.push_back(point.alpha);
            for (const L2FlapPoint &point : l2.flaps)
                grid.flaps.push_back(point.flap);
        }

        SortUnique(grid.alpha);
        grid.beta = { glm::radians(-10.0f), 0.0f, glm::radians(10.0f) };
        grid.mach = { 0.2f };
        if (grid.flaps.empty())
            grid.flaps.push_back(0.0f);
        return grid;
    }

    L3DeflectionGrids DefaultL3Deflections(const AircraftConfig &config)
    {
        const auto build = [](float maximum)
        {
            return std::vector<float>{ -maximum, -0.5f * maximum, 0.0f, 0.5f * maximum, maximum };
        };
        return L3DeflectionGrids{ build(config.actuators.elevator.maxDeflection),
            build(config.actuators.aileron.maxDeflection), build(config.actuators.rudder.maxDeflection) };
    }

    L3Profile GenerateL3FromL2(const L2Profile &profile, const AircraftConfig &config, const L3Grid &grid,
        const L3DeflectionGrids &deflections)
    {
        L3Profile out;
        out.minAirspeed = profile.minAirspeed;
        out.grid = grid;

        const std::size_t nAlpha = grid.alpha.size();
        const std::size_t nBeta = grid.beta.size();
        const std::size_t nMach = grid.mach.size();
        const std::size_t nFlaps = grid.flaps.size();
        ResizeTable(out.base, nAlpha * nBeta * nMach * nFlaps);

        for (std::size_t ia = 0; ia < nAlpha; ++ia)
        {
            for (std::size_t ib = 0; ib < nBeta; ++ib)
            {
                for (std::size_t iFlap = 0; iFlap < nFlaps; ++iFlap)
                {
                    const L3Coefficients coefficients = L2Coefficients(profile, config, grid.alpha[ia],
                        grid.beta[ib], grid.flaps[iFlap], glm::vec3{ 0.0f });
                    for (std::size_t im = 0; im < nMach; ++im)
                        WriteCoefficients(out.base, ((ia * nBeta + ib) * nMach + im) * nFlaps + iFlap, coefficients);
                }
            }
        }

        out.controls.elevator =
            GenerateControlTable(profile, config, grid.alpha, deflections.elevator, 0);
        out.controls.aileron = GenerateControlTable(profile, config, grid.alpha, deflections.aileron, 1);
        out.controls.rudder = GenerateControlTable(profile, config, grid.alpha, deflections.rudder, 2);

        out.rates.Clp.assign(nAlpha, profile.Clp);
        out.rates.Cmq.assign(nAlpha, profile.Cmq);
        out.rates.Cnr.assign(nAlpha, profile.Cnr);
        return out;
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
            return profile.l3.has_value();
        }
        return false;
    }

    const IAerodynamicModel &AerodynamicModelFor(AerodynamicTier tier)
    {
        static const ZeroAerodynamicModel kZero;
        static const L1AerodynamicModel kL1;
        static const L2AerodynamicModel kL2;
        static const L3AerodynamicModel kL3;

        switch (tier)
        {
        case AerodynamicTier::L1:
            return kL1;
        case AerodynamicTier::L2:
            return kL2;
        case AerodynamicTier::L3:
            return kL3;
        }
        return kZero;
    }
}

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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

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
        // An L3 input fell outside one of the table axes and was clamped to its edge.
        bool outOfTable = false;
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

    struct L2PostStallPoint
    {
        float alpha = 0.0f; // radians
        float CL = 0.0f;
        float CD = 0.0f;
        float Cm = 0.0f;
    };

    struct L2FlapPoint
    {
        float flap = 0.0f; // radians
        float dCL = 0.0f;
        float dCD = 0.0f;
        float dCm = 0.0f;
        float stallShift = 0.0f; // radians, positive stall angle shift
    };

    struct L2Stall
    {
        float positive = 0.0f; // radians
        float negative = 0.0f; // radians, below zero
        float blend = 0.0f;    // radians, width after the critical angle
        float controlScale = 1.0f;
        float stabilityScale = 1.0f;
        std::vector<L2PostStallPoint> postStall;
    };

    struct L2Profile
    {
        float CL0 = 0.0f;
        float CLalpha = 0.0f;
        float CD0 = 0.0f;
        float k = 0.0f;
        float Cm0 = 0.0f;
        float Cmalpha = 0.0f;
        float Cmq = 0.0f;
        float Cmde = 0.0f;
        float CYbeta = 0.0f;
        float CYdr = 0.0f;
        float Clbeta = 0.0f;
        float Clp = 0.0f;
        float Clda = 0.0f;
        float Cnbeta = 0.0f;
        float Cnr = 0.0f;
        float Cndr = 0.0f;
        float Cnda = 0.0f;
        float minAirspeed = 0.0f;
        L2Stall stall;
        std::vector<L2FlapPoint> flaps;
    };

    // Post-stall blend weight in 0..1 for the given angle of attack and flap angle.
    float L2StallFraction(const L2Profile &profile, float alpha, float flaps);
    // Positive critical angle of attack, flaps already applied.
    float L2CriticalAngleOfAttack(const L2Profile &profile, float flaps);

    struct AircraftConfig;

    struct L3Coefficients
    {
        float CX = 0.0f;
        float CY = 0.0f;
        float CZ = 0.0f;
        float Cl = 0.0f;
        float Cm = 0.0f;
        float Cn = 0.0f;
    };

    struct L3Grid
    {
        std::vector<float> alpha; // radians
        std::vector<float> beta;  // radians
        std::vector<float> mach;
        std::vector<float> flaps; // radians
    };

    // One flat array per coefficient; the index order is documented on L3Profile::base.
    struct L3Table
    {
        std::vector<float> CX;
        std::vector<float> CY;
        std::vector<float> CZ;
        std::vector<float> Cl;
        std::vector<float> Cm;
        std::vector<float> Cn;
    };

    struct L3ControlTable
    {
        std::vector<float> deflection; // radians, strictly increasing
        // Over (alpha, deflection) as ia*nDeflection + id; increments relative to zero deflection.
        L3Table increments;
    };

    struct L3Profile
    {
        float minAirspeed = 0.0f;
        L3Grid grid;
        // Over (alpha, beta, mach, flaps) as ((ia*nBeta + ib)*nMach + im)*nFlaps + if.
        L3Table base;
        struct Controls
        {
            L3ControlTable elevator;
            L3ControlTable aileron;
            L3ControlTable rudder;
        } controls;
        struct Rates
        {
            std::vector<float> Clp; // over the alpha grid
            std::vector<float> Cmq;
            std::vector<float> Cnr;
        } rates;
    };

    struct L3Sample
    {
        L3Coefficients base;
        // Lower cell index per axis; a clamped input reports the edge cell.
        std::size_t alphaIndex = 0;
        std::size_t betaIndex = 0;
        std::size_t machIndex = 0;
        std::size_t flapIndex = 0;
        bool outOfTable = false;
    };

    // Multilinear base coefficients at the input; every axis is clamped at its ends.
    L3Sample SampleL3(const L3Profile &profile, float alpha, float beta, float mach, float flaps);

    struct L3DeflectionGrids
    {
        std::vector<float> elevator; // radians
        std::vector<float> aileron;
        std::vector<float> rudder;
    };

    L3Grid DefaultL3Grid(const AircraftConfig &config);
    L3DeflectionGrids DefaultL3Deflections(const AircraftConfig &config);
    // Samples the L2 profile at every grid point; L2 ignores Mach, so every Mach slice is equal.
    L3Profile GenerateL3FromL2(const L2Profile &profile, const AircraftConfig &config, const L3Grid &grid,
        const L3DeflectionGrids &deflections);

    struct AerodynamicProfile
    {
        std::optional<L1Profile> l1;
        std::optional<L2Profile> l2;
        std::optional<L3Profile> l3;
    };

    bool HasAerodynamicProfile(const AerodynamicProfile &profile, AerodynamicTier tier);

    class IAerodynamicModel
    {
      public:
        virtual ~IAerodynamicModel() = default;
        virtual ForceMoment Evaluate(const AeroInput &input, const AerodynamicProfile &profile) const = 0;
    };

    // Stateless models; a tier without a profile yields zero.
    const IAerodynamicModel &AerodynamicModelFor(AerodynamicTier tier);
}

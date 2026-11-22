/**
 * @file AerodynamicsL3.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/AerodynamicModel L3, its L2 generator, the aero.L3 parser and the serializer.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AerodynamicModel.h>

#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>
#include <flight/FlightTelemetry.h>
#include <flight/components/Aircraft.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    constexpr float kArea = 16.0f;
    constexpr float kSpan = 11.0f;
    constexpr float kChord = 1.5f;
    constexpr float kMachPoint = 0.2f;

    mir::AirData AirAt(float speed, float alpha, float beta)
    {
        mir::AirData air;
        const float cosAlpha = std::cos(alpha);
        const float cosBeta = std::cos(beta);
        air.relativeVelocityBody = speed * glm::vec3{ cosAlpha * cosBeta, std::sin(beta),
            std::sin(alpha) * cosBeta };
        air.trueAirspeed = speed;
        air.angleOfAttack = alpha;
        air.sideslip = beta;
        air.dynamicPressure = 0.5f * 1.225f * speed * speed;
        air.density = 1.225f;
        air.mach = kMachPoint;
        return air;
    }

    mir::AeroInput L3Input(float speed, float alpha, float beta = 0.0f)
    {
        mir::AeroInput input;
        input.air = AirAt(speed, alpha, beta);
        input.elevatorMax = glm::radians(25.0f);
        input.aileronMax = glm::radians(20.0f);
        input.rudderMax = glm::radians(25.0f);
        input.area = kArea;
        input.span = kSpan;
        input.chord = kChord;
        return input;
    }

    mir::AerodynamicProfile WrapL3(mir::L3Profile profile)
    {
        mir::AerodynamicProfile wrapped;
        wrapped.l3 = std::move(profile);
        return wrapped;
    }

    mir::L3Profile LinearProfile()
    {
        mir::L3Profile profile;
        profile.minAirspeed = 10.0f;
        profile.grid.alpha = { 0.0f, glm::radians(10.0f) };
        profile.grid.beta = { 0.0f, glm::radians(10.0f) };
        profile.grid.mach = { kMachPoint };
        profile.grid.flaps = { 0.0f };
        profile.base.CX = { 0.0f, 10.0f, 20.0f, 40.0f };
        profile.base.CY.assign(4, 0.0f);
        profile.base.CZ.assign(4, 0.0f);
        profile.base.Cl.assign(4, 0.0f);
        profile.base.Cm.assign(4, 0.0f);
        profile.base.Cn.assign(4, 0.0f);
        return profile;
    }

    mir::L3Profile CornerProfile()
    {
        mir::L3Profile profile;
        profile.minAirspeed = 10.0f;
        profile.grid.alpha = { 0.0f, glm::radians(10.0f) };
        profile.grid.beta = { 0.0f, glm::radians(10.0f) };
        profile.grid.mach = { 0.2f, 0.6f };
        profile.grid.flaps = { 0.0f, glm::radians(20.0f) };
        profile.base.CX.resize(16);
        for (std::size_t i = 0; i < profile.base.CX.size(); ++i)
            profile.base.CX[i] = static_cast<float>(i);
        profile.base.CY.assign(16, 0.0f);
        profile.base.CZ.assign(16, 0.0f);
        profile.base.Cl.assign(16, 0.0f);
        profile.base.Cm.assign(16, 0.0f);
        profile.base.Cn.assign(16, 0.0f);
        return profile;
    }

    mir::L2Profile LightAircraft()
    {
        mir::L2Profile profile;
        profile.CL0 = 0.25f;
        profile.CLalpha = 5.0f;
        profile.CD0 = 0.03f;
        profile.k = 0.05f;
        profile.Cm0 = 0.05f;
        profile.Cmalpha = -0.6f;
        profile.Cmq = -12.0f;
        profile.Cmde = 1.2f;
        profile.CYbeta = -0.4f;
        profile.CYdr = -0.1f;
        profile.Clbeta = -0.1f;
        profile.Clp = -0.5f;
        profile.Clda = 0.08f;
        profile.Cnbeta = 0.12f;
        profile.Cnr = -0.15f;
        profile.Cndr = 0.08f;
        profile.Cnda = -0.02f;
        profile.minAirspeed = 10.0f;

        profile.stall.positive = glm::radians(15.0f);
        profile.stall.negative = glm::radians(-15.0f);
        profile.stall.blend = glm::radians(6.0f);
        profile.stall.controlScale = 0.4f;
        profile.stall.stabilityScale = 0.3f;
        profile.stall.postStall = {
            { glm::radians(-15.0f), -0.9f, 0.20f, -0.15f },
            { 0.0f, 0.25f, 0.05f, 0.05f },
            { glm::radians(15.0f), 1.35f, 0.16f, -0.10f },
            { glm::radians(21.0f), 1.02f, 0.24f, -0.13f },
        };
        profile.flaps = {
            { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
            { glm::radians(10.0f), 0.25f, 0.005f, -0.01f, glm::radians(-0.5f) },
            { glm::radians(20.0f), 0.5f, 0.012f, -0.02f, glm::radians(-1.0f) },
            { glm::radians(30.0f), 0.7f, 0.02f, -0.03f, glm::radians(-1.0f) },
        };
        return profile;
    }

    mir::AircraftConfig GeneratorConfig()
    {
        mir::AircraftConfig config;
        config.reference.area = kArea;
        config.reference.span = kSpan;
        config.reference.chord = kChord;
        config.actuators.elevator.maxDeflection = glm::radians(25.0f);
        config.actuators.aileron.maxDeflection = glm::radians(20.0f);
        config.actuators.rudder.maxDeflection = glm::radians(25.0f);
        config.aero.l2 = LightAircraft();
        return config;
    }

    void CheckMatches(const mir::ForceMoment &actual, const mir::ForceMoment &expected)
    {
        CHECK(actual.force.x == Catch::Approx(expected.force.x).epsilon(1e-3).margin(1e-4f));
        CHECK(actual.force.y == Catch::Approx(expected.force.y).epsilon(1e-3).margin(1e-4f));
        CHECK(actual.force.z == Catch::Approx(expected.force.z).epsilon(1e-3).margin(1e-4f));
        CHECK(actual.moment.x == Catch::Approx(expected.moment.x).epsilon(1e-3).margin(1e-4f));
        CHECK(actual.moment.y == Catch::Approx(expected.moment.y).epsilon(1e-3).margin(1e-4f));
        CHECK(actual.moment.z == Catch::Approx(expected.moment.z).epsilon(1e-3).margin(1e-4f));
    }

    constexpr std::string_view kStall = R"({
        "positiveDeg": 15.0, "negativeDeg": -15.0, "blendDeg": 6.0,
        "controlScale": 0.4, "stabilityScale": 0.3,
        "postStall": [[-15, -0.9, 0.2, -0.15], [0, 0.25, 0.05, 0.05],
                      [15, 1.35, 0.16, -0.1], [21, 1.02, 0.24, -0.13]]
    })";

    constexpr std::string_view kFlaps = R"([[0, 0, 0, 0, 0], [10, 0.25, 0.005, -0.01, -0.5],
        [20, 0.5, 0.012, -0.02, -1], [30, 0.7, 0.02, -0.03, -1]])";

    std::string L2Object()
    {
        std::string object = R"({ "L2": {
            "CL0": 0.25, "CLalpha": 5.0, "CD0": 0.03, "k": 0.05,
            "Cm0": 0.05, "Cmalpha": -0.6, "Cmq": -12, "Cmde": 1.2,
            "CYbeta": -0.4, "CYdr": -0.1, "Clbeta": -0.1, "Clp": -0.5, "Clda": 0.08,
            "Cnbeta": 0.12, "Cnr": -0.15, "Cndr": 0.08, "Cnda": -0.02,
            "minAirspeed": 10,
            "stall": )";
        object += kStall;
        object += R"(, "flaps": )";
        object += kFlaps;
        object += "}";
        return object;
    }

    std::string L3Object(std::string_view betaDeg = "[0]", std::string_view baseCm = "[0.1, 0.2]")
    {
        std::string object = R"({ "L3": {
            "minAirspeed": 10,
            "grid": { "alphaDeg": [-10, 0], "betaDeg": )";
        object += betaDeg;
        object += R"(, "mach": [0.2], "flapsDeg": [0] },
            "base": { "CX": [0, 0], "CY": [0, 0], "CZ": [0, 0], "Cl": [0, 0], "Cm": )";
        object += baseCm;
        object += R"(, "Cn": [0, 0] },
            "controls": {
                "elevator": { "deflectionDeg": [-10, 0, 10], "CX": [0, 0, 0, 0, 0, 0],
                    "CY": [0, 0, 0, 0, 0, 0], "CZ": [0, 0, 0, 0, 0, 0], "Cl": [0, 0, 0, 0, 0, 0],
                    "Cm": [0, 0, 0, 0, 0, 0], "Cn": [0, 0, 0, 0, 0, 0] },
                "aileron": { "deflectionDeg": [-10, 0, 10], "CX": [0, 0, 0, 0, 0, 0],
                    "CY": [0, 0, 0, 0, 0, 0], "CZ": [0, 0, 0, 0, 0, 0], "Cl": [0, 0, 0, 0, 0, 0],
                    "Cm": [0, 0, 0, 0, 0, 0], "Cn": [0, 0, 0, 0, 0, 0] },
                "rudder": { "deflectionDeg": [-10, 0, 10], "CX": [0, 0, 0, 0, 0, 0],
                    "CY": [0, 0, 0, 0, 0, 0], "CZ": [0, 0, 0, 0, 0, 0], "Cl": [0, 0, 0, 0, 0, 0],
                    "Cm": [0, 0, 0, 0, 0, 0], "Cn": [0, 0, 0, 0, 0, 0] }
            },
            "rates": { "Clp": [0, 0], "Cmq": [0, 0], "Cnr": [0, 0] }
        } })";
        return object;
    }

    std::string ConfigWithAero(std::string_view aero)
    {
        std::string document = R"({
            "version": 1, "mass": 10, "inertia": { "diagonal": [1, 2, 3] },
            "centerOfMass": [0, 0, 0], "reference": { "area": 16, "span": 11, "chord": 1.5 },
            "camera": { "eye": [0, 0, 0], "chaseDistance": 0, "chaseHeight": 0 },
            "throttleRate": 0.5,
            "actuators": {
                "elevator": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
                "aileron": { "maxDeflectionDeg": 20, "rateDegPerSec": 80, "delay": 0 },
                "rudder": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
                "flaps": { "detentsDeg": [0], "rateDegPerSec": 5 },
                "trim": { "stepDeg": 0.5, "maxDeg": 10 }
            },
            "engines": [],
            "gear": [],
            "aero": )";
        document += aero;
        document += "}";
        return document;
    }
}

TEST_CASE("L3 base coefficients blend the table corners", "[flight][aero]")
{
    const mir::L3Profile profile = LinearProfile();
    const mir::L3Sample sample = mir::SampleL3(profile, glm::radians(5.0f), glm::radians(5.0f), kMachPoint, 0.0f);

    CHECK(sample.alphaIndex == 0);
    CHECK(sample.betaIndex == 0);
    CHECK(sample.machIndex == 0);
    CHECK(sample.flapIndex == 0);
    CHECK_FALSE(sample.outOfTable);
    CHECK(sample.base.CX == Catch::Approx(17.5f));
    CHECK(sample.base.CY == Catch::Approx(0.0f));

    mir::AeroInput input = L3Input(50.0f, glm::radians(5.0f), glm::radians(5.0f));
    const mir::ForceMoment result =
        mir::AerodynamicModelFor(mir::AerodynamicTier::L3).Evaluate(input, WrapL3(profile));
    CHECK(result.force.x == Catch::Approx(input.air.dynamicPressure * kArea * 17.5f));
    CHECK(result.force.y == Catch::Approx(0.0f).margin(1e-4f));
    CHECK(result.force.z == Catch::Approx(0.0f).margin(1e-4f));
    CHECK_FALSE(result.outOfTable);
}

TEST_CASE("L3 clamps every axis at its edge and flags it", "[flight][aero]")
{
    const mir::L3Profile profile = CornerProfile();
    const float midAlpha = glm::radians(5.0f);
    const float midBeta = glm::radians(5.0f);
    const float midFlaps = glm::radians(10.0f);

    const mir::L3Sample inside = mir::SampleL3(profile, midAlpha, midBeta, 0.4f, midFlaps);
    CHECK_FALSE(inside.outOfTable);
    CHECK(inside.base.CX == Catch::Approx(7.5f));

    const mir::L3Sample alphaLow = mir::SampleL3(profile, glm::radians(-5.0f), midBeta, 0.4f, midFlaps);
    CHECK(alphaLow.outOfTable);
    CHECK(alphaLow.alphaIndex == 0);
    CHECK(alphaLow.base.CX == mir::SampleL3(profile, 0.0f, midBeta, 0.4f, midFlaps).base.CX);

    const mir::L3Sample alphaHigh = mir::SampleL3(profile, glm::radians(20.0f), midBeta, 0.4f, midFlaps);
    CHECK(alphaHigh.outOfTable);
    CHECK(alphaHigh.alphaIndex == 1);
    CHECK(alphaHigh.base.CX ==
        mir::SampleL3(profile, glm::radians(10.0f), midBeta, 0.4f, midFlaps).base.CX);

    const mir::L3Sample betaLow = mir::SampleL3(profile, midAlpha, glm::radians(-5.0f), 0.4f, midFlaps);
    CHECK(betaLow.outOfTable);
    CHECK(betaLow.betaIndex == 0);
    CHECK(betaLow.base.CX == mir::SampleL3(profile, midAlpha, 0.0f, 0.4f, midFlaps).base.CX);

    const mir::L3Sample betaHigh = mir::SampleL3(profile, midAlpha, glm::radians(20.0f), 0.4f, midFlaps);
    CHECK(betaHigh.outOfTable);
    CHECK(betaHigh.betaIndex == 1);
    CHECK(betaHigh.base.CX ==
        mir::SampleL3(profile, midAlpha, glm::radians(10.0f), 0.4f, midFlaps).base.CX);

    const mir::L3Sample machLow = mir::SampleL3(profile, midAlpha, midBeta, 0.1f, midFlaps);
    CHECK(machLow.outOfTable);
    CHECK(machLow.machIndex == 0);
    CHECK(machLow.base.CX == mir::SampleL3(profile, midAlpha, midBeta, 0.2f, midFlaps).base.CX);

    const mir::L3Sample machHigh = mir::SampleL3(profile, midAlpha, midBeta, 0.9f, midFlaps);
    CHECK(machHigh.outOfTable);
    CHECK(machHigh.machIndex == 1);
    CHECK(machHigh.base.CX == mir::SampleL3(profile, midAlpha, midBeta, 0.6f, midFlaps).base.CX);

    const mir::L3Sample flapLow = mir::SampleL3(profile, midAlpha, midBeta, 0.4f, glm::radians(-5.0f));
    CHECK(flapLow.outOfTable);
    CHECK(flapLow.flapIndex == 0);
    CHECK(flapLow.base.CX == mir::SampleL3(profile, midAlpha, midBeta, 0.4f, 0.0f).base.CX);

    const mir::L3Sample flapHigh = mir::SampleL3(profile, midAlpha, midBeta, 0.4f, glm::radians(30.0f));
    CHECK(flapHigh.outOfTable);
    CHECK(flapHigh.flapIndex == 1);
    CHECK(flapHigh.base.CX ==
        mir::SampleL3(profile, midAlpha, midBeta, 0.4f, glm::radians(20.0f)).base.CX);
}

TEST_CASE("A single-point Mach axis is constant", "[flight][aero]")
{
    const mir::L3Profile profile = LinearProfile();
    const float alpha = glm::radians(3.0f);
    const float beta = glm::radians(4.0f);

    const mir::L3Sample inTable = mir::SampleL3(profile, alpha, beta, kMachPoint, 0.0f);
    const mir::L3Sample onPoint = mir::SampleL3(profile, alpha, beta, 0.35f, 0.0f);

    CHECK_FALSE(inTable.outOfTable);
    CHECK(onPoint.outOfTable);
    CHECK(onPoint.base.CX == Catch::Approx(inTable.base.CX));
    CHECK(onPoint.base.Cm == Catch::Approx(inTable.base.Cm));
}

TEST_CASE("L3 generated from L2 matches L2 at the grid points", "[flight][aero]")
{
    const mir::AircraftConfig config = GeneratorConfig();
    const mir::L3Grid grid = mir::DefaultL3Grid(config);
    mir::AerodynamicProfile l3Profile =
        WrapL3(mir::GenerateL3FromL2(*config.aero.l2, config, grid, mir::DefaultL3Deflections(config)));
    const mir::AerodynamicProfile l2Profile = config.aero;

    const auto compare = [&](mir::AeroInput input)
    {
        const mir::ForceMoment l2 =
            mir::AerodynamicModelFor(mir::AerodynamicTier::L2).Evaluate(input, l2Profile);
        const mir::ForceMoment l3 =
            mir::AerodynamicModelFor(mir::AerodynamicTier::L3).Evaluate(input, l3Profile);
        CheckMatches(l3, l2);
        CHECK_FALSE(l3.outOfTable);
    };

    for (const float alphaDegrees : { -10.0f, 0.0f, 5.0f, 15.0f, 21.0f })
    {
        for (const float betaDegrees : { -10.0f, 0.0f, 10.0f })
        {
            for (const float flapDegrees : { 0.0f, 10.0f, 20.0f, 30.0f })
            {
                mir::AeroInput input =
                    L3Input(50.0f, glm::radians(alphaDegrees), glm::radians(betaDegrees));
                input.flaps = glm::radians(flapDegrees);
                compare(input);
            }
        }
    }

    mir::AeroInput controls = L3Input(50.0f, glm::radians(5.0f), glm::radians(10.0f));
    controls.elevator = glm::radians(12.5f);
    controls.aileron = glm::radians(-10.0f);
    controls.rudder = glm::radians(25.0f);
    compare(controls);

    controls.angularVelocityBody = glm::vec3{ 0.4f, -0.2f, 0.3f };
    compare(controls);
}

TEST_CASE("L3 control increments reproduce L2 at a table deflection", "[flight][aero]")
{
    const mir::AircraftConfig config = GeneratorConfig();
    mir::AerodynamicProfile l3Profile = WrapL3(mir::GenerateL3FromL2(*config.aero.l2, config,
        mir::DefaultL3Grid(config), mir::DefaultL3Deflections(config)));
    const mir::AerodynamicProfile l2Profile = config.aero;

    mir::AeroInput input = L3Input(50.0f, glm::radians(5.0f), 0.0f);
    input.elevator = glm::radians(25.0f);
    const mir::ForceMoment l2 = mir::AerodynamicModelFor(mir::AerodynamicTier::L2).Evaluate(input, l2Profile);
    const mir::ForceMoment l3 = mir::AerodynamicModelFor(mir::AerodynamicTier::L3).Evaluate(input, l3Profile);

    CheckMatches(l3, l2);
    CHECK(l3.sources.controlMoment.y > 0.0f);
}

TEST_CASE("The default L3 grid covers the L2 rows", "[flight][aero]")
{
    const mir::AircraftConfig config = GeneratorConfig();
    const mir::L3Grid grid = mir::DefaultL3Grid(config);

    CHECK(grid.alpha.front() == Catch::Approx(glm::radians(-15.0f)));
    CHECK(grid.alpha.back() == Catch::Approx(glm::radians(21.0f)));
    CHECK(std::is_sorted(grid.alpha.begin(), grid.alpha.end()));
    CHECK(grid.beta.size() == 3);
    CHECK(grid.mach.size() == 1);
    CHECK(grid.mach.front() == Catch::Approx(0.2f));
    REQUIRE(grid.flaps.size() == 4);
    CHECK(grid.flaps.back() == Catch::Approx(glm::radians(30.0f)));

    const mir::L3DeflectionGrids deflections = mir::DefaultL3Deflections(config);
    REQUIRE(deflections.elevator.size() == 5);
    CHECK(deflections.elevator.front() == Catch::Approx(glm::radians(-25.0f)));
    CHECK(deflections.elevator[2] == Catch::Approx(0.0f));
    CHECK(deflections.elevator.back() == Catch::Approx(glm::radians(25.0f)));
}

TEST_CASE("L3 uses its profile and a missing one yields nothing", "[flight][config][aero]")
{
    const mir::AircraftConfig config = GeneratorConfig();
    mir::AerodynamicProfile profile =
        WrapL3(mir::GenerateL3FromL2(*config.aero.l2, config, mir::DefaultL3Grid(config),
            mir::DefaultL3Deflections(config)));

    CHECK(mir::HasAerodynamicProfile(profile, mir::AerodynamicTier::L3));
    const mir::ForceMoment result =
        mir::AerodynamicModelFor(mir::AerodynamicTier::L3).Evaluate(L3Input(50.0f, glm::radians(5.0f)), profile);
    CHECK(glm::length(result.force) > 0.0f);
    CHECK(glm::length(result.moment) > 0.0f);

    const mir::AerodynamicProfile missing;
    CHECK_FALSE(mir::HasAerodynamicProfile(missing, mir::AerodynamicTier::L3));
    const mir::ForceMoment none =
        mir::AerodynamicModelFor(mir::AerodynamicTier::L3).Evaluate(L3Input(50.0f, 0.0f), missing);
    CHECK(none.force == glm::vec3{ 0.0f });
    CHECK(none.moment == glm::vec3{ 0.0f });
}

TEST_CASE("L3 yields nothing below the minimum airspeed", "[flight][aero]")
{
    const mir::L3Profile profile = CornerProfile();
    mir::AeroInput input = L3Input(0.05f, 0.0f);
    input.elevator = glm::radians(5.0f);
    input.angularVelocityBody = glm::vec3{ 1.0f };

    const mir::ForceMoment result =
        mir::AerodynamicModelFor(mir::AerodynamicTier::L3).Evaluate(input, WrapL3(profile));
    CHECK(result.force == glm::vec3{ 0.0f });
    CHECK(result.moment == glm::vec3{ 0.0f });
    CHECK_FALSE(result.outOfTable);
}

TEST_CASE("The telemetry reports the L3 out-of-table flag", "[flight][aero]")
{
    mir::AircraftConfig config;
    config.mass = 1000.0f;
    const mir::AircraftState state;
    const mir::AirData air = AirAt(50.0f, 0.0f, 0.0f);
    mir::AircraftRuntime runtime;
    runtime.aero.force = glm::vec3{ 100.0f, 0.0f, -200.0f };
    runtime.aero.outOfTable = true;

    mir::FlightTelemetry telemetry;
    mir::FillFlightTelemetry(config, state, air, 0.0f, false, runtime, telemetry);
    CHECK(telemetry.aeroOutOfTable);

    runtime.aero.outOfTable = false;
    mir::FillFlightTelemetry(config, state, air, 0.0f, false, runtime, telemetry);
    CHECK_FALSE(telemetry.aeroOutOfTable);
}

TEST_CASE("An L3 config parses", "[flight][config]")
{
    mir::AircraftConfig config;
    std::string error;
    REQUIRE(mir::ParseAircraftConfig(ConfigWithAero(L3Object()), config, error));
    REQUIRE(config.aero.l3.has_value());
    const mir::L3Profile &l3 = *config.aero.l3;
    CHECK(l3.minAirspeed == Catch::Approx(10.0f));
    REQUIRE(l3.grid.alpha.size() == 2);
    CHECK(l3.grid.alpha.front() == Catch::Approx(glm::radians(-10.0f)));
    CHECK(l3.grid.beta.size() == 1);
    CHECK(l3.grid.mach.size() == 1);
    CHECK(l3.grid.flaps.size() == 1);
    CHECK(l3.base.CX.size() == 2);
    CHECK(l3.base.Cm[1] == Catch::Approx(0.2f));
    REQUIRE(l3.controls.elevator.deflection.size() == 3);
    CHECK(l3.controls.elevator.increments.Cn.size() == 6);
    CHECK(l3.rates.Clp.size() == 2);
    CHECK(mir::HasAerodynamicProfile(config.aero, mir::AerodynamicTier::L3));
    CHECK_FALSE(mir::HasAerodynamicProfile(config.aero, mir::AerodynamicTier::L2));
}

TEST_CASE("L3 config errors name the JSON path", "[flight][config]")
{
    mir::AircraftConfig config;
    std::string error;

    SECTION("a base array of the wrong size")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(ConfigWithAero(L3Object("[0]", "[0.1]")), config, error));
        CHECK(error == "aero.L3.base.Cm: expected 2 values, got 1");
    }

    SECTION("an unsorted grid axis")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(ConfigWithAero(L3Object("[5, 0]")), config, error));
        CHECK(error == "aero.L3.grid.betaDeg: must be strictly increasing");
    }

    SECTION("an empty grid axis")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(ConfigWithAero(L3Object("[]")), config, error));
        CHECK(error == "aero.L3.grid.betaDeg: must not be empty");
    }

    SECTION("a missing control table")
    {
        std::string object = L3Object();
        const std::size_t keyStart = object.find("\"rudder\":");
        const std::size_t comma = object.rfind(',', keyStart);
        const std::size_t objectEnd = object.find('}', keyStart);
        object.erase(comma, objectEnd - comma + 1);
        REQUIRE_FALSE(mir::ParseAircraftConfig(ConfigWithAero(object), config, error));
        CHECK(error == "aero.L3.controls.rudder: missing");
    }
}

TEST_CASE("An L3 profile round trips through the serializer", "[flight][config]")
{
    mir::AircraftConfig config;
    config.version = 1;
    config.mass = 1000.0f;
    config.inertiaDiagonal = glm::vec3{ 1000.0f, 2000.0f, 3000.0f };
    config.centerOfMass = glm::vec3{ 0.1f, 0.0f, 0.2f };
    config.reference.area = kArea;
    config.reference.span = kSpan;
    config.reference.chord = kChord;
    config.throttleRate = 0.5f;
    config.actuators.elevator = { glm::radians(25.0f), glm::radians(60.0f), 0.05f };
    config.actuators.aileron = { glm::radians(20.0f), glm::radians(80.0f), 0.05f };
    config.actuators.rudder = { glm::radians(25.0f), glm::radians(60.0f), 0.05f };
    config.actuators.flaps.detents = { 0.0f };
    config.actuators.flaps.rate = glm::radians(5.0f);
    config.actuators.trim = { glm::radians(0.5f), glm::radians(10.0f) };
    config.aero.l2 = LightAircraft();
    config.aero.l3 = mir::GenerateL3FromL2(*config.aero.l2, config, mir::DefaultL3Grid(config),
        mir::DefaultL3Deflections(config));

    const std::string document = mir::SerializeAircraftConfig(config);
    mir::AircraftConfig parsed;
    std::string error;
    REQUIRE(mir::ParseAircraftConfig(document, parsed, error));
    REQUIRE(parsed.aero.l3.has_value());

    const mir::L3Profile &l3 = *parsed.aero.l3;
    CHECK(l3.minAirspeed == Catch::Approx(config.aero.l3->minAirspeed));
    REQUIRE(l3.grid.alpha.size() == config.aero.l3->grid.alpha.size());
    CHECK(l3.grid.alpha.front() == Catch::Approx(config.aero.l3->grid.alpha.front()));
    CHECK(l3.grid.beta.back() == Catch::Approx(config.aero.l3->grid.beta.back()));
    CHECK(l3.grid.mach.front() == Catch::Approx(config.aero.l3->grid.mach.front()));
    CHECK(l3.grid.flaps.back() == Catch::Approx(config.aero.l3->grid.flaps.back()));
    REQUIRE(l3.base.Cm.size() == config.aero.l3->base.Cm.size());
    CHECK(l3.base.Cm.back() == Catch::Approx(config.aero.l3->base.Cm.back()));
    REQUIRE(l3.controls.aileron.deflection.size() == config.aero.l3->controls.aileron.deflection.size());
    CHECK(l3.controls.aileron.deflection.back() ==
        Catch::Approx(config.aero.l3->controls.aileron.deflection.back()));
    CHECK(l3.controls.aileron.increments.Cl.back() ==
        Catch::Approx(config.aero.l3->controls.aileron.increments.Cl.back()));
    CHECK(l3.rates.Cnr.back() == Catch::Approx(config.aero.l3->rates.Cnr.back()));
}

/**
 * @file AerodynamicsL2.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/AerodynamicModel L2 and the aero.L2 parser.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AerodynamicModel.h>

#include <flight/AircraftConfig.h>
#include <flight/components/Aircraft.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    constexpr float kArea = 16.0f;
    constexpr float kSpan = 11.0f;
    constexpr float kChord = 1.5f;

    mir::AirData AirAt(float speed, float alpha, float beta)
    {
        mir::AirData air;
        const float cosAlpha = std::cos(alpha);
        air.relativeVelocityBody = speed * glm::vec3{ cosAlpha * std::cos(beta), std::sin(beta),
            std::sin(alpha) * std::cos(beta) };
        air.trueAirspeed = speed;
        air.angleOfAttack = alpha;
        air.sideslip = beta;
        air.dynamicPressure = 0.5f * 1.225f * speed * speed;
        air.density = 1.225f;
        return air;
    }

    mir::AeroInput L2Input(float speed, float alpha, float beta = 0.0f)
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

    mir::AerodynamicProfile Wrap(mir::L2Profile profile)
    {
        mir::AerodynamicProfile wrapped;
        wrapped.l2 = std::move(profile);
        return wrapped;
    }

    float SignedLiftCoefficient(const mir::ForceMoment &result, const mir::AirData &air)
    {
        const glm::vec3 flow = air.relativeVelocityBody / air.trueAirspeed;
        const glm::vec3 liftAxis = glm::normalize(glm::cross(glm::vec3{ 0.0f, 1.0f, 0.0f }, flow));
        const float dynamicPressure = 0.5f * air.density * air.trueAirspeed * air.trueAirspeed;
        return glm::dot(result.sources.lift, liftAxis) / (dynamicPressure * kArea);
    }

    constexpr std::string_view kStall = R"({
        "positiveDeg": 15.0, "negativeDeg": -15.0, "blendDeg": 4.0,
        "controlScale": 0.4, "stabilityScale": 0.3,
        "postStall": [[-15, -0.9, 0.2, -0.15], [0, 0.25, 0.05, 0.05],
                      [15, 1.35, 0.16, -0.1], [19, 1.08, 0.22, -0.12]]
    })";

    constexpr std::string_view kFlaps = R"([[0, 0, 0, 0, 0], [10, 0.25, 0.005, -0.01, -0.5],
        [20, 0.5, 0.012, -0.02, -1], [30, 0.7, 0.02, -0.03, -1]])";

    std::string L2Object(std::string_view stall, std::string_view flaps)
    {
        std::string object = R"({ "L2": {
            "CL0": 0.25, "CLalpha": 5.0, "CD0": 0.03, "k": 0.05,
            "Cm0": 0.05, "Cmalpha": -0.6, "Cmq": -12, "Cmde": 1.2,
            "CYbeta": -0.4, "CYdr": -0.1, "Clbeta": -0.1, "Clp": -0.5, "Clda": 0.08,
            "Cnbeta": 0.12, "Cnr": -0.15, "Cndr": 0.08, "Cnda": -0.02,
            "minAirspeed": 10,
            "stall": )";
        object += stall;
        object += R"(, "flaps": )";
        object += flaps;
        object += "} }";
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
            "aero": )";
        document += aero;
        document += "}";
        return document;
    }
}

TEST_CASE("L2 is a pure function of its input", "[flight][aero]")
{
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L2);
    const mir::AerodynamicProfile profile = Wrap(LightAircraft());
    mir::AeroInput input = L2Input(50.0f, glm::radians(5.0f), glm::radians(2.0f));
    input.angularVelocityBody = glm::vec3{ 0.1f, -0.2f, 0.3f };
    input.elevator = 0.05f;
    input.aileron = -0.04f;
    input.rudder = 0.03f;
    input.flaps = glm::radians(10.0f);

    const mir::ForceMoment first = model.Evaluate(input, profile);
    const mir::ForceMoment second = model.Evaluate(input, profile);

    CHECK(first.force == second.force);
    CHECK(first.moment == second.moment);
    CHECK(first.sources.lift == second.sources.lift);
    CHECK(first.sources.drag == second.sources.drag);
    CHECK(first.sources.side == second.sources.side);
    CHECK(first.sources.controlMoment == second.sources.controlMoment);
    CHECK(first.sources.dampingMoment == second.sources.dampingMoment);
    CHECK(first.sources.stabilityMoment == second.sources.stabilityMoment);
}

TEST_CASE("L2 lift is continuous through the stall", "[flight][aero]")
{
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L2);
    const mir::AerodynamicProfile profile = Wrap(LightAircraft());

    const auto checkRange = [&](float startDegrees, float endDegrees)
    {
        float previous = 0.0f;
        bool first = true;
        for (float degrees = startDegrees; degrees <= endDegrees + 1e-3f; degrees += 0.05f)
        {
            const mir::AeroInput input = L2Input(50.0f, glm::radians(degrees));
            const float cl = SignedLiftCoefficient(model.Evaluate(input, profile), input.air);
            if (!first)
                CHECK(std::abs(cl - previous) <= 0.01f);
            previous = cl;
            first = false;
        }
    };

    checkRange(-23.0f, -13.0f);
    checkRange(13.0f, 23.0f);
}

TEST_CASE("L2 full flaps raise the maximum lift", "[flight][aero]")
{
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L2);
    const mir::AerodynamicProfile profile = Wrap(LightAircraft());
    const float flapLift = 0.7f;

    float maxWithoutFlaps = -10.0f;
    float maxWithFlaps = -10.0f;
    for (float degrees = -25.0f; degrees <= 25.0f; degrees += 0.05f)
    {
        mir::AeroInput input = L2Input(50.0f, glm::radians(degrees));
        maxWithoutFlaps = std::max(maxWithoutFlaps, SignedLiftCoefficient(model.Evaluate(input, profile), input.air));
        input.flaps = glm::radians(30.0f);
        maxWithFlaps = std::max(maxWithFlaps, SignedLiftCoefficient(model.Evaluate(input, profile), input.air));
    }

    CHECK(maxWithFlaps - maxWithoutFlaps >= 0.8f * flapLift);
}

TEST_CASE("L2 static pitch is stable around the trim angle", "[flight][aero]")
{
    const mir::L2Profile l2 = LightAircraft();
    const mir::AerodynamicProfile profile = Wrap(l2);
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L2);
    const float trim = -l2.Cm0 / l2.Cmalpha;

    const mir::ForceMoment above = model.Evaluate(L2Input(50.0f, trim + 0.05f), profile);
    const mir::ForceMoment below = model.Evaluate(L2Input(50.0f, trim - 0.05f), profile);

    CHECK(above.sources.stabilityMoment.y < 0.0f);
    CHECK(below.sources.stabilityMoment.y > 0.0f);
    CHECK(above.moment.y < 0.0f);
    CHECK(below.moment.y > 0.0f);
}

TEST_CASE("L2 sideslip gives a positive yaw moment with Cnbeta > 0", "[flight][aero]")
{
    const mir::AerodynamicProfile profile = Wrap(LightAircraft());
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L2);

    const mir::ForceMoment result = model.Evaluate(L2Input(50.0f, 0.0f, glm::radians(5.0f)), profile);

    CHECK(result.sources.stabilityMoment.z > 0.0f);
    CHECK(result.moment.z > 0.0f);
}

TEST_CASE("L2 positive controls give moments in their positive direction", "[flight][aero]")
{
    const mir::AerodynamicProfile profile = Wrap(LightAircraft());
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L2);
    mir::AeroInput input = L2Input(50.0f, 0.0f);
    input.elevator = glm::radians(5.0f);
    input.aileron = glm::radians(5.0f);
    input.rudder = glm::radians(5.0f);

    const mir::ForceMoment result = model.Evaluate(input, profile);

    CHECK(result.sources.controlMoment.x > 0.0f);
    CHECK(result.sources.controlMoment.y > 0.0f);
    CHECK(result.sources.controlMoment.z > 0.0f);
    CHECK(result.moment.x > 0.0f);
    CHECK(result.moment.y > 0.0f);
    CHECK(result.moment.z > 0.0f);
}

TEST_CASE("L2 aileron gives adverse yaw with Cnda < 0", "[flight][aero]")
{
    const mir::AerodynamicProfile profile = Wrap(LightAircraft());
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L2);
    mir::AeroInput deflected = L2Input(50.0f, 0.0f);
    deflected.aileron = glm::radians(5.0f);
    const mir::AeroInput neutral = L2Input(50.0f, 0.0f);

    const mir::ForceMoment deflectedResult = model.Evaluate(deflected, profile);
    const mir::ForceMoment neutralResult = model.Evaluate(neutral, profile);

    CHECK(deflectedResult.sources.controlMoment.z < neutralResult.sources.controlMoment.z);
}

TEST_CASE("L2 post-stall scales the control and stability moments", "[flight][aero]")
{
    const mir::L2Profile l2 = LightAircraft();
    const mir::AerodynamicProfile profile = Wrap(l2);
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L2);

    const float stalledAlpha = l2.stall.positive + l2.stall.blend;
    CHECK(mir::L2StallFraction(l2, 0.0f, 0.0f) == Catch::Approx(0.0f));
    CHECK(mir::L2StallFraction(l2, stalledAlpha, 0.0f) == Catch::Approx(1.0f));

    mir::AeroInput attached = L2Input(50.0f, 0.0f, glm::radians(5.0f));
    attached.elevator = glm::radians(5.0f);
    attached.aileron = glm::radians(5.0f);
    attached.rudder = glm::radians(5.0f);
    mir::AeroInput stalled = attached;
    stalled.air = AirAt(50.0f, stalledAlpha, glm::radians(5.0f));

    const mir::ForceMoment attachedResult = model.Evaluate(attached, profile);
    const mir::ForceMoment stalledResult = model.Evaluate(stalled, profile);

    CHECK(stalledResult.sources.controlMoment.x ==
        Catch::Approx(attachedResult.sources.controlMoment.x * l2.stall.controlScale));
    CHECK(stalledResult.sources.controlMoment.y ==
        Catch::Approx(attachedResult.sources.controlMoment.y * l2.stall.controlScale));
    CHECK(stalledResult.sources.controlMoment.z ==
        Catch::Approx(attachedResult.sources.controlMoment.z * l2.stall.controlScale));
    CHECK(stalledResult.sources.stabilityMoment.x ==
        Catch::Approx(attachedResult.sources.stabilityMoment.x * l2.stall.stabilityScale));
    CHECK(stalledResult.sources.stabilityMoment.z ==
        Catch::Approx(attachedResult.sources.stabilityMoment.z * l2.stall.stabilityScale));
}

TEST_CASE("L2 moments stay bounded at a low airspeed", "[flight][aero]")
{
    const mir::L2Profile l2 = LightAircraft();
    const mir::AerodynamicProfile profile = Wrap(l2);
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L2);
    const float speed = 0.5f;

    mir::AeroInput input = L2Input(speed, 0.0f);
    input.angularVelocityBody = glm::vec3{ 3.0f };
    const mir::ForceMoment result = model.Evaluate(input, profile);

    CHECK(std::isfinite(result.moment.x));
    CHECK(std::isfinite(result.moment.y));
    CHECK(std::isfinite(result.moment.z));

    const float dynamicPressure = 0.5f * 1.225f * speed * speed;
    const float rateScale = 3.0f * kSpan / (2.0f * l2.minAirspeed);
    const float bound = dynamicPressure * kArea * kSpan
        * (std::abs(l2.Clp) + std::abs(l2.Cmq) + std::abs(l2.Cnr)) * rateScale;
    CHECK(glm::length(result.moment) <= bound);
}

TEST_CASE("L2 yields nothing at zero airspeed", "[flight][aero]")
{
    const mir::AerodynamicProfile profile = Wrap(LightAircraft());
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L2);
    mir::AeroInput input = L2Input(0.0f, 0.0f);
    input.angularVelocityBody = glm::vec3{ 3.0f };
    input.elevator = glm::radians(5.0f);
    input.aileron = glm::radians(5.0f);
    input.rudder = glm::radians(5.0f);

    const mir::ForceMoment result = model.Evaluate(input, profile);

    CHECK(result.force == glm::vec3{ 0.0f });
    CHECK(result.moment == glm::vec3{ 0.0f });
    CHECK(result.sources.controlMoment == glm::vec3{ 0.0f });
    CHECK(result.sources.dampingMoment == glm::vec3{ 0.0f });
    CHECK(result.sources.stabilityMoment == glm::vec3{ 0.0f });
}

TEST_CASE("An L2 config parses", "[flight][config]")
{
    mir::AircraftConfig config;
    std::string error;
    const std::string document = ConfigWithAero(L2Object(kStall, kFlaps));

    REQUIRE(mir::ParseAircraftConfig(document, config, error));
    REQUIRE(config.aero.l2.has_value());
    const mir::L2Profile &l2 = *config.aero.l2;
    CHECK(l2.CL0 == Catch::Approx(0.25f));
    CHECK(l2.CLalpha == Catch::Approx(5.0f));
    CHECK(l2.CD0 == Catch::Approx(0.03f));
    CHECK(l2.k == Catch::Approx(0.05f));
    CHECK(l2.Cmalpha == Catch::Approx(-0.6f));
    CHECK(l2.Cnbeta == Catch::Approx(0.12f));
    CHECK(l2.Cnda == Catch::Approx(-0.02f));
    CHECK(l2.minAirspeed == Catch::Approx(10.0f));
    CHECK(l2.stall.positive == Catch::Approx(glm::radians(15.0f)));
    CHECK(l2.stall.negative == Catch::Approx(glm::radians(-15.0f)));
    CHECK(l2.stall.blend == Catch::Approx(glm::radians(4.0f)));
    CHECK(l2.stall.controlScale == Catch::Approx(0.4f));
    CHECK(l2.stall.stabilityScale == Catch::Approx(0.3f));
    REQUIRE(l2.stall.postStall.size() == 4);
    CHECK(l2.stall.postStall.front().alpha == Catch::Approx(glm::radians(-15.0f)));
    CHECK(l2.stall.postStall.back().alpha == Catch::Approx(glm::radians(19.0f)));
    REQUIRE(l2.flaps.size() == 4);
    CHECK(l2.flaps[0].flap == Catch::Approx(0.0f));
    CHECK(l2.flaps[3].flap == Catch::Approx(glm::radians(30.0f)));
    CHECK(l2.flaps[3].dCL == Catch::Approx(0.7f));
    CHECK(l2.flaps[3].stallShift == Catch::Approx(glm::radians(-1.0f)));

    CHECK(mir::HasAerodynamicProfile(config.aero, mir::AerodynamicTier::L2));
    CHECK_FALSE(mir::HasAerodynamicProfile(config.aero, mir::AerodynamicTier::L1));
}

TEST_CASE("L2 is optional and missing it yields nothing", "[flight][config]")
{
    mir::AircraftConfig config;
    std::string error;
    REQUIRE(mir::ParseAircraftConfig(ConfigWithAero("{}"), config, error));
    CHECK_FALSE(config.aero.l2.has_value());

    const mir::AerodynamicProfile &profile = config.aero;
    const mir::ForceMoment result =
        mir::AerodynamicModelFor(mir::AerodynamicTier::L2).Evaluate(L2Input(50.0f, 0.0f), profile);
    CHECK(result.force == glm::vec3{ 0.0f });
    CHECK(result.moment == glm::vec3{ 0.0f });
}

TEST_CASE("L2 config errors name the JSON path", "[flight][config]")
{
    mir::AircraftConfig config;
    std::string error;

    SECTION("post-stall rows must be strictly increasing in alpha")
    {
        constexpr std::string_view stall = R"({
            "positiveDeg": 15.0, "negativeDeg": -15.0, "blendDeg": 4.0,
            "controlScale": 0.4, "stabilityScale": 0.3,
            "postStall": [[-15, -0.9, 0.2, -0.15], [15, 1.35, 0.16, -0.1],
                          [0, 0.25, 0.05, 0.05], [19, 1.08, 0.22, -0.12]]
        })";
        const std::string document = ConfigWithAero(L2Object(stall, kFlaps));
        REQUIRE_FALSE(mir::ParseAircraftConfig(document, config, error));
        CHECK(error == "aero.L2.stall.postStall: must be strictly increasing in alpha");
    }

    SECTION("a missing stall key names its path")
    {
        constexpr std::string_view stall = R"({
            "positiveDeg": 15.0, "negativeDeg": -15.0,
            "controlScale": 0.4, "stabilityScale": 0.3,
            "postStall": [[-15, -0.9, 0.2, -0.15], [19, 1.08, 0.22, -0.12]]
        })";
        const std::string document = ConfigWithAero(L2Object(stall, kFlaps));
        REQUIRE_FALSE(mir::ParseAircraftConfig(document, config, error));
        CHECK(error == "aero.L2.stall.blendDeg: missing");
    }

    SECTION("a missing post-stall table names its path")
    {
        constexpr std::string_view stall = R"({
            "positiveDeg": 15.0, "negativeDeg": -15.0, "blendDeg": 4.0,
            "controlScale": 0.4, "stabilityScale": 0.3
        })";
        const std::string document = ConfigWithAero(L2Object(stall, kFlaps));
        REQUIRE_FALSE(mir::ParseAircraftConfig(document, config, error));
        CHECK(error == "aero.L2.stall.postStall: missing");
    }

    SECTION("the post-stall table must cover the stall range")
    {
        constexpr std::string_view stall = R"({
            "positiveDeg": 15.0, "negativeDeg": -15.0, "blendDeg": 4.0,
            "controlScale": 0.4, "stabilityScale": 0.3,
            "postStall": [[0, 0.25, 0.05, 0.05], [15, 1.35, 0.16, -0.1]]
        })";
        const std::string document = ConfigWithAero(L2Object(stall, kFlaps));
        REQUIRE_FALSE(mir::ParseAircraftConfig(document, config, error));
        CHECK(error == "aero.L2.stall.postStall: must cover negativeDeg to positiveDeg + blendDeg");
    }
}

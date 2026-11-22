/**
 * @file FlightModelTemplates.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight_editor/FlightModelTemplates.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight_editor/FlightModelTemplates.h>

#include <flight/AerodynamicModel.h>
#include <flight/AircraftConfig.h>
#include <flight/components/Aircraft.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using Catch::Approx;

    mir::AircraftConfig ParseConfig(std::string_view text)
    {
        mir::AircraftConfig config;
        std::string error;
        INFO(error);
        REQUIRE(mir::ParseAircraftConfig(text, config, error));
        return config;
    }

    mir::AircraftConfig LoadCaldera(std::string_view name)
    {
        const std::string path = std::string(MIR_GAMES_DIR) + "/CalderaDemo/assets/aircraft/" + std::string(name);
        std::ifstream file(path);
        REQUIRE(file.good());
        std::stringstream text;
        text << file.rdbuf();
        return ParseConfig(text.str());
    }

    void CheckVector(const glm::vec3 &actual, const glm::vec3 &expected)
    {
        CHECK(actual.x == Approx(expected.x));
        CHECK(actual.y == Approx(expected.y));
        CHECK(actual.z == Approx(expected.z));
    }

    void CheckActuator(const mir::AircraftConfig::Actuator &actual, const mir::AircraftConfig::Actuator &expected)
    {
        CHECK(actual.maxDeflection == Approx(expected.maxDeflection));
        CHECK(actual.rate == Approx(expected.rate));
        CHECK(actual.delay == Approx(expected.delay));
    }

    void CheckCurve(const std::vector<mir::CurvePoint> &actual, const std::vector<mir::CurvePoint> &expected)
    {
        REQUIRE(actual.size() == expected.size());
        for (std::size_t i = 0; i < actual.size(); ++i)
        {
            CHECK(actual[i].x == Approx(expected[i].x));
            CHECK(actual[i].y == Approx(expected[i].y));
        }
    }

    void CheckL2(const mir::L2Profile &actual, const mir::L2Profile &expected)
    {
        CHECK(actual.CL0 == Approx(expected.CL0));
        CHECK(actual.CLalpha == Approx(expected.CLalpha));
        CHECK(actual.CD0 == Approx(expected.CD0));
        CHECK(actual.k == Approx(expected.k));
        CHECK(actual.Cm0 == Approx(expected.Cm0));
        CHECK(actual.Cmalpha == Approx(expected.Cmalpha));
        CHECK(actual.Cmq == Approx(expected.Cmq));
        CHECK(actual.Cmde == Approx(expected.Cmde));
        CHECK(actual.CYbeta == Approx(expected.CYbeta));
        CHECK(actual.CYdr == Approx(expected.CYdr));
        CHECK(actual.Clbeta == Approx(expected.Clbeta));
        CHECK(actual.Clp == Approx(expected.Clp));
        CHECK(actual.Clda == Approx(expected.Clda));
        CHECK(actual.Cnbeta == Approx(expected.Cnbeta));
        CHECK(actual.Cnr == Approx(expected.Cnr));
        CHECK(actual.Cndr == Approx(expected.Cndr));
        CHECK(actual.Cnda == Approx(expected.Cnda));
        CHECK(actual.minAirspeed == Approx(expected.minAirspeed));
        CHECK(actual.stall.positive == Approx(expected.stall.positive));
        CHECK(actual.stall.negative == Approx(expected.stall.negative));
        CHECK(actual.stall.blend == Approx(expected.stall.blend));
        CHECK(actual.stall.controlScale == Approx(expected.stall.controlScale));
        CHECK(actual.stall.stabilityScale == Approx(expected.stall.stabilityScale));

        REQUIRE(actual.stall.postStall.size() == expected.stall.postStall.size());
        for (std::size_t i = 0; i < actual.stall.postStall.size(); ++i)
        {
            CHECK(actual.stall.postStall[i].alpha == Approx(expected.stall.postStall[i].alpha));
            CHECK(actual.stall.postStall[i].CL == Approx(expected.stall.postStall[i].CL));
            CHECK(actual.stall.postStall[i].CD == Approx(expected.stall.postStall[i].CD));
            CHECK(actual.stall.postStall[i].Cm == Approx(expected.stall.postStall[i].Cm));
        }

        REQUIRE(actual.flaps.size() == expected.flaps.size());
        for (std::size_t i = 0; i < actual.flaps.size(); ++i)
        {
            CHECK(actual.flaps[i].flap == Approx(expected.flaps[i].flap));
            CHECK(actual.flaps[i].dCL == Approx(expected.flaps[i].dCL));
            CHECK(actual.flaps[i].dCD == Approx(expected.flaps[i].dCD));
            CHECK(actual.flaps[i].dCm == Approx(expected.flaps[i].dCm));
            CHECK(actual.flaps[i].stallShift == Approx(expected.flaps[i].stallShift));
        }
    }

    // The source with nodes, L1 and L3 removed; every remaining value is unchanged.
    void CheckSameExceptRemoved(const mir::AircraftConfig &actual, const mir::AircraftConfig &source)
    {
        CHECK(actual.version == source.version);
        CHECK(actual.mass == Approx(source.mass));
        CheckVector(actual.inertiaDiagonal, source.inertiaDiagonal);
        CheckVector(actual.inertiaProducts, source.inertiaProducts);
        CheckVector(actual.centerOfMass, source.centerOfMass);
        CHECK(actual.reference.area == Approx(source.reference.area));
        CHECK(actual.reference.span == Approx(source.reference.span));
        CHECK(actual.reference.chord == Approx(source.reference.chord));
        CheckVector(actual.camera.eye, source.camera.eye);
        CHECK(actual.camera.chaseDistance == Approx(source.camera.chaseDistance));
        CHECK(actual.camera.chaseHeight == Approx(source.camera.chaseHeight));
        CHECK(actual.throttleRate == Approx(source.throttleRate));
        CHECK(actual.stallWarningMargin == Approx(source.stallWarningMargin));

        CheckActuator(actual.actuators.elevator, source.actuators.elevator);
        CheckActuator(actual.actuators.aileron, source.actuators.aileron);
        CheckActuator(actual.actuators.rudder, source.actuators.rudder);
        REQUIRE(actual.actuators.flaps.detents.size() == source.actuators.flaps.detents.size());
        for (std::size_t i = 0; i < actual.actuators.flaps.detents.size(); ++i)
            CHECK(actual.actuators.flaps.detents[i] == Approx(source.actuators.flaps.detents[i]));
        CHECK(actual.actuators.flaps.rate == Approx(source.actuators.flaps.rate));
        CHECK(actual.actuators.trim.step == Approx(source.actuators.trim.step));
        CHECK(actual.actuators.trim.max == Approx(source.actuators.trim.max));

        REQUIRE(actual.engines.size() == source.engines.size());
        for (std::size_t i = 0; i < actual.engines.size(); ++i)
        {
            CheckVector(actual.engines[i].position, source.engines[i].position);
            CheckVector(actual.engines[i].direction, source.engines[i].direction);
            CHECK(actual.engines[i].idleRpm == Approx(source.engines[i].idleRpm));
            CHECK(actual.engines[i].maxRpm == Approx(source.engines[i].maxRpm));
            CHECK(actual.engines[i].rpmTau == Approx(source.engines[i].rpmTau));
            CheckCurve(actual.engines[i].staticThrust, source.engines[i].staticThrust);
            CheckCurve(actual.engines[i].speedFactor, source.engines[i].speedFactor);
        }

        REQUIRE(actual.gear.size() == source.gear.size());
        for (std::size_t i = 0; i < actual.gear.size(); ++i)
        {
            CHECK(actual.gear[i].name == source.gear[i].name);
            CheckVector(actual.gear[i].mount, source.gear[i].mount);
            CHECK(actual.gear[i].restLength == Approx(source.gear[i].restLength));
            CHECK(actual.gear[i].stiffness == Approx(source.gear[i].stiffness));
            CHECK(actual.gear[i].damping == Approx(source.gear[i].damping));
            CHECK(actual.gear[i].rollingFriction == Approx(source.gear[i].rollingFriction));
            CHECK(actual.gear[i].sideFriction == Approx(source.gear[i].sideFriction));
            CHECK(actual.gear[i].brake == Approx(source.gear[i].brake));
            CHECK(actual.gear[i].maxSteer == Approx(source.gear[i].maxSteer));
        }

        CHECK(actual.nodes.empty());
        CHECK_FALSE(actual.aero.l1.has_value());
        CHECK_FALSE(actual.aero.l3.has_value());
        REQUIRE(actual.aero.l2.has_value());
        REQUIRE(source.aero.l2.has_value());
        CheckL2(*actual.aero.l2, *source.aero.l2);
    }

    mir::AirData AirAt(float speed, float alpha)
    {
        mir::AirData air;
        air.relativeVelocityBody = speed * glm::vec3{ std::cos(alpha), 0.0f, std::sin(alpha) };
        air.trueAirspeed = speed;
        air.angleOfAttack = alpha;
        air.sideslip = 0.0f;
        air.density = 1.225f;
        air.dynamicPressure = 0.5f * air.density * speed * speed;
        return air;
    }
} // namespace

TEST_CASE("Both flight model templates parse", "[flight_editor][templates]")
{
    for (const mir::FlightModelTemplate kind : { mir::FlightModelTemplate::Powered, mir::FlightModelTemplate::Glider })
    {
        INFO(mir::FlightModelTemplateName(kind));
        const mir::AircraftConfig config = ParseConfig(mir::FlightModelTemplateJson(kind));
        REQUIRE(config.aero.l2.has_value());
        CHECK_FALSE(config.aero.l1.has_value());
        CHECK_FALSE(config.aero.l3.has_value());
        CHECK(config.nodes.empty());
        CHECK(config.gear.size() == 3);
    }
}

TEST_CASE("The glider template has no engines", "[flight_editor][templates]")
{
    const mir::AircraftConfig config = ParseConfig(mir::FlightModelTemplateJson(mir::FlightModelTemplate::Glider));
    CHECK(config.engines.empty());
}

TEST_CASE("The powered template has one engine", "[flight_editor][templates]")
{
    const mir::AircraftConfig config = ParseConfig(mir::FlightModelTemplateJson(mir::FlightModelTemplate::Powered));
    CHECK(config.engines.size() == 1);
}

TEST_CASE("Both flight model templates round-trip through the writer", "[flight_editor][templates]")
{
    for (const mir::FlightModelTemplate kind : { mir::FlightModelTemplate::Powered, mir::FlightModelTemplate::Glider })
    {
        INFO(mir::FlightModelTemplateName(kind));
        const mir::AircraftConfig config = ParseConfig(mir::FlightModelTemplateJson(kind));
        CheckSameExceptRemoved(ParseConfig(mir::SerializeAircraftConfig(config)), config);
    }
}

TEST_CASE("The templates match the CalderaDemo configs apart from L1, L3 and nodes",
    "[flight_editor][templates]")
{
    SECTION("powered")
    {
        CheckSameExceptRemoved(
            ParseConfig(mir::FlightModelTemplateJson(mir::FlightModelTemplate::Powered)),
            LoadCaldera("powered.aircraft.json"));
    }
    SECTION("glider")
    {
        CheckSameExceptRemoved(
            ParseConfig(mir::FlightModelTemplateJson(mir::FlightModelTemplate::Glider)),
            LoadCaldera("glider.aircraft.json"));
    }
}

TEST_CASE("Both templates' L2 profiles produce lift at flying airspeed", "[flight_editor][templates]")
{
    for (const mir::FlightModelTemplate kind : { mir::FlightModelTemplate::Powered, mir::FlightModelTemplate::Glider })
    {
        INFO(mir::FlightModelTemplateName(kind));
        const mir::AircraftConfig config = ParseConfig(mir::FlightModelTemplateJson(kind));
        REQUIRE(config.aero.l2.has_value());

        mir::AeroInput input;
        input.air = AirAt(40.0f, glm::radians(4.0f));
        input.elevatorMax = config.actuators.elevator.maxDeflection;
        input.aileronMax = config.actuators.aileron.maxDeflection;
        input.rudderMax = config.actuators.rudder.maxDeflection;
        input.area = config.reference.area;
        input.span = config.reference.span;
        input.chord = config.reference.chord;

        const mir::ForceMoment result =
            mir::AerodynamicModelFor(mir::AerodynamicTier::L2).Evaluate(input, config.aero);
        CHECK(glm::length(result.sources.lift) > 0.0f);
        CHECK(glm::dot(result.sources.lift, glm::vec3{ 0.0f, 0.0f, -1.0f }) > 0.0f);
    }
}

TEST_CASE("Both templates rest their weight on the main gear", "[flight_editor][templates]")
{
    for (const mir::FlightModelTemplate kind : { mir::FlightModelTemplate::Powered, mir::FlightModelTemplate::Glider })
    {
        INFO(mir::FlightModelTemplateName(kind));
        const mir::AircraftConfig config = ParseConfig(mir::FlightModelTemplateJson(kind));

        float nose = 0.0f;
        float mains = 0.0f;
        for (const mir::AircraftConfig::Gear &gear : config.gear)
        {
            if (gear.maxSteer > 0.0f)
                nose = gear.mount.x;
            else
                mains = gear.mount.x;
        }
        CHECK(nose > config.centerOfMass.x);
        CHECK(mains < config.centerOfMass.x);
    }
}

/**
 * @file FlightDebug.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight_debug/FlightDebugRows: the row text, the tier switch and the visibility rule.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight_debug/FlightDebugRows.h>

#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>
#include <flight/components/Aircraft.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace
{
    const mir::FlightDebugRow &FindRow(const std::vector<mir::FlightDebugRow> &rows, std::string_view label)
    {
        for (const mir::FlightDebugRow &row : rows)
        {
            if (row.label == label)
                return row;
        }
        FAIL("missing row: " << label);
        return rows.front();
    }

    mir::AircraftRuntime TelemetryRuntime()
    {
        mir::AircraftRuntime runtime;
        runtime.telemetry.trueAirspeed = 80.0f;
        runtime.telemetry.heading = glm::radians(90.0f);
        runtime.telemetry.pitch = glm::radians(10.0f);
        runtime.telemetry.roll = glm::radians(-20.0f);
        runtime.telemetry.angleOfAttack = glm::radians(5.0f);
        runtime.telemetry.sideslip = glm::radians(3.0f);
        runtime.telemetry.flaps = glm::radians(20.0f);
        runtime.telemetry.trim = glm::radians(1.5f);
        runtime.telemetry.elevator = glm::radians(-2.0f);
        runtime.telemetry.aileron = glm::radians(4.0f);
        runtime.telemetry.rudder = glm::radians(6.0f);
        runtime.telemetry.stallMargin = glm::radians(4.0f);
        runtime.telemetry.stallWarning = true;
        runtime.telemetry.engineCount = 1;
        runtime.telemetry.rpm[0] = 2400.0f;
        runtime.telemetry.gearCount = 1;
        runtime.telemetry.gearCompression[0] = 0.125f;
        runtime.telemetry.weightOnWheels = true;
        return runtime;
    }
}

TEST_CASE("The telemetry rows convert angles to degrees", "[flight_debug]")
{
    const std::vector<mir::FlightDebugRow> rows = mir::BuildFlightDebugTelemetryRows(TelemetryRuntime());

    CHECK(FindRow(rows, "Heading").value == "90.0 deg");
    CHECK(FindRow(rows, "Pitch").value == "10.0 deg");
    CHECK(FindRow(rows, "Roll").value == "-20.0 deg");
    CHECK(FindRow(rows, "Angle of attack").value == "5.0 deg");
    CHECK(FindRow(rows, "Sideslip").value == "3.0 deg");
    CHECK(FindRow(rows, "Flaps").value == "20.0 deg");
    CHECK(FindRow(rows, "Trim").value == "1.5 deg");
    CHECK(FindRow(rows, "Elevator").value == "-2.0 deg");
    CHECK(FindRow(rows, "Aileron").value == "4.0 deg");
    CHECK(FindRow(rows, "Rudder").value == "6.0 deg");
    CHECK(FindRow(rows, "Stall margin").value == "4.0 deg");
    CHECK(FindRow(rows, "Stall margin").warning);
    CHECK(FindRow(rows, "RPM 1").value == "2400");
    CHECK(FindRow(rows, "Gear compression 1").value == "0.125 m");
    CHECK(FindRow(rows, "Weight on wheels").value == "yes");
}

TEST_CASE("The stall margin row is not highlighted without the warning", "[flight_debug]")
{
    mir::AircraftRuntime runtime = TelemetryRuntime();
    runtime.telemetry.stallWarning = false;

    const std::vector<mir::FlightDebugRow> rows = mir::BuildFlightDebugTelemetryRows(runtime);
    CHECK_FALSE(FindRow(rows, "Stall margin").warning);
    CHECK_FALSE(FindRow(rows, "Pitch").warning);
}

TEST_CASE("The configuration rows summarize the aircraft", "[flight_debug]")
{
    mir::AircraftConfig config;
    config.mass = 1200.0f;
    config.inertiaDiagonal = glm::vec3{ 2000.0f, 3500.0f, 2500.0f };
    config.inertiaProducts = glm::vec3{ 10.0f, 20.0f, 30.0f };
    config.centerOfMass = glm::vec3{ 0.0f, 0.0f, 0.15f };
    config.reference.area = 16.0f;
    config.reference.span = 11.0f;
    config.reference.chord = 1.5f;
    config.actuators.elevator.maxDeflection = glm::radians(25.0f);
    config.actuators.elevator.rate = glm::radians(600.0f);
    config.actuators.flaps.detents = { 0.0f, glm::radians(10.0f), glm::radians(20.0f) };
    config.engines.resize(2);
    config.gear.resize(3);

    const std::vector<mir::FlightDebugRow> rows = mir::BuildFlightDebugConfigRows(config);

    CHECK(FindRow(rows, "Mass").value == "1200.00 kg");
    CHECK(FindRow(rows, "Inertia diagonal").value == "(2000.00, 3500.00, 2500.00) kg m^2");
    CHECK(FindRow(rows, "Inertia products").value == "(10.00, 20.00, 30.00) kg m^2");
    CHECK(FindRow(rows, "Reference area").value == "16.00 m^2");
    CHECK(FindRow(rows, "Reference span").value == "11.00 m");
    CHECK(FindRow(rows, "Reference chord").value == "1.50 m");
    CHECK(FindRow(rows, "Elevator travel").value == "25.0 deg");
    CHECK(FindRow(rows, "Elevator rate").value == "600.0 deg/s");
    CHECK(FindRow(rows, "Flap detents").value == "0.0, 10.0, 20.0 deg");
    CHECK(FindRow(rows, "Engines").value == "2");
    CHECK(FindRow(rows, "Gear points").value == "3");
}

TEST_CASE("The force rows report the body-frame sources", "[flight_debug]")
{
    mir::AircraftRuntime runtime;
    runtime.aero.sources.lift = glm::vec3{ 1.0f, 2.0f, 3.0f };
    runtime.aero.sources.controlMoment = glm::vec3{ -4.0f, 5.0f, 6.0f };
    runtime.propulsion.force = glm::vec3{ 7.0f, 0.0f, 0.0f };
    runtime.gear.moment = glm::vec3{ 0.0f, 0.0f, 8.0f };

    const std::vector<mir::FlightDebugRow> rows = mir::BuildFlightDebugForceRows(runtime);

    CHECK(FindRow(rows, "Aero lift").value == "(1.00, 2.00, 3.00) N");
    CHECK(FindRow(rows, "Aero control moment").value == "(-4.00, 5.00, 6.00) N m");
    CHECK(FindRow(rows, "Propulsion force").value == "(7.00, 0.00, 0.00) N");
    CHECK(FindRow(rows, "Gear moment").value == "(0.00, 0.00, 8.00) N m");
}

TEST_CASE("The tier switch writes the component and reports a missing profile", "[flight_debug]")
{
    mir::AircraftConfig config;
    config.aero.l2 = mir::L2Profile{};
    mir::Aircraft aircraft;
    aircraft.tier = mir::AerodynamicTier::L2;

    CHECK(mir::FlightDebugTierAvailable(config, mir::AerodynamicTier::L2));
    CHECK_FALSE(mir::FlightDebugTierAvailable(config, mir::AerodynamicTier::L1));

    CHECK_FALSE(mir::SetFlightDebugTier(aircraft, &config, mir::AerodynamicTier::L1));
    CHECK(aircraft.tier == mir::AerodynamicTier::L1);
    CHECK(mir::SetFlightDebugTier(aircraft, &config, mir::AerodynamicTier::L2));
    CHECK(aircraft.tier == mir::AerodynamicTier::L2);

    CHECK_FALSE(mir::SetFlightDebugTier(aircraft, nullptr, mir::AerodynamicTier::L3));
    CHECK(aircraft.tier == mir::AerodynamicTier::L3);
}

TEST_CASE("The editor window needs a Play world", "[flight_debug]")
{
    CHECK_FALSE(mir::FlightDebugWindowVisible(true, false, true));
    CHECK(mir::FlightDebugWindowVisible(true, true, true));
    CHECK_FALSE(mir::FlightDebugWindowVisible(true, true, false));
    CHECK(mir::FlightDebugWindowVisible(false, false, true));
    CHECK_FALSE(mir::FlightDebugWindowVisible(false, false, false));
}

TEST_CASE("Degrees are the window's angle unit", "[flight_debug]")
{
    CHECK(mir::FlightDebugDegrees(glm::radians(180.0f)) == Catch::Approx(180.0f));
    CHECK(mir::FlightDebugDegrees(glm::radians(-45.0f)) == Catch::Approx(-45.0f));
}

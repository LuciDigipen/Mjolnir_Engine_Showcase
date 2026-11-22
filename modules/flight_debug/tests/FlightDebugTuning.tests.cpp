/**
 * @file FlightDebugTuning.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight_debug/FlightDebugSaving and the panel's editable field table.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight_debug/FlightDebugRows.h>
#include <flight_debug/FlightDebugSaving.h>

#include <flight/AircraftConfig.h>

#include <core/fs/FileIO.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::string_view kPath = "aircraft/plane.aircraft.json";

    constexpr std::string_view kConfig = R"({
        "version": 1,
        "mass": 1200.0,
        "inertia": { "diagonal": [1000.0, 2000.0, 2500.0], "products": [0.0, 150.0, 0.0] },
        "centerOfMass": [0.5, 0.0, 0.2],
        "reference": { "area": 16.0, "span": 11.0, "chord": 1.5 },
        "camera": { "eye": [0.8, 0.0, -0.4], "chaseDistance": 12.0, "chaseHeight": 3.0 },
        "nodes": { "elevator": "Elevator" },
        "throttleRate": 0.5,
        "stallWarningMarginDeg": 3.0,
        "actuators": {
            "elevator": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.05 },
            "aileron": { "maxDeflectionDeg": 20.0, "rateDegPerSec": 80.0, "delay": 0.05 },
            "rudder": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.05 },
            "flaps": { "detentsDeg": [0.0, 10.0], "rateDegPerSec": 5.0 },
            "trim": { "stepDeg": 0.5, "maxDeg": 10.0 }
        },
        "engines": [
            {
                "position": [0.0, 0.0, -1.5],
                "direction": [0.0, 0.0, -1.0],
                "idleRpm": 600.0,
                "maxRpm": 2500.0,
                "rpmTau": 0.3,
                "staticThrust": [[0.0, 0.0], [1.0, 1000.0]],
                "speedFactor": [[0.0, 1.0]]
            }
        ],
        "gear": [
            {
                "name": "main",
                "mount": [0.0, 1.0, 1.0],
                "restLength": 0.8,
                "stiffness": 30000.0,
                "damping": 1500.0,
                "rollingFriction": 0.02,
                "sideFriction": 0.6,
                "brake": 0.4,
                "maxSteerDeg": 10.0
            }
        ],
        "aero": {
            "L1": { "CL": 0.4, "CD": 0.04, "Ka": 3.0, "Ke": 4.0, "Kr": 2.0,
                    "Kp": 2000.0, "Kq": 4000.0, "Kn": 2000.0 },
            "L2": {
                "CL0": 0.25, "CLalpha": 5.0, "CD0": 0.03, "k": 0.04,
                "Cm0": 0.02, "Cmalpha": -0.5, "Cmq": -10.0, "Cmde": -1.0,
                "CYbeta": -0.5, "CYdr": 0.2, "Clbeta": -0.1, "Clp": -0.5, "Clda": 0.1,
                "Cnbeta": 0.1, "Cnr": -0.1, "Cndr": -0.05, "Cnda": 0.02,
                "minAirspeed": 10.0,
                "stall": {
                    "positiveDeg": 15.0,
                    "negativeDeg": -10.0,
                    "blendDeg": 5.0,
                    "controlScale": 0.3,
                    "stabilityScale": 0.5,
                    "postStall": [
                        [-10.0, -0.5, 0.2, 0.0],
                        [15.0, 1.0, 0.1, 0.0],
                        [20.0, 0.8, 0.2, 0.0]
                    ]
                },
                "flaps": [[0.0, 0.0, 0.0, 0.0, 0.0]]
            }
        }
    })";

    mir::AircraftConfig ParseConfig()
    {
        mir::AircraftConfig config;
        std::string error;
        REQUIRE(mir::ParseAircraftConfig(kConfig, config, error));
        return config;
    }

    std::filesystem::path MakeTempRoot(std::string_view name)
    {
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / "mjolnir_flight_debug_tests" / std::string(name);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root, ignored);
        return root;
    }

    const mir::FlightDebugField &FindField(const std::vector<mir::FlightDebugField> &fields, std::string_view group,
        std::string_view label)
    {
        for (const mir::FlightDebugField &field : fields)
        {
            if (field.group == group && field.label == label)
                return field;
        }
        FAIL("missing field: " << group << " / " << label);
        return fields.front();
    }

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
}

TEST_CASE("The save helper writes a file that parses back to the edits", "[flight_debug][tuning]")
{
    const std::filesystem::path root = MakeTempRoot("save");
    const std::filesystem::path target = root / std::filesystem::path(kPath);
    std::filesystem::create_directories(target.parent_path());

    mir::AircraftConfig config = ParseConfig();
    config.mass = 1450.0f;
    config.actuators.elevator.maxDeflection = glm::radians(30.0f);

    std::string error;
    REQUIRE(mir::SaveAircraftConfigToSource(config, root, kPath, error));
    CHECK(error.empty());

    CHECK_FALSE(std::filesystem::exists(target.string() + ".tmp"));

    const std::optional<std::string> written = mir::ReadText(target);
    REQUIRE(written.has_value());

    mir::AircraftConfig reloaded;
    error.clear();
    REQUIRE(mir::ParseAircraftConfig(*written, reloaded, error));
    CHECK(reloaded.mass == Catch::Approx(1450.0f));
    CHECK(reloaded.actuators.elevator.maxDeflection == Catch::Approx(glm::radians(30.0f)).margin(1e-5f));

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("The save helper refuses without a source root", "[flight_debug][tuning]")
{
    const mir::AircraftConfig config = ParseConfig();

    std::string error;
    CHECK_FALSE(mir::SaveAircraftConfigToSource(config, std::filesystem::path{}, kPath, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("The field table covers the editable parameters", "[flight_debug][tuning]")
{
    mir::AircraftConfig config = ParseConfig();
    const std::vector<mir::FlightDebugField> fields = mir::BuildFlightDebugEditableFields(config);
    REQUIRE_FALSE(fields.empty());

    SECTION("every accessor reads back what its setter wrote")
    {
        for (const mir::FlightDebugField &field : fields)
        {
            const float before = field.get(config);
            field.set(config, before + 1.0f);
            CHECK(field.get(config) == Catch::Approx(before + 1.0f).margin(1e-4f));
            field.set(config, before);
        }
    }

    SECTION("the body, camera and actuator scalars are present")
    {
        const mir::FlightDebugField &mass = FindField(fields, "Body", "Mass");
        CHECK(mass.get(config) == Catch::Approx(1200.0f));
        mass.set(config, 1300.0f);
        CHECK(config.mass == Catch::Approx(1300.0f));

        FindField(fields, "Body", "Inertia diagonal X");
        FindField(fields, "Body", "Inertia product Y");
        FindField(fields, "Body", "Centre of mass Z");
        FindField(fields, "Body", "Reference area");
        FindField(fields, "Body", "Reference span");
        FindField(fields, "Body", "Reference chord");
        FindField(fields, "Body", "Throttle rate");
        FindField(fields, "Camera", "Eye Y");
        FindField(fields, "Camera", "Chase distance");
        FindField(fields, "Camera", "Chase height");

        const mir::FlightDebugField &travel = FindField(fields, "Actuators", "Elevator travel");
        CHECK(travel.degrees);
        travel.set(config, glm::radians(30.0f));
        CHECK(config.actuators.elevator.maxDeflection == Catch::Approx(glm::radians(30.0f)).margin(1e-5f));
        FindField(fields, "Actuators", "Aileron rate");
        FindField(fields, "Actuators", "Rudder delay");
        FindField(fields, "Actuators", "Flap rate");
        FindField(fields, "Actuators", "Trim step");
        FindField(fields, "Actuators", "Trim max");

        const mir::FlightDebugField &margin = FindField(fields, "Body", "Stall warning margin");
        CHECK(margin.degrees);
        margin.set(config, glm::radians(4.0f));
        CHECK(config.stallWarningMargin == Catch::Approx(glm::radians(4.0f)).margin(1e-5f));
    }

    SECTION("engine and gear fields are indexed")
    {
        const mir::FlightDebugField &idle = FindField(fields, "Engine 1", "Idle rpm");
        CHECK(idle.get(config) == Catch::Approx(600.0f));
        idle.set(config, 700.0f);
        CHECK(config.engines[0].idleRpm == Catch::Approx(700.0f));
        FindField(fields, "Engine 1", "Max rpm");
        FindField(fields, "Engine 1", "rpm tau");
        FindField(fields, "Engine 1", "Position Z");
        FindField(fields, "Engine 1", "Direction Z");

        FindField(fields, "Gear 1", "Mount Y");
        const mir::FlightDebugField &stiffness = FindField(fields, "Gear 1", "Stiffness");
        stiffness.set(config, 35000.0f);
        CHECK(config.gear[0].stiffness == Catch::Approx(35000.0f));
        FindField(fields, "Gear 1", "Rest length");
        FindField(fields, "Gear 1", "Damping");
        FindField(fields, "Gear 1", "Rolling friction");
        FindField(fields, "Gear 1", "Side friction");
        FindField(fields, "Gear 1", "Brake");
        FindField(fields, "Gear 1", "Max steer");
    }

    SECTION("the aerodynamic coefficients are present")
    {
        for (const std::string_view label : { "CL", "CD", "Ka", "Ke", "Kr", "Kp", "Kq", "Kn" })
            FindField(fields, "Aero L1", label);

        for (const std::string_view label : { "CL0", "CLalpha", "CD0", "k", "Cm0", "Cmalpha", "Cmq", "Cmde",
                 "CYbeta", "CYdr", "Clbeta", "Clp", "Clda", "Cnbeta", "Cnr", "Cndr", "Cnda", "Min airspeed" })
            FindField(fields, "Aero L2", label);

        const mir::FlightDebugField &alpha = FindField(fields, "Aero L2", "CLalpha");
        alpha.set(config, 5.5f);
        CHECK(config.aero.l2->CLalpha == Catch::Approx(5.5f));

        const mir::FlightDebugField &blend = FindField(fields, "Aero L2 stall", "Blend");
        CHECK(blend.degrees);
        blend.set(config, glm::radians(6.0f));
        CHECK(config.aero.l2->stall.blend == Catch::Approx(glm::radians(6.0f)).margin(1e-5f));
        FindField(fields, "Aero L2 stall", "Positive");
        FindField(fields, "Aero L2 stall", "Negative");
        FindField(fields, "Aero L2 stall", "Control scale");
        FindField(fields, "Aero L2 stall", "Stability scale");
    }
}

TEST_CASE("The read-only rows report the curve and table sizes", "[flight_debug][tuning]")
{
    const std::vector<mir::FlightDebugRow> rows = mir::BuildFlightDebugTableRows(ParseConfig());

    CHECK(FindRow(rows, "Flap detents").value == "2 detents");
    CHECK(FindRow(rows, "Engine 1 static thrust").value == "2 points");
    CHECK(FindRow(rows, "Engine 1 speed factor").value == "1 points");
    CHECK(FindRow(rows, "L2 post-stall").value == "3 rows");
    CHECK(FindRow(rows, "L2 flaps").value == "1 rows");
    CHECK(FindRow(rows, "Nodes").value == "1 entries");
}

TEST_CASE("Saving reports the file it wrote once, and nothing when it fails", "[flight_debug][save]")
{
    const std::filesystem::path root = MakeTempRoot("callback");
    const std::filesystem::path target = root / std::filesystem::path(kPath);
    std::filesystem::create_directories(target.parent_path());

    int saved = 0;
    std::filesystem::path savedPath;
    const mir::SavedSourceCallback onSaved = [&](const std::filesystem::path &path)
    {
        ++saved;
        savedPath = path;
    };
    std::string error;
    REQUIRE(mir::SaveAircraftConfigToSource(ParseConfig(), root, kPath, error, onSaved));
    CHECK(saved == 1);
    CHECK(savedPath == std::filesystem::absolute(target));

    saved = 0;
    CHECK_FALSE(mir::SaveAircraftConfigToSource(ParseConfig(), std::filesystem::path{}, kPath, error, onSaved));
    CHECK(saved == 0);

    mir::AircraftConfig invalid = ParseConfig();
    REQUIRE_FALSE(invalid.gear.empty());
    invalid.gear[0].restLength = -0.1f;
    CHECK_FALSE(mir::SaveAircraftConfigToSource(invalid, root, kPath, error, onSaved));
    CHECK(saved == 0);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("Saving refuses a config the parser rejects and keeps the source file", "[flight_debug][save]")
{
    const std::filesystem::path root = MakeTempRoot("invalid");
    std::filesystem::create_directories((root / kPath).parent_path());
    mir::AircraftConfig config = ParseConfig();
    std::string error;
    REQUIRE(mir::SaveAircraftConfigToSource(config, root, kPath, error));
    const auto saved = mir::ReadBytes(root / kPath);
    REQUIRE(saved.has_value());

    REQUIRE_FALSE(config.gear.empty());
    config.gear[0].restLength = -0.1f;
    CHECK_FALSE(mir::SaveAircraftConfigToSource(config, root, kPath, error));
    CHECK(error.find("restLength") != std::string::npos);
    CHECK(mir::ReadBytes(root / kPath) == saved);
}

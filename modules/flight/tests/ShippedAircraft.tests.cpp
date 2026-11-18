/**
 * @file ShippedAircraft.tests.cpp
 * @author Jackey Cen
 * @brief Tests for the CalderaDemo aircraft configurations.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AerodynamicModel.h>
#include <flight/AircraftConfig.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>

namespace
{
    mir::AircraftConfig LoadShipped(const std::string &name)
    {
        std::ifstream file(std::string(MIR_GAMES_DIR) + "/CalderaDemo/assets/aircraft/" + name);
        REQUIRE(file.good());
        std::stringstream text;
        text << file.rdbuf();

        mir::AircraftConfig config;
        std::string error;
        INFO(error);
        REQUIRE(mir::ParseAircraftConfig(text.str(), config, error));
        return config;
    }
}

TEST_CASE("The powered CalderaDemo aircraft parses with every tier", "[flight][shipped]")
{
    const mir::AircraftConfig config = LoadShipped("powered.aircraft.json");

    CHECK(config.engines.size() == 1);
    CHECK(config.gear.size() == 3);
    CHECK(config.aero.l1.has_value());
    CHECK(config.aero.l2.has_value());
    CHECK(config.aero.l3.has_value());
}

TEST_CASE("The CalderaDemo glider parses without an engine", "[flight][shipped]")
{
    const mir::AircraftConfig config = LoadShipped("glider.aircraft.json");

    CHECK(config.engines.empty());
    CHECK(config.gear.size() == 3);
    CHECK(config.aero.l2.has_value());
    CHECK(config.aero.l3.has_value());
}

TEST_CASE("The CalderaDemo aircraft carry their weight on the main gear", "[flight][shipped]")
{
    const mir::AircraftConfig config = LoadShipped("powered.aircraft.json");

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

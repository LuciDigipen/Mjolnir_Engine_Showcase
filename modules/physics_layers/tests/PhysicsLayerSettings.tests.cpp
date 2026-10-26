/**
 * @file PhysicsLayerSettings.tests.cpp
 * @author sumin.park
 * @brief Tests for physics/PhysicsLayerSettings: the "physicsLayers" section of project_settings.json.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics_layers/PhysicsLayerSettings.h>

#include <core/log/SetupError.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    struct SettingsFile
    {
        std::filesystem::path path = std::filesystem::temp_directory_path() / "mir_physics_layers_settings.json";

        SettingsFile() { std::filesystem::remove(path); }
        ~SettingsFile() { std::filesystem::remove(path); }

        void Write(const std::string &text) const
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << text;
        }

        std::string Read() const
        {
            std::ifstream in(path, std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        }
    };
}

TEST_CASE("No file and no section give the default table", "[physics][layers][settings]")
{
    SettingsFile file;
    mir::PhysicsLayerTable table = mir::ReadPhysicsLayers(file.path);
    CHECK(table.names[0] == "Default");
    CHECK(table.Collide(0, 0));
    CHECK(table.Collide(3, 17));

    file.Write(R"({ "something": 1 })");
    table = mir::ReadPhysicsLayers(file.path);
    CHECK(table.Collide(5, 6));
}

TEST_CASE("Names and ignored pairs are read", "[physics][layers][settings]")
{
    SettingsFile file;
    file.Write(R"({ "physicsLayers": { "names": ["Default", "Player", "", "Enemy"],
                                       "ignore": [["Player", "Player"], ["Enemy", "Player"]] } })");

    const mir::PhysicsLayerTable table = mir::ReadPhysicsLayers(file.path);
    CHECK(table.names[1] == "Player");
    CHECK(table.names[2].empty());
    CHECK(table.IndexOf("Enemy") == 3);
    CHECK_FALSE(table.Collide(1, 1));
    CHECK_FALSE(table.Collide(3, 1));
    CHECK_FALSE(table.Collide(1, 3));
    CHECK(table.Collide(0, 1));
    CHECK(table.Collide(3, 3));
}

TEST_CASE("Default is added when the list does not start with it", "[physics][layers][settings]")
{
    SettingsFile file;
    file.Write(R"({ "physicsLayers": { "names": ["Player"] } })");

    const mir::PhysicsLayerTable table = mir::ReadPhysicsLayers(file.path);
    CHECK(table.names[0] == "Default");
    CHECK(table.names[1] == "Player");
}

TEST_CASE("An invalid section is refused with what is wrong", "[physics][layers][settings]")
{
    SettingsFile file;

    const auto refuses = [&](const std::string &section)
    {
        file.Write(R"({ "physicsLayers": )" + section + " }");
        REQUIRE_THROWS_AS(mir::ReadPhysicsLayers(file.path), mir::SetupError);
    };

    refuses(R"([])");
    refuses(R"({ "names": "Player" })");
    refuses(R"({ "names": ["Default", 5] })");
    refuses(R"({ "names": ["Default", "A", "A"] })");
    refuses(R"({ "names": ["Player", "Default"] })");
    refuses(R"({ "ignore": "all" })");
    refuses(R"({ "ignore": [["Default"]] })");
    refuses(R"({ "names": ["Default"], "ignore": [["Default", "Nobody"]] })");

    // more than 32
    std::string many = R"({ "names": [)";
    for (int i = 0; i < 33; ++i)
        many += (i ? "," : "") + std::string("\"L") + std::to_string(i) + "\"";
    many += "] }";
    refuses(many);
}

TEST_CASE("A table written reads back the same, and the other settings are kept", "[physics][layers][settings]")
{
    SettingsFile file;
    file.Write(R"({ "ide": { "path": "code" } })");

    mir::PhysicsLayerTable table;
    table.names[1] = "Player";
    table.names[3] = "Enemy";
    table.SetCollide(1, 3, false);
    table.SetCollide(3, 3, false);
    REQUIRE(mir::WritePhysicsLayers(file.path, table));

    const mir::PhysicsLayerTable back = mir::ReadPhysicsLayers(file.path);
    CHECK(back.names == table.names);
    CHECK(back.collides == table.collides);
    CHECK(file.Read().find("\"ide\"") != std::string::npos);
}

TEST_CASE("The check finds a renamed Default and a repeated name", "[physics][layers][settings]")
{
    mir::PhysicsLayerTable table;
    CHECK(mir::CheckPhysicsLayers(table).empty());

    table.names[1] = "A";
    table.names[2] = "A";
    CHECK_FALSE(mir::CheckPhysicsLayers(table).empty());

    table.names[2] = "B";
    table.names[0] = "Everything";
    CHECK_FALSE(mir::CheckPhysicsLayers(table).empty());
}

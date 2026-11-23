/**
 * @file FlightModelCreate.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight_editor/FlightModelCreate.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight_editor/FlightModelCreate.h>

#include <flight/AircraftConfig.h>

#include <core/ecs/ComponentFields.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    struct TempRoot
    {
        explicit TempRoot(std::string_view name)
            : path(std::filesystem::temp_directory_path() / "mjolnir_flight_editor_tests" / name)
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
            std::filesystem::create_directories(path, error);
        }

        ~TempRoot()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

    mir::AircraftConfig ParseFile(const std::filesystem::path &path)
    {
        std::ifstream file(path);
        REQUIRE(file.good());
        std::stringstream text;
        text << file.rdbuf();

        mir::AircraftConfig config;
        std::string error;
        INFO(error);
        REQUIRE(mir::ParseAircraftConfig(text.str(), config, error));
        return config;
    }

    std::string ReadFile(const std::filesystem::path &path)
    {
        std::ifstream file(path);
        REQUIRE(file.good());
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }

    mir::FlightModelCreateRequest Request(std::string name, std::optional<std::string> folder)
    {
        return { std::move(name), mir::FlightModelTemplate::Powered, std::move(folder) };
    }
} // namespace

TEST_CASE("The preview names the selected folder", "[flight_editor][create]")
{
    TempRoot root("preview");
    CHECK(mir::PreviewFlightModelCreate(Request("Wing", std::string("models")), root.path).assetsRelativePath ==
          "models/Wing.aircraft.json");
}

TEST_CASE("The preview writes at the assets root when it is selected", "[flight_editor][create]")
{
    TempRoot root("preview-root");
    const mir::FlightModelCreatePreview preview =
        mir::PreviewFlightModelCreate(Request("Wing", std::string()), root.path);
    CHECK(preview.Valid());
    CHECK(preview.assetsRelativePath == "Wing.aircraft.json");
}

TEST_CASE("The preview uses aircraft/ outside the assets root", "[flight_editor][create]")
{
    TempRoot root("preview-outside");
    const mir::FlightModelCreatePreview preview =
        mir::PreviewFlightModelCreate(Request("Wing", std::nullopt), root.path);
    CHECK(preview.Valid());
    CHECK(preview.assetsRelativePath == "aircraft/Wing.aircraft.json");
}

TEST_CASE("An empty name is refused", "[flight_editor][create]")
{
    TempRoot root("empty-name");
    const mir::FlightModelCreatePreview preview =
        mir::PreviewFlightModelCreate(Request("", std::string("aircraft")), root.path);
    CHECK_FALSE(preview.Valid());
    CHECK_FALSE(preview.error.empty());
}

TEST_CASE("A name outside letters, digits, '_' and '-' is refused", "[flight_editor][create]")
{
    TempRoot root("bad-name");
    for (const std::string name : { "Wing Tip", "Wing.Tip", "Wing/Tip", "Wing\\Tip", "Wing:Tip" })
    {
        INFO(name);
        CHECK_FALSE(mir::PreviewFlightModelCreate(Request(name, std::string("aircraft")), root.path).Valid());
    }
}

TEST_CASE("A Windows reserved name is refused", "[flight_editor][create]")
{
    TempRoot root("reserved-name");
    for (const std::string name : { "CON", "con", "Aux", "NUL", "COM1", "lpt9" })
    {
        INFO(name);
        const mir::FlightModelCreatePreview preview =
            mir::PreviewFlightModelCreate(Request(name, std::string("aircraft")), root.path);
        CHECK_FALSE(preview.Valid());
        CHECK(preview.error.find("reserved") != std::string::npos);
    }
}

TEST_CASE("An existing file is found regardless of case", "[flight_editor][create]")
{
    TempRoot root("existing");
    std::string createdPath;
    std::filesystem::path absolutePath;
    std::string error;
    REQUIRE(mir::CreateFlightModelFile(Request("Wing", std::string("aircraft")), root.path, createdPath,
        absolutePath, error));

    const mir::FlightModelCreatePreview preview =
        mir::PreviewFlightModelCreate(Request("wing", std::string("aircraft")), root.path);
    CHECK_FALSE(preview.Valid());
    CHECK(preview.error.find("already exists") != std::string::npos);
}

TEST_CASE("A path longer than the config field holds is refused", "[flight_editor][create]")
{
    TempRoot root("long-name");
    const std::string name(60, 'a');
    const mir::FlightModelCreatePreview preview =
        mir::PreviewFlightModelCreate(Request(name, std::string("aircraft")), root.path);
    CHECK_FALSE(preview.Valid());
    CHECK(preview.assetsRelativePath.size() >= mir::kFieldStringCapacity);
}

TEST_CASE("A folder the asset cook skips is refused", "[flight_editor][create]")
{
    TempRoot root("cook-excluded");
    for (const std::string folder : { "terrain/demo.terrain.layers", "terrain/releases/current", "TERRAIN/Releases/x" })
    {
        INFO(folder);
        const mir::FlightModelCreatePreview preview =
            mir::PreviewFlightModelCreate(Request("Wing", folder), root.path);
        CHECK_FALSE(preview.Valid());
        CHECK(preview.error.find("skips") != std::string::npos);
    }
    CHECK(mir::PreviewFlightModelCreate(Request("Wing", std::string("terrain")), root.path).Valid());
}

TEST_CASE("Creating writes the chosen template and creates its folder", "[flight_editor][create]")
{
    TempRoot root("create");
    mir::FlightModelCreateRequest request = Request("Wing", std::string("models/nested"));
    request.templateKind = mir::FlightModelTemplate::Glider;

    std::string assetsRelativePath;
    std::filesystem::path absolutePath;
    std::string error;
    REQUIRE(mir::CreateFlightModelFile(request, root.path, assetsRelativePath, absolutePath, error));
    CHECK(assetsRelativePath == "models/nested/Wing.aircraft.json");
    CHECK(absolutePath == root.path / "models" / "nested" / "Wing.aircraft.json");
    CHECK(std::filesystem::is_regular_file(absolutePath));

    const mir::AircraftConfig config = ParseFile(absolutePath);
    CHECK(config.engines.empty());
    CHECK_FALSE(config.aero.l1.has_value());
    CHECK_FALSE(config.aero.l3.has_value());
    CHECK(config.nodes.empty());
}

TEST_CASE("Creating at the assets root writes no folder prefix", "[flight_editor][create]")
{
    TempRoot root("create-root");
    std::string assetsRelativePath;
    std::filesystem::path absolutePath;
    std::string error;
    REQUIRE(mir::CreateFlightModelFile(
        Request("Wing", std::string()), root.path, assetsRelativePath, absolutePath, error));
    CHECK(assetsRelativePath == "Wing.aircraft.json");
    CHECK(std::filesystem::is_regular_file(root.path / "Wing.aircraft.json"));
}

TEST_CASE("Creating writes the same pretty form Save writes", "[flight_editor][create]")
{
    TempRoot root("create-pretty");
    std::string assetsRelativePath;
    std::filesystem::path absolutePath;
    std::string error;
    REQUIRE(mir::CreateFlightModelFile(
        Request("Wing", std::string("aircraft")), root.path, assetsRelativePath, absolutePath, error));

    mir::AircraftConfig templateConfig;
    REQUIRE(mir::ParseAircraftConfig(
        mir::FlightModelTemplateJson(mir::FlightModelTemplate::Powered), templateConfig, error));

    const std::string text = ReadFile(absolutePath);
    CHECK(text == mir::SerializeAircraftConfig(templateConfig));
    CHECK(text.find('\n') != std::string::npos);
    CHECK(ParseFile(absolutePath).engines.size() == 1);
}

TEST_CASE("A refused create writes nothing", "[flight_editor][create]")
{
    TempRoot root("refused");
    std::string assetsRelativePath;
    std::filesystem::path absolutePath;
    std::string error;
    CHECK_FALSE(mir::CreateFlightModelFile(
        Request("Bad Name", std::string("aircraft")), root.path, assetsRelativePath, absolutePath, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(std::filesystem::exists(root.path / "aircraft"));
}

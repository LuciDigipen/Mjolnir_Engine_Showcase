/**
 * @file FlightModelDraft.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight_editor/FlightModelDraft.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight_editor/FlightModelDraft.h>
#include <flight_editor/FlightModelTemplates.h>

#include <flight/AircraftConfig.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
    using Catch::Approx;

    constexpr std::string_view kPath = "aircraft/Wing.aircraft.json";

    struct TempRoot
    {
        explicit TempRoot(std::string_view name)
            : path(std::filesystem::temp_directory_path() / "mjolnir_flight_editor_tests" / name)
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
            std::filesystem::create_directories(path / "aircraft", error);
        }

        ~TempRoot()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

    std::string ReadFile(const std::filesystem::path &path)
    {
        std::ifstream file(path);
        REQUIRE(file.good());
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }

    void WriteFile(const std::filesystem::path &path, std::string_view text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();

        const std::filesystem::file_time_type later =
            std::filesystem::last_write_time(path) + std::chrono::seconds(1);
        std::filesystem::last_write_time(path, later);
    }

    std::string TemplateText()
    {
        return std::string(mir::FlightModelTemplateJson(mir::FlightModelTemplate::Powered));
    }

    std::string ConfigText(float mass)
    {
        mir::AircraftConfig config;
        std::string error;
        REQUIRE(mir::ParseAircraftConfig(TemplateText(), config, error));
        config.mass = mass;
        return mir::SerializeAircraftConfig(config);
    }

    mir::AircraftConfig ParseText(std::string_view text)
    {
        mir::AircraftConfig config;
        std::string error;
        INFO(error);
        REQUIRE(mir::ParseAircraftConfig(text, config, error));
        return config;
    }
} // namespace

TEST_CASE("Opening a config reads it as a clean draft", "[flight_editor][draft]")
{
    TempRoot root("draft-open");
    WriteFile(root.path / kPath, TemplateText());

    mir::FlightModelDraft draft;
    std::string error;
    REQUIRE(draft.Open(root.path, kPath, error));
    CHECK(draft.Loaded());
    CHECK_FALSE(draft.Dirty());
    CHECK(draft.Path() == kPath);
    REQUIRE(draft.Config() != nullptr);
    CHECK(draft.Config()->mass == Approx(1100.0f));
}

TEST_CASE("Saving writes the edit and queues the import", "[flight_editor][draft]")
{
    TempRoot root("draft-save");
    WriteFile(root.path / kPath, TemplateText());

    mir::FlightModelDraft draft;
    std::string error;
    REQUIRE(draft.Open(root.path, kPath, error));
    draft.Config()->mass = 1000.0f;
    draft.MarkEdited();
    REQUIRE(draft.Dirty());

    int savedCount = 0;
    std::filesystem::path savedPath;
    const mir::SavedSourceCallback onSaved = [&savedCount, &savedPath](const std::filesystem::path &saved)
    {
        ++savedCount;
        savedPath = saved;
    };

    CHECK(draft.Save(root.path, onSaved, error) == mir::FlightModelSaveResult::Saved);
    CHECK(savedCount == 1);
    CHECK(savedPath == root.path / kPath);
    CHECK_FALSE(draft.Dirty());
    CHECK(ParseText(ReadFile(root.path / kPath)).mass == Approx(1000.0f));
}

TEST_CASE("A failed save keeps the draft and names the field path", "[flight_editor][draft]")
{
    TempRoot root("draft-invalid");
    WriteFile(root.path / kPath, TemplateText());

    mir::FlightModelDraft draft;
    std::string error;
    REQUIRE(draft.Open(root.path, kPath, error));
    const std::string before = ReadFile(root.path / kPath);

    mir::L2Profile &l2 = *draft.Config()->aero.l2;
    REQUIRE(l2.flaps.size() > 1);
    l2.flaps[1].flap = l2.flaps[0].flap;
    draft.MarkEdited();

    bool saved = false;
    const mir::SavedSourceCallback onSaved = [&saved](const std::filesystem::path &) { saved = true; };
    CHECK(draft.Save(root.path, onSaved, error) == mir::FlightModelSaveResult::Invalid);
    CHECK_FALSE(saved);
    CHECK(draft.Dirty());
    CHECK(error.find("aero.L2.flaps") != std::string::npos);
    CHECK(ReadFile(root.path / kPath) == before);
}

TEST_CASE("A file changed on disk is not overwritten without a choice", "[flight_editor][draft]")
{
    TempRoot root("draft-changed");
    WriteFile(root.path / kPath, TemplateText());

    mir::FlightModelDraft draft;
    std::string error;
    REQUIRE(draft.Open(root.path, kPath, error));
    draft.Config()->mass = 1000.0f;
    draft.MarkEdited();

    WriteFile(root.path / kPath, ConfigText(900.0f));
    CHECK(draft.DiskChanged(root.path));

    int savedCount = 0;
    const mir::SavedSourceCallback onSaved = [&savedCount](const std::filesystem::path &) { ++savedCount; };
    CHECK(draft.Save(root.path, onSaved, error) == mir::FlightModelSaveResult::ChangedOnDisk);
    CHECK(savedCount == 0);
    CHECK(draft.Dirty());
    CHECK(ParseText(ReadFile(root.path / kPath)).mass == Approx(900.0f));

    CHECK(draft.Save(root.path, onSaved, error, true) == mir::FlightModelSaveResult::Saved);
    CHECK(savedCount == 1);
    CHECK_FALSE(draft.Dirty());
    CHECK(ParseText(ReadFile(root.path / kPath)).mass == Approx(1000.0f));
}

TEST_CASE("A clean draft reloads an on-disk change on a poll", "[flight_editor][draft]")
{
    TempRoot root("draft-poll");
    WriteFile(root.path / kPath, TemplateText());

    mir::FlightModelDraft draft;
    std::string error;
    REQUIRE(draft.Open(root.path, kPath, error));

    WriteFile(root.path / kPath, ConfigText(800.0f));
    CHECK(draft.Poll(root.path));
    REQUIRE(draft.Config() != nullptr);
    CHECK(draft.Config()->mass == Approx(800.0f));
    CHECK_FALSE(draft.Dirty());
    CHECK_FALSE(draft.Poll(root.path));
}

TEST_CASE("A dirty draft keeps its edits across a poll", "[flight_editor][draft]")
{
    TempRoot root("draft-poll-dirty");
    WriteFile(root.path / kPath, TemplateText());

    mir::FlightModelDraft draft;
    std::string error;
    REQUIRE(draft.Open(root.path, kPath, error));
    draft.Config()->mass = 1000.0f;
    draft.MarkEdited();

    WriteFile(root.path / kPath, ConfigText(800.0f));
    CHECK_FALSE(draft.Poll(root.path));
    CHECK(draft.DiskChanged(root.path));
    CHECK(draft.Config()->mass == Approx(1000.0f));
    CHECK(draft.Dirty());
}

TEST_CASE("Revert drops the edits", "[flight_editor][draft]")
{
    TempRoot root("draft-revert");
    WriteFile(root.path / kPath, TemplateText());

    mir::FlightModelDraft draft;
    std::string error;
    REQUIRE(draft.Open(root.path, kPath, error));
    draft.Config()->mass = 1000.0f;
    draft.MarkEdited();

    CHECK(draft.Revert(root.path, error));
    CHECK_FALSE(draft.Dirty());
    REQUIRE(draft.Config() != nullptr);
    CHECK(draft.Config()->mass == Approx(1100.0f));
}

TEST_CASE("A damaged file reports the parser message and reloads once fixed", "[flight_editor][draft]")
{
    TempRoot root("draft-damaged");
    WriteFile(root.path / kPath, "{ not json");

    mir::FlightModelDraft draft;
    std::string error;
    CHECK_FALSE(draft.Open(root.path, kPath, error));
    CHECK_FALSE(draft.Loaded());
    CHECK_FALSE(draft.ParseError().empty());
    CHECK(draft.Path() == kPath);

    WriteFile(root.path / kPath, TemplateText());
    CHECK(draft.Poll(root.path));
    CHECK(draft.Loaded());
}

TEST_CASE("A dirty draft asks only before a different file replaces it", "[flight_editor][draft]")
{
    CHECK(mir::EvaluateFlightModelOpen(true, "a.aircraft.json", "a.aircraft.json") ==
          mir::FlightModelOpenAction::Open);
    CHECK(mir::EvaluateFlightModelOpen(true, "a.aircraft.json", "b.aircraft.json") ==
          mir::FlightModelOpenAction::Ask);
    CHECK(mir::EvaluateFlightModelOpen(false, "a.aircraft.json", "b.aircraft.json") ==
          mir::FlightModelOpenAction::Open);
}

/**
 * @file RenderSettings.tests.cpp
 * @author Rahul Nair
 * @brief Tests for renderer/RenderSettings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/RenderSettings.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace
{
    struct TempSettingsFile
    {
        std::filesystem::path mPath;

        explicit TempSettingsFile(std::string_view name)
            : mPath(std::filesystem::temp_directory_path() / "mir_render_settings_tests" / name)
        {
            std::filesystem::create_directories(mPath.parent_path());
            std::filesystem::remove(mPath);
        }

        void Write(std::string_view contents) const
        {
            std::ofstream out(mPath);
            out << contents;
        }

        std::string Read() const
        {
            std::ifstream in(mPath);
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

        ~TempSettingsFile() { std::filesystem::remove(mPath); }
    };
}

TEST_CASE("LoadFile on a missing path returns nullopt", "[renderer]")
{
    CHECK_FALSE(mir::RenderSettings::LoadFile("no/such/project_settings.json").has_value());
}

TEST_CASE("LoadFile on a malformed file returns nullopt", "[renderer]")
{
    TempSettingsFile file{ "malformed.json" };
    file.Write(R"({"rendering": broken)");

    CHECK_FALSE(mir::RenderSettings::LoadFile(file.mPath).has_value());
}

// An existing file with no "rendering" section still loads - defaults, not nullopt.
TEST_CASE("LoadFile on a legacy unsectioned input-only file returns defaults", "[renderer]")
{
    TempSettingsFile file{ "legacy_unsectioned.json" };
    file.Write(R"({"actions": []})");

    const std::optional<mir::RenderSettings> settings = mir::RenderSettings::LoadFile(file.mPath);
    REQUIRE(settings.has_value());
    CHECK_FALSE(settings->rayTracing);
}

// value() hands back the raw node, so a scalar "rendering" used to throw
// type_error 306 out of App::Initialize and terminate the process.
TEST_CASE("LoadFile survives a rendering key that is not an object", "[renderer]")
{
    TempSettingsFile file{ "rendering_not_object.json" };
    file.Write(R"({"rendering": null})");

    const std::optional<mir::RenderSettings> settings = mir::RenderSettings::LoadFile(file.mPath);
    REQUIRE(settings.has_value());
    CHECK_FALSE(settings->rayTracing);
}

TEST_CASE("LoadFile survives a scalar rendering key", "[renderer]")
{
    TempSettingsFile file{ "rendering_scalar.json" };
    file.Write(R"({"rendering": 3})");

    const std::optional<mir::RenderSettings> settings = mir::RenderSettings::LoadFile(file.mPath);
    REQUIRE(settings.has_value());
    CHECK_FALSE(settings->rayTracing);
}

TEST_CASE("SaveFile then LoadFile round-trips rayTracing", "[renderer]")
{
    TempSettingsFile file{ "roundtrip.json" };

    mir::RenderSettings saved;
    saved.rayTracing = true;
    REQUIRE(saved.SaveFile(file.mPath));

    const std::optional<mir::RenderSettings> loaded = mir::RenderSettings::LoadFile(file.mPath);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->rayTracing);
}

TEST_CASE("SaveFile preserves a sibling 'input' section instead of clobbering it", "[renderer]")
{
    TempSettingsFile file{ "sibling_preserved.json" };
    file.Write(R"({"input": {"actions": [{"name": "ZZZ_MARKER_ACTION", "type": "Button", "bindings": []}]}})");

    mir::RenderSettings saved;
    saved.rayTracing = true;
    REQUIRE(saved.SaveFile(file.mPath));

    const std::string contents = file.Read();
    REQUIRE(contents.find("ZZZ_MARKER_ACTION") != std::string::npos);
    REQUIRE(contents.find("\"rayTracing\": true") != std::string::npos);
}

TEST_CASE("Legacy rendering settings default to disabled GI", "[renderer][ddgi]")
{
    TempSettingsFile file{ "gi_legacy.json" };
    file.Write(R"({"rendering":{"rayTracing":true}})");
    const auto loaded = mir::RenderSettings::LoadFile(file.mPath);
    REQUIRE(loaded);
    CHECK(loaded->rayTracing);
    CHECK(loaded->ddgi == mir::DdgiSettings{});
    CHECK(loaded->ddgi.ValidationError() == nullptr);
}

TEST_CASE("GI settings round trip independently of ray tracing availability", "[renderer][ddgi]")
{
    TempSettingsFile file{ "gi_roundtrip.json" };
    mir::RenderSettings settings;
    settings.ddgi.enabled = true;
    settings.ddgi.origin = { 1000000000000.125, -2000000000000.5, 42.25 };
    settings.ddgi.probeCounts = { 4, 6, 8 };
    settings.ddgi.probeSpacing = 3.5f;
    settings.ddgi.raysPerProbe = 256;
    settings.ddgi.probesPerFrame = 64;
    settings.ddgi.intensity = 1.75f;
    REQUIRE(settings.SaveFile(file.mPath));
    const auto loaded = mir::RenderSettings::LoadFile(file.mPath);
    REQUIRE(loaded);
    CHECK(*loaded == settings);
}

TEST_CASE("Malformed GI data safely defaults without losing ray tracing", "[renderer][ddgi]")
{
    TempSettingsFile file{ "gi_invalid.json" };
    const auto checkInvalid = [&](const nlohmann::json &gi)
        {
            file.Write(nlohmann::json{ { "rendering", { { "rayTracing", true }, { "ddgi", gi } } } }.dump());
            const auto loaded = mir::RenderSettings::LoadFile(file.mPath);
            REQUIRE(loaded);
            CHECK(loaded->rayTracing);
            CHECK(loaded->ddgi == mir::DdgiSettings{});
        };
    checkInvalid(nullptr);
    checkInvalid(12);
    checkInvalid({ { "enabled", "true" } });
    checkInvalid({ { "enabled", true }, { "origin", { 0, 1 } } });
    checkInvalid({ { "enabled", true }, { "origin", { 0, "bad", 2 } } });
    checkInvalid({ { "enabled", true }, { "probeCounts", { 8, 8, 1e30 } } });
    checkInvalid({ { "enabled", true }, { "probeCounts", { 8, 8, 8.5 } } });
    checkInvalid({ { "enabled", true }, { "probeSpacing", -1 } });
    checkInvalid({ { "enabled", true }, { "raysPerProbe", 65 } });
    checkInvalid({ { "enabled", true }, { "probesPerFrame", 513 } });
    checkInvalid({ { "enabled", true }, { "intensity", nullptr } });
}

TEST_CASE("GI validation bounds allocation and per-frame work", "[renderer][ddgi]")
{
    mir::DdgiSettings gi;
    SECTION("default budget")
    {
        CHECK(gi.ProbeCount() == 512);
        CHECK(gi.EstimatedBytes() <= gi.kMaxEstimatedBytes);
        CHECK(gi.ValidationError() == nullptr);
    }
    SECTION("extreme counts reject before multiplying")
    {
        gi.probeCounts = { std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
            std::numeric_limits<int>::max() };
        CHECK(gi.ValidationError() != nullptr);
    }
    SECTION("total probe cap")
    {
        gi.probeCounts = { 64, 64, 64 };
        CHECK(gi.ValidationError() != nullptr);
    }
    SECTION("memory cap")
    {
        gi.probeCounts = { 32, 32, 32 };
        gi.probesPerFrame = 1;
        CHECK(gi.EstimatedBytes() > gi.kMaxEstimatedBytes);
        CHECK(gi.ValidationError() != nullptr);
    }
    SECTION("ray budget boundary")
    {
        gi.probeCounts = { 16, 16, 16 };
        gi.raysPerProbe = 1024;
        gi.probesPerFrame = 1024;
        CHECK(gi.ValidationError() == nullptr);
        ++gi.probesPerFrame;
        CHECK(gi.ValidationError() != nullptr);
    }
    SECTION("empty update") { gi.probesPerFrame = 0; CHECK(gi.ValidationError() != nullptr); }
    SECTION("nonfinite position") { gi.origin[0] = std::numeric_limits<double>::infinity(); CHECK(gi.ValidationError() != nullptr); }
    SECTION("nonfinite spacing") { gi.probeSpacing = std::numeric_limits<float>::quiet_NaN(); CHECK(gi.ValidationError() != nullptr); }
    SECTION("nonfinite intensity") { gi.intensity = std::numeric_limits<float>::infinity(); CHECK(gi.ValidationError() != nullptr); }
}

TEST_CASE("Invalid GI settings do not overwrite the settings file", "[renderer][ddgi]")
{
    TempSettingsFile file{ "gi_invalid_save.json" };
    file.Write(R"({"input":{"marker":true}})");
    const std::string before = file.Read();
    mir::RenderSettings settings;
    settings.ddgi.probeSpacing = 0;
    CHECK_FALSE(settings.SaveFile(file.mPath));
    CHECK(file.Read() == before);
}

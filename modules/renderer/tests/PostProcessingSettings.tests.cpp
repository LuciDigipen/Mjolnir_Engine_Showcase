/**
 * @file PostProcessingSettings.tests.cpp
 * @author Rahul Nair
 * @brief Tests for renderer/PostProcessingSettings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/PostProcessingSettings.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
    struct TempSettingsFile
    {
        std::filesystem::path mPath;

        explicit TempSettingsFile(std::string_view name)
            : mPath(std::filesystem::temp_directory_path() / "mir_post_processing_settings_tests" / name)
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

TEST_CASE("PostProcessingSettings::LoadFile on a missing path returns nullopt", "[renderer]")
{
    CHECK_FALSE(mir::PostProcessingSettings::LoadFile("no/such/project_settings.json").has_value());
}

TEST_CASE("PostProcessingSettings::LoadFile on a malformed file returns nullopt", "[renderer]")
{
    TempSettingsFile file{ "malformed.json" };
    file.Write(R"({"postProcessing": broken)");

    CHECK_FALSE(mir::PostProcessingSettings::LoadFile(file.mPath).has_value());
}

TEST_CASE("LoadFile on a file with no postProcessing section returns no effects", "[renderer]")
{
    TempSettingsFile file{ "no_section.json" };
    file.Write(R"({"rendering": {"rayTracing": true}})");

    const std::optional<mir::PostProcessingSettings> settings = mir::PostProcessingSettings::LoadFile(file.mPath);
    REQUIRE(settings.has_value());
    CHECK(settings->effects.empty());
}

TEST_CASE("LoadFile survives a postProcessing key that is not an object", "[renderer]")
{
    TempSettingsFile file{ "post_processing_not_object.json" };
    file.Write(R"({"postProcessing": 3})");

    const std::optional<mir::PostProcessingSettings> settings = mir::PostProcessingSettings::LoadFile(file.mPath);
    REQUIRE(settings.has_value());
    CHECK(settings->effects.empty());
}

TEST_CASE("LoadFile survives an effects key that is not an object", "[renderer]")
{
    TempSettingsFile file{ "effects_not_object.json" };
    file.Write(R"({"postProcessing": {"effects": "nope"}})");

    const std::optional<mir::PostProcessingSettings> settings = mir::PostProcessingSettings::LoadFile(file.mPath);
    REQUIRE(settings.has_value());
    CHECK(settings->effects.empty());
}

TEST_CASE("LoadFile skips a non-object effect entry", "[renderer]")
{
    TempSettingsFile file{ "effect_entry_not_object.json" };
    file.Write(R"({"postProcessing": {"effects": {"Bloom": null, "Fog": {"enabled": true}}}})");

    const std::optional<mir::PostProcessingSettings> settings = mir::PostProcessingSettings::LoadFile(file.mPath);
    REQUIRE(settings.has_value());
    CHECK(settings->effects.find("Bloom") == settings->effects.end());
    const auto fogIt = settings->effects.find("Fog");
    REQUIRE(fogIt != settings->effects.end());
    CHECK(fogIt->second.enabled);
}

TEST_CASE("LoadFile falls back to empty params when params is not an object", "[renderer]")
{
    TempSettingsFile file{ "params_not_object.json" };
    file.Write(R"({"postProcessing": {"effects": {"Fog": {"enabled": true, "params": 5}}}})");

    const std::optional<mir::PostProcessingSettings> settings = mir::PostProcessingSettings::LoadFile(file.mPath);
    REQUIRE(settings.has_value());
    const auto fogIt = settings->effects.find("Fog");
    REQUIRE(fogIt != settings->effects.end());
    CHECK(fogIt->second.params.empty());
}

TEST_CASE("SaveFile then LoadFile round-trips an enabled effect with params", "[renderer]")
{
    TempSettingsFile file{ "roundtrip.json" };

    mir::PostProcessingSettings saved;
    mir::PostEffectState &fog = saved.effects["Fog"];
    fog.enabled = true;
    fog.params = nlohmann::json{ { "density", 0.25f } };
    REQUIRE(saved.SaveFile(file.mPath));

    const std::optional<mir::PostProcessingSettings> loaded = mir::PostProcessingSettings::LoadFile(file.mPath);
    REQUIRE(loaded.has_value());
    const auto fogIt = loaded->effects.find("Fog");
    REQUIRE(fogIt != loaded->effects.end());
    CHECK(fogIt->second.enabled);
    CHECK(fogIt->second.params.value("density", 0.0f) == 0.25f);
}

TEST_CASE("SaveFile preserves a sibling 'rendering' section instead of clobbering it", "[renderer]")
{
    TempSettingsFile file{ "sibling_preserved.json" };
    file.Write(R"({"rendering": {"rayTracing": true, "fog": true}})");

    mir::PostProcessingSettings saved;
    saved.effects["Fog"].enabled = true;
    REQUIRE(saved.SaveFile(file.mPath));

    const std::string contents = file.Read();
    REQUIRE(contents.find("\"rayTracing\": true") != std::string::npos);
    REQUIRE(contents.find("\"postProcessing\"") != std::string::npos);
}

TEST_CASE("LoadFile migrates a legacy rendering.fog=true into Fog.enabled", "[renderer]")
{
    TempSettingsFile file{ "legacy_fog_true.json" };
    file.Write(R"({"rendering": {"fog": true, "rayTracing": true}})");

    const std::optional<mir::PostProcessingSettings> settings = mir::PostProcessingSettings::LoadFile(file.mPath);
    REQUIRE(settings.has_value());
    const auto fogIt = settings->effects.find("Fog");
    REQUIRE(fogIt != settings->effects.end());
    CHECK(fogIt->second.enabled);
}

TEST_CASE("LoadFile migrates a legacy rendering.fog=false into Fog.enabled", "[renderer]")
{
    TempSettingsFile file{ "legacy_fog_false.json" };
    file.Write(R"({"rendering": {"fog": false, "rayTracing": true}})");

    const std::optional<mir::PostProcessingSettings> settings = mir::PostProcessingSettings::LoadFile(file.mPath);
    REQUIRE(settings.has_value());
    const auto fogIt = settings->effects.find("Fog");
    REQUIRE(fogIt != settings->effects.end());
    CHECK_FALSE(fogIt->second.enabled);
}

TEST_CASE("LoadFile does not let a stale rendering.fog override an existing Fog entry", "[renderer]")
{
    TempSettingsFile file{ "legacy_fog_ignored.json" };
    file.Write(
        R"({"rendering": {"fog": true}, "postProcessing": {"effects": {"Fog": {"enabled": false}}}})");

    const std::optional<mir::PostProcessingSettings> settings = mir::PostProcessingSettings::LoadFile(file.mPath);
    REQUIRE(settings.has_value());
    const auto fogIt = settings->effects.find("Fog");
    REQUIRE(fogIt != settings->effects.end());
    CHECK_FALSE(fogIt->second.enabled);
}

/**
 * @file AssetSettings.tests.cpp
 * @author sumin.park
 * @brief Tests for assets/AssetSettings: the "assets" section of project_settings.json.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <assets/AssetSettings.h>

#include <core/log/SetupError.h>
#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using mir::AssetSettings;

namespace
{
    // A settings path that is written only when a test gives it text
    struct SettingsFile
    {
        std::filesystem::path path;

        SettingsFile()
        {
            static std::atomic<int> counter{ 0 };
            path = std::filesystem::temp_directory_path() /
                   ("mir_asset_settings_" + std::to_string(mir::CurrentProcessId()) + "_" +
                       std::to_string(counter.fetch_add(1)) + ".json");
        }

        ~SettingsFile()
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }

        void Write(const std::string &text) const { std::ofstream(path, std::ios::binary) << text; }
    };
}

TEST_CASE("Asset settings are the defaults when the file or section is missing", "[assets][settings]")
{
    const SettingsFile file;
    CHECK(AssetSettings::LoadFile(file.path).loadThreads == AssetSettings::kDefaultLoadThreads);

    for (const char *text : { "{}", R"({"assets":{}})", R"({"assets":null})", R"({"assets":3})",
             R"({"assets":{"other":1}})" })
    {
        file.Write(text);
        CHECK(AssetSettings::LoadFile(file.path).loadThreads == AssetSettings::kDefaultLoadThreads);
    }
}

TEST_CASE("assets.loadThreads is read from the project settings", "[assets][settings]")
{
    const SettingsFile file;
    for (const uint32_t threads : { 1u, 4u, AssetSettings::kMaxLoadThreads })
    {
        file.Write(R"({"rendering":{},"assets":{"loadThreads":)" + std::to_string(threads) + "}}");
        CHECK(AssetSettings::LoadFile(file.path).loadThreads == threads);
    }
}

TEST_CASE("An invalid assets.loadThreads keeps the default", "[assets][settings]")
{
    const SettingsFile file;
    for (const char *value : { "0", "-1", "33", "2.5", "\"four\"", "true", "null", "[2]" })
    {
        file.Write(std::string(R"({"assets":{"loadThreads":)") + value + "}}");
        CHECK(AssetSettings::LoadFile(file.path).loadThreads == AssetSettings::kDefaultLoadThreads);
    }
}

TEST_CASE("A project settings file that is not a JSON object fails setup", "[assets][settings]")
{
    const SettingsFile file;
    file.Write("[1, 2]");
    CHECK_THROWS_AS(AssetSettings::LoadFile(file.path), mir::SetupError);
}

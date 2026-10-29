/**
 * @file AudioSettings.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for the per-user audio settings file: defaults, round trips and broken input.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <audio/AudioSettings.h>
#include <audio/AudioSystem.h>

#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace
{
    using mir::AudioSettings;

    // one folder per process: ctest runs each case in its own, possibly in parallel
    struct TempDir
    {
        std::filesystem::path path;

        explicit TempDir(std::string_view name)
            : path(std::filesystem::temp_directory_path() /
                   ("mir_audio_settings_" + std::string(name) + "_" + std::to_string(mir::CurrentProcessId())))
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    void WriteFile(const std::filesystem::path &path, std::string_view text)
    {
        std::ofstream(path, std::ios::binary) << text;
    }

    std::string ReadFile(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    AudioSettings Sample()
    {
        AudioSettings settings;
        settings.mMusic = {0.3f, true};
        settings.mSfx = {0.7f, false};
        return settings;
    }
}

TEST_CASE("a missing audio settings file gives the defaults", "[audio]")
{
    const TempDir dir("missing");

    CHECK(mir::LoadAudioSettings(dir.path / "settings.json") == AudioSettings{});
}

TEST_CASE("a missing audio settings file gives the project's defaults", "[audio]")
{
    const TempDir dir("project_defaults");
    const std::filesystem::path project = dir.path / "project_settings.json";

    WriteFile(project, R"({ "input": { "actions": [] } })");
    CHECK(mir::LoadAudioSettings(project) == AudioSettings{});

    WriteFile(project, R"({ "input": { "actions": [] },
        "audio": { "music": { "volume": 0.3, "muted": true }, "sfx": { "volume": 0.7 } } })");
    const AudioSettings defaults = mir::LoadAudioSettings(project);
    CHECK(defaults == Sample());

    CHECK(mir::LoadAudioSettings(dir.path / "settings.json", defaults) == Sample());
}

TEST_CASE("a bad audio setting falls back to the project's default", "[audio]")
{
    const TempDir dir("bad_value_project_default");
    const std::filesystem::path path = dir.path / "settings.json";

    WriteFile(path, R"({ "audio": { "music": { "volume": "loud" }, "sfx": { "volume": 0.4 } } })");

    const AudioSettings loaded = mir::LoadAudioSettings(path, Sample());
    CHECK(loaded.mMusic == Sample().mMusic);
    CHECK(loaded.mSfx == mir::ChannelSettings{0.4f, false});
}

TEST_CASE("audio settings saved are the audio settings loaded", "[audio]")
{
    const TempDir dir("round_trip");
    const std::filesystem::path path = dir.path / "nested" / "settings.json";

    REQUIRE(mir::SaveAudioSettings(path, Sample()));
    CHECK(mir::LoadAudioSettings(path) == Sample());

    // written as typed, not as the nearest float
    const std::string text = ReadFile(path);
    CHECK(text.find("0.3") != std::string::npos);
    CHECK(text.find("0.300") == std::string::npos);
}

TEST_CASE("an audio settings file that is not JSON gives the defaults", "[audio]")
{
    const TempDir dir("corrupt");
    const std::filesystem::path path = dir.path / "settings.json";

    WriteFile(path, "{ \"audio\": { \"music\": ");
    CHECK(mir::LoadAudioSettings(path) == AudioSettings{});

    WriteFile(path, "[0.5, true]");
    CHECK(mir::LoadAudioSettings(path) == AudioSettings{});
}

TEST_CASE("a bad audio setting falls back to its default and keeps the rest", "[audio]")
{
    const TempDir dir("bad_values");
    const std::filesystem::path path = dir.path / "settings.json";

    WriteFile(path, R"({ "audio": {
        "music": { "volume": 1.5, "muted": "yes" },
        "sfx": { "volume": 0.4, "muted": true } } })");

    const AudioSettings loaded = mir::LoadAudioSettings(path);
    CHECK(loaded.mMusic == mir::ChannelSettings{});
    CHECK(loaded.mSfx == mir::ChannelSettings{0.4f, true});
}

TEST_CASE("saving audio settings keeps the file's other sections and replaces a broken file", "[audio]")
{
    const TempDir dir("other_sections");
    const std::filesystem::path path = dir.path / "settings.json";

    WriteFile(path, R"({ "graphics": { "vsync": false } })");
    REQUIRE(mir::SaveAudioSettings(path, Sample()));
    CHECK(ReadFile(path).find("vsync") != std::string::npos);
    CHECK(mir::LoadAudioSettings(path) == Sample());

    WriteFile(path, "not json");
    REQUIRE(mir::SaveAudioSettings(path, Sample()));
    CHECK(mir::LoadAudioSettings(path) == Sample());
}

TEST_CASE("audio settings applied are the audio settings read back, without a sound engine", "[audio]")
{
    REQUIRE_FALSE(mir::audio::IsInitialized());

    mir::ApplyAudioSettings(Sample());
    CHECK(mir::CurrentAudioSettings() == Sample());
    CHECK_FALSE(mir::audio::IsIdle());

    AudioSettings silent = Sample();
    silent.mSfx.mVolume = 0.0f;
    mir::ApplyAudioSettings(silent);
    CHECK(mir::audio::IsIdle());

    mir::ApplyAudioSettings(AudioSettings{});
    CHECK_FALSE(mir::audio::IsIdle());
}

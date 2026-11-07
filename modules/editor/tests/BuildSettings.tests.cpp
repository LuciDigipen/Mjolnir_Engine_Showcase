/**
 * @file BuildSettings.tests.cpp
 * @author Quy Dong Ta
 * @brief File > Build Settings: where the profile and paths are kept, and the template check.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/BuildSettings.h>

#include <serial/JsonFile.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using namespace mir;

namespace
{
    using json = nlohmann::json;

    struct TempCheckout
    {
        std::filesystem::path root;

        TempCheckout()
        {
            root = std::filesystem::temp_directory_path() /
                ("build-settings-test-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(root / "games" / "Demo");
        }

        ~TempCheckout() { std::filesystem::remove_all(root); }

        BuildGame Game() const
        {
            return BuildGame{ root, root / "games" / "Demo", "Demo Game", root / "games" / "Demo" / "project_settings.json",
                root / "editor" / "game_build_info.json" };
        }

        void Write(const std::filesystem::path &path, const std::string &content) const
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << content;
        }

        json Read(const std::filesystem::path &path) const
        {
            const std::optional<json> read = ReadJsonFile(path);
            REQUIRE(read);
            return *read;
        }
    };

    json Info(bool editor, const std::string &commit, const std::string &config, bool audio)
    {
        return { { "version", 1 }, { "engineVersion", "0.1.0" }, { "commit", commit }, { "config", config },
            { "editor", editor }, { "largeWorld", true }, { "audio", audio }, { "dlss", false }, { "rtxgi", false } };
    }

    bool HasWarning(const GameBuildTemplateCheck &check, std::string_view text)
    {
        for (const std::string &warning : check.mWarnings)
        {
            if (warning.find(text) != std::string::npos)
                return true;
        }
        return false;
    }
}

TEST_CASE("The build profile is kept in project_settings.json's build section", "[build-settings]")
{
    TempCheckout checkout;
    const std::filesystem::path settings = checkout.Game().mProjectSettingsPath;

    CHECK(ReadBuildProfile(settings) == BuildProfile::Release);

    checkout.Write(settings, R"({ "ide": { "path": "C:/Code.exe" }, "rendering": { "rayTracing": true } })");
    REQUIRE(WriteBuildProfile(settings, BuildProfile::Development));
    CHECK(ReadBuildProfile(settings) == BuildProfile::Development);

    // no path ever goes into the checked-in file, and the other sections stay as they were
    const json written = checkout.Read(settings);
    CHECK(written.at("build") == json{ { "profile", "development" } });
    CHECK(written.at("ide") == json{ { "path", "C:/Code.exe" } });
    CHECK(written.at("rendering") == json{ { "rayTracing", true } });

    REQUIRE(WriteBuildProfile(settings, BuildProfile::Release));
    CHECK(checkout.Read(settings)["build"] == json{ { "profile", "release" } });
    CHECK(ReadBuildProfile(settings) == BuildProfile::Release);
}

TEST_CASE("An unknown or malformed build profile reads as Release", "[build-settings]")
{
    TempCheckout checkout;
    const std::filesystem::path settings = checkout.Game().mProjectSettingsPath;

    for (const std::string text : { R"({ "build": { "profile": "fast" } })", R"({ "build": { "profile": 1 } })",
             R"({ "build": "development" })", "not json" })
    {
        INFO(text);
        checkout.Write(settings, text);
        CHECK(ReadBuildProfile(settings) == BuildProfile::Release);
    }

    checkout.Write(settings, R"({ "build": 3 })");
    REQUIRE(WriteBuildProfile(settings, BuildProfile::Development));
    CHECK(checkout.Read(settings)["build"] == json{ { "profile", "development" } });
}

TEST_CASE("Build paths default under the checkout's builds/ and resolve from it", "[build-settings]")
{
    TempCheckout checkout;
    const BuildGame game = checkout.Game();
    EditorPreferences preferences;

    CHECK(GameBuildTemplateDir(preferences, game, BuildProfile::Development) == checkout.root / "builds/game-build/debug");
    CHECK(GameBuildTemplateDir(preferences, game, BuildProfile::Release) == checkout.root / "builds/game-build/release");
    CHECK(BuildOutputDir(preferences, game) == checkout.root / "builds/game-build-out/Demo");
    CHECK(CheckoutPath(BuildOutputDir(preferences, game), checkout.root) == "builds/game-build-out/Demo");

    preferences.buildReleaseTemplate = "templates/release";
    CHECK(GameBuildTemplateDir(preferences, game, BuildProfile::Release) ==
        (checkout.root / "templates/release").lexically_normal());
    CHECK(GameBuildTemplateDir(preferences, game, BuildProfile::Development) == checkout.root / "builds/game-build/debug");

    const std::filesystem::path elsewhere = (checkout.root.parent_path() / "Demo Build").lexically_normal();
    preferences.buildOutputs[BuildOutputKey(game)] = elsewhere.string();
    CHECK(BuildOutputDir(preferences, game) == elsewhere);
    CHECK(CheckoutPath(elsewhere, checkout.root) == elsewhere.generic_string());

    // another game's folder is its own
    BuildGame other = game;
    other.mGameDir = checkout.root / "games" / "Other";
    CHECK(BuildOutputDir(preferences, other) == checkout.root / "builds/game-build-out/Other");
}

TEST_CASE("Build preferences survive a save and a load", "[build-settings]")
{
    TempCheckout checkout;
    const std::filesystem::path path = checkout.root / "prefs" / "editor_preferences.json";

    EditorPreferences saved;
    saved.buildDevelopmentTemplate = "D:/Templates/debug";
    saved.buildReleaseTemplate = "builds/game-build/shipping";
    saved.buildOutputs[BuildOutputKey(checkout.Game())] = "D:/Builds/Demo";
    saved.buildRunAfter = true;
    CHECK(saved.buildCompileGameBuild);
    saved.buildCompileGameBuild = false;
    REQUIRE(saved.SaveFile(path));

    const EditorPreferences loaded = EditorPreferences::LoadFile(path);
    CHECK(loaded.buildDevelopmentTemplate == saved.buildDevelopmentTemplate);
    CHECK(loaded.buildReleaseTemplate == saved.buildReleaseTemplate);
    CHECK(loaded.buildOutputs == saved.buildOutputs);
    CHECK(loaded.buildRunAfter);
    CHECK_FALSE(loaded.buildCompileGameBuild);

    checkout.Write(path, R"({ "build": { "releaseTemplate": 4, "outputs": [ "x" ], "runAfterBuild": "yes",
        "compileGameBuild": "no" } })");
    const EditorPreferences broken = EditorPreferences::LoadFile(path);
    CHECK(broken.buildReleaseTemplate.empty());
    CHECK(broken.buildOutputs.empty());
    CHECK_FALSE(broken.buildRunAfter);
    CHECK(broken.buildCompileGameBuild);
}

TEST_CASE("A template that cannot be used says why", "[build-settings]")
{
    TempCheckout checkout;
    const std::filesystem::path dir = checkout.root / "builds" / "game-build" / "debug";

    CHECK(CheckGameBuildTemplate(dir, BuildProfile::Development, checkout.Game().mEditorInfo).mProblem.find(
              "does not exist") != std::string::npos);

    checkout.Write(dir / "MjolnirEngine.exe", "");
    checkout.Write(dir / "game_build_info.json", Info(true, "1111111111aa", "Debug", true).dump());
    CHECK(CheckGameBuildTemplate(dir, BuildProfile::Development, checkout.Game().mEditorInfo).mProblem.find(
              "editor build") != std::string::npos);
}

TEST_CASE("A template is compared with the editor and the profile", "[build-settings]")
{
    TempCheckout checkout;
    const BuildGame game = checkout.Game();
    const std::filesystem::path dir = checkout.root / "builds" / "game-build" / "release";
    checkout.Write(dir / "MjolnirEngine.exe", "");
    checkout.Write(dir / "game_build_info.json", Info(false, "1111111111aa", "Release", true).dump());

    checkout.Write(game.mEditorInfo, Info(true, "1111111111aa", "Debug", true).dump());
    const GameBuildTemplateCheck same = CheckGameBuildTemplate(dir, BuildProfile::Release, game.mEditorInfo);
    CHECK(same.mProblem.empty());
    CHECK(same.mWarnings.empty());
    CHECK(same.mInfo.at("commit") == "1111111111aa");

    CHECK(HasWarning(CheckGameBuildTemplate(dir, BuildProfile::Development, game.mEditorInfo), "Development profile"));

    checkout.Write(game.mEditorInfo, Info(true, "2222222222bb", "Debug", false).dump());
    const GameBuildTemplateCheck differs = CheckGameBuildTemplate(dir, BuildProfile::Release, game.mEditorInfo);
    CHECK(differs.mProblem.empty());
    CHECK(differs.mWarnings.size() == 2);
    CHECK(HasWarning(differs, "same commit"));
    CHECK(HasWarning(differs, "MIR_WITH_AUDIO ON and the editor with it OFF"));

    std::filesystem::remove(game.mEditorInfo);
    CHECK(HasWarning(CheckGameBuildTemplate(dir, BuildProfile::Release, game.mEditorInfo), "not compared"));
}

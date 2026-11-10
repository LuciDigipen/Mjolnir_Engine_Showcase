/**
 * @file BuildSettings.tests.cpp
 * @author Quy Dong Ta
 * @brief File > Build Settings: where the profile and paths are kept, and the template check.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/BuildSettings.h>
#include <editor/panels/BuildSettingsPanel.h>

#include <serial/JsonFile.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
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

TEST_CASE("A build option choice is kept in project_settings.json, and Reset removes it", "[build-settings]")
{
    TempCheckout checkout;
    const std::filesystem::path settings = checkout.Game().mProjectSettingsPath;
    checkout.Write(settings, R"({ "rendering": { "rayTracing": true }, "build": { "profile": "development" } })");

    REQUIRE(WriteGameBuildChoice(settings, "dlss", false));
    REQUIRE(WriteGameBuildChoice(settings, "audio", true));
    CHECK(checkout.Read(settings) == json::parse(R"({ "rendering": { "rayTracing": true },
        "build": { "profile": "development", "gameBuildOptions": { "dlss": false, "audio": true } } })"));
    CHECK(ReadGameBuildChoices(settings).mOn == std::map<std::string, bool, std::less<>>{ { "audio", true }, { "dlss", false } });

    // the last Reset leaves no empty section
    REQUIRE(WriteGameBuildChoice(settings, "dlss", std::nullopt));
    CHECK(checkout.Read(settings).at("build").at("gameBuildOptions") == json{ { "audio", true } });
    REQUIRE(WriteGameBuildChoice(settings, "audio", std::nullopt));
    CHECK(checkout.Read(settings) ==
        json::parse(R"({ "rendering": { "rayTracing": true }, "build": { "profile": "development" } })"));

    // a Reset adds nothing to a file without choices, and clears a section that is not an object
    checkout.Write(settings, R"({ "rendering": {} })");
    REQUIRE(WriteGameBuildChoice(settings, "rtxgi", std::nullopt));
    CHECK(checkout.Read(settings) == json::parse(R"({ "rendering": {} })"));
    checkout.Write(settings, R"({ "build": { "profile": "release", "gameBuildOptions": [ "dlss" ] } })");
    REQUIRE(WriteGameBuildChoice(settings, "dlss", std::nullopt));
    CHECK(checkout.Read(settings) == json::parse(R"({ "build": { "profile": "release" } })"));

    // a game without the file gets one; a file that is not JSON is left alone
    std::filesystem::remove(settings);
    REQUIRE(WriteGameBuildChoice(settings, "largeWorld", false));
    CHECK(checkout.Read(settings) == json::parse(R"({ "build": { "gameBuildOptions": { "largeWorld": false } } })"));
    checkout.Write(settings, "{ not json");
    CHECK_FALSE(WriteGameBuildChoice(settings, "largeWorld", true));
    std::ifstream in(settings, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) == "{ not json");
}

TEST_CASE("A template is checked against the game's choices, and a chosen difference from the editor is not warned",
    "[build-settings]")
{
    TempCheckout checkout;
    const BuildGame game = checkout.Game();
    const std::filesystem::path dir = checkout.root / "builds" / "game-build" / "release";
    checkout.Write(dir / "MjolnirEngine.exe", "");
    json built = Info(false, "1111111111aa", "Release", true);
    built["dlss"] = true;
    checkout.Write(dir / "game_build_info.json", built.dump());
    checkout.Write(game.mEditorInfo, Info(true, "1111111111aa", "Debug", true).dump());

    CHECK(HasWarning(CheckGameBuildTemplate(dir, BuildProfile::Release, game.mEditorInfo), "the editor with it OFF"));

    GameBuildChoices choices;
    choices.mOn = { { "dlss", true } };
    CHECK(CheckGameBuildTemplate(dir, BuildProfile::Release, game.mEditorInfo, choices).mWarnings.empty());

    choices.mOn = { { "dlss", true }, { "largeWorld", false } };
    const GameBuildTemplateCheck chosen = CheckGameBuildTemplate(dir, BuildProfile::Release, game.mEditorInfo, choices);
    REQUIRE(chosen.mWarnings.size() == 1);
    CHECK(chosen.mWarnings[0] == "the game build template was built with MIR_LARGE_WORLD ON, but the game chooses it OFF");

    // without the editor's, the choices are still compared
    std::filesystem::remove(game.mEditorInfo);
    const GameBuildTemplateCheck alone = CheckGameBuildTemplate(dir, BuildProfile::Release, game.mEditorInfo, choices);
    CHECK(HasWarning(alone, "could not read the editor's"));
    CHECK(HasWarning(alone, "MIR_LARGE_WORLD ON, but the game chooses it OFF"));
}

TEST_CASE("The options a compile changes are those the template has other than the choices and the editor",
    "[build-settings]")
{
    const json editor = Info(true, "1111111111aa", "Debug", true);
    json built = Info(false, "1111111111aa", "Release", true);
    CHECK(ChangedGameBuildOptions(built, {}, editor).empty());

    built["dlss"] = true;
    GameBuildChoices choices;
    choices.mOn = { { "audio", false } };
    CHECK(ChangedGameBuildOptions(built, choices, editor) == std::vector<std::string>{ "Audio", "DLSS" });

    // a template or editor that does not say is no change
    CHECK(ChangedGameBuildOptions(json::object(), choices, editor).empty());
    CHECK(ChangedGameBuildOptions(built, {}, json()).empty());
}

TEST_CASE("File > Build starts what Build Settings would, and Build and Run runs the game", "[build-settings]")
{
    TempCheckout checkout;
    const BuildGame game = checkout.Game();
    EditorPreferences preferences;
    preferences.buildReleaseTemplate = "D:/Templates/release";

    BuildAction action = MakeBuildAction(BuildCommand::Build, preferences, game, BuildProfile::Release, true);
    CHECK(action.mCommand == BuildCommand::Build);
    CHECK(action.mGameBuildDir == GameBuildTemplateDir(preferences, game, BuildProfile::Release));
    CHECK(action.mOutDir == BuildOutputDir(preferences, game));
    CHECK(action.mGameBuildPreset == "windows-msvc-game-release");
    CHECK(action.mRun);

    // with Compile the game build off, the template is used as it is
    preferences.buildCompileGameBuild = false;
    action = MakeBuildAction(BuildCommand::Build, preferences, game, BuildProfile::Development, false);
    CHECK(action.mGameBuildDir == DefaultGameBuildTemplate(checkout.root, BuildProfile::Development));
    CHECK(action.mGameBuildPreset.empty());
    CHECK_FALSE(action.mRun);
}

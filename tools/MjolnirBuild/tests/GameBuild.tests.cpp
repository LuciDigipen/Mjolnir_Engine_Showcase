/**
 * @file GameBuild.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for RunGameBuild, with the real AssetCooker and TerrainTool.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "BuildTestSupport.h"

#include <BuildOutput.h>
#include <GameBuild.h>

#include <assets/AssetManifest.h>
#include <project/GameBuildInfo.h>
#include <project/GameProject.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace mir;
using namespace mir::build;
using namespace mir::build::test;

namespace
{
    BuildRequest Request(const fs::path &gameDir, const fs::path &gameBuildDir, const fs::path &outDir)
    {
        BuildRequest request;
        request.mGameDir = gameDir;
        request.mGameBuildDir = gameBuildDir;
        request.mOutDir = outDir;
        request.mAssetCooker = MJOLNIR_BUILD_ASSET_COOKER;
        request.mTerrainTool = MJOLNIR_BUILD_TERRAIN_TOOL;
        return request;
    }

    const BuildStep *FindStep(const BuildReport &report, std::string_view name)
    {
        const auto it = std::find_if(report.mSteps.begin(), report.mSteps.end(),
            [&](const BuildStep &step) { return step.mName == name; });
        return it == report.mSteps.end() ? nullptr : &*it;
    }

    bool HasWarning(const BuildReport &report, std::string_view text)
    {
        return std::ranges::any_of(report.mWarnings,
            [&](const std::string &warning) { return warning.find(text) != std::string::npos; });
    }

    nlohmann::json ReadJson(const fs::path &file)
    {
        const std::optional<std::string> text = ReadText(file);
        REQUIRE(text);
        return nlohmann::json::parse(*text);
    }

    // A release-like game_build_info.json; commit and the feature flags vary per test
    std::string GameBuildInfo(bool editor, std::string_view commit, bool largeWorld, bool audio)
    {
        return nlohmann::json{ { "version", 1 }, { "engineVersion", "0.1.0" }, { "commit", std::string(commit) },
            { "editor", editor }, { "largeWorld", largeWorld }, { "audio", audio }, { "dlss", false },
            { "rtxgi", false } }
            .dump();
    }

    struct Fixture
    {
        Sandbox mSandbox;
        fs::path mCheckout = mSandbox.mRoot / "checkout";
        fs::path mGameBuildDir = mSandbox.mRoot / "game-build";
        fs::path mOut = mSandbox.mRoot / "out";

        Fixture()
        {
            MakeCheckout(mCheckout);
            MakeGameBuildTemplate(mGameBuildDir);
        }
    };
}

TEST_CASE("A build holds the game build template, the game's scene and data, and its cook", "[game-build]")
{
    Fixture fixture;
    Touch(fixture.mGameBuildDir / "logs" / "engine.log");

    const BuildReport report =
        RunGameBuild(Request(fixture.mCheckout / "games" / "Demo", fixture.mGameBuildDir, fixture.mOut));
    INFO(report.mError);
    REQUIRE(report.mOk);

    const fs::path game = fixture.mOut / "games" / "Demo";
    CHECK(IsBuildDir(fixture.mOut));
    CHECK(report.mExecutable == fixture.mOut / "Demo.exe");
    CHECK(fs::exists(fixture.mOut / "Demo.exe"));
    CHECK(fs::exists(fixture.mOut / "MjolnirErrorReporter.exe"));
    CHECK_FALSE(fs::exists(fixture.mOut / "MjolnirEngine.exe"));
    CHECK(ReadJson(fixture.mOut / "engine.json") == nlohmann::json{ { "defaultGame", "games/Demo" } });
    CHECK(fs::exists(fixture.mOut / "fonts" / "Inter.ttf"));
    CHECK(fs::exists(fixture.mOut / "cooked" / kAssetManifestFileName));
    CHECK(fs::exists(game / "game.json"));
    CHECK(fs::exists(game / "terrain_streaming.json"));
    CHECK(fs::exists(game / "scenes" / "default" / "scene.json"));
    CHECK(fs::exists(game / "assets" / "notes.txt"));
    CHECK(fs::exists(game / "assets" / "audio" / "SoundBanks" / "Init.bnk"));
    CHECK(report.mAssetCount > 0);

    CHECK_FALSE(fs::exists(fixture.mOut / "logs"));
    CHECK_FALSE(fs::exists(game / "terrain_streaming.local.json"));
    CHECK_FALSE(fs::exists(game / "scenes" / "other"));
    CHECK_FALSE(fs::exists(game / "assets" / "prefabs"));
    CHECK_FALSE(fs::exists(StagingDir(fixture.mOut)));
    CHECK(report.mWarnings.size() == 1);
}

TEST_CASE("A build's settings leave out this machine's sections, and the source keeps them", "[game-build]")
{
    Fixture fixture;
    const fs::path source = fixture.mCheckout / "games" / "Demo" / "project_settings.json";
    const std::optional<std::string> before = ReadText(source);

    const BuildReport report =
        RunGameBuild(Request(fixture.mCheckout / "games" / "Demo", fixture.mGameBuildDir, fixture.mOut));
    INFO(report.mError);
    REQUIRE(report.mOk);

    const nlohmann::json settings = ReadJson(fixture.mOut / "games" / "Demo" / "project_settings.json");
    CHECK_FALSE(settings.contains("ide"));
    CHECK(settings.at("rendering").at("rayTracing") == true);
    CHECK(ReadText(source) == before);
}

TEST_CASE("The exe takes game.json's name, and engine.json the game's folder", "[game-build]")
{
    Fixture fixture;
    const fs::path game = fixture.mCheckout / "games" / "Quiet";
    Touch(game / "game.json", R"({ "name": "Quiet Game" })");

    const BuildReport report = RunGameBuild(Request(game, fixture.mGameBuildDir, fixture.mOut));
    INFO(report.mError);
    REQUIRE(report.mOk);

    CHECK(fs::exists(fixture.mOut / "Quiet Game.exe"));
    CHECK(ReadJson(fixture.mOut / "engine.json").at("defaultGame") == "games/Quiet");
    CHECK(nlohmann::json::parse(BuildReportJson(report)).at("executable") == PathText(fixture.mOut / "Quiet Game.exe"));
}

TEST_CASE("A game.json name that cannot name the exe is refused", "[game-build]")
{
    Fixture fixture;
    const fs::path game = fixture.mCheckout / "games" / "Quiet";

    for (const std::string name : { "Bad:Name", "Trailing.", "con", "LPT1.v2", "MjolnirErrorReporter" })
    {
        INFO(name);
        Touch(game / "game.json", nlohmann::json{ { "name", name } }.dump());

        const BuildReport report = RunGameBuild(Request(game, fixture.mGameBuildDir, fixture.mOut));
        CHECK_FALSE(report.mOk);
        CHECK(report.mError.find("game.json's name") != std::string::npos);
        CHECK_FALSE(fs::exists(fixture.mOut));
    }
}

TEST_CASE("A template built differently from the editor is warned about", "[game-build]")
{
    Fixture fixture;
    const fs::path game = fixture.mCheckout / "games" / "Demo";
    const fs::path editorInfo = fixture.mSandbox.mRoot / "editor_info.json";
    Touch(editorInfo, GameBuildInfo(true, "1111111111aaaa", true, true));

    BuildRequest request = Request(game, fixture.mGameBuildDir, fixture.mOut);
    request.mEditorInfo = editorInfo;

    Touch(fixture.mGameBuildDir / "game_build_info.json", GameBuildInfo(false, "1111111111aaaa", true, true));
    const BuildReport same = RunGameBuild(request);
    INFO(same.mError);
    REQUIRE(same.mOk);
    CHECK(same.mWarnings.empty());

    // Demo has SoundBanks, so a template without audio gets the one warning about that
    Touch(fixture.mGameBuildDir / "game_build_info.json", GameBuildInfo(false, "2222222222bbbb", false, false));
    const BuildReport differs = RunGameBuild(request);
    REQUIRE(differs.mOk);
    CHECK(differs.mWarnings.size() == 3);
    CHECK(HasWarning(differs, "commit 2222222222"));
    CHECK(HasWarning(differs, "MIR_LARGE_WORLD"));
    CHECK(HasWarning(differs, "SoundBanks"));
}

TEST_CASE("A game's build options are read from project_settings.json, and a bad entry is named and left out",
    "[game-build]")
{
    using Json = nlohmann::json;
    CHECK(ReadGameBuildChoices(Json::object()).mOn.empty());
    CHECK(ReadGameBuildChoices(Json{ { "build", { { "profile", "release" } } } }).mProblems.empty());
    CHECK(ReadGameBuildChoices(fs::path("C:/no/such/project_settings.json")).mOn.empty());

    const GameBuildChoices choices = ReadGameBuildChoices(Json::parse(
        R"({ "build": { "gameBuildOptions": { "dlss": false, "audio": true, "rtxgi": "yes", "DLSS": true } } })"));
    CHECK(choices.mOn == std::map<std::string, bool, std::less<>>{ { "audio", true }, { "dlss", false } });
    REQUIRE(choices.mProblems.size() == 2);
    CHECK(choices.mProblems[0] == "project_settings.json's build.gameBuildOptions.DLSS is not a build option "
                                  "(largeWorld, audio, dlss, rtxgi), so it is ignored");
    CHECK(choices.mProblems[1] == "project_settings.json's build.gameBuildOptions.rtxgi is not true or false, so it "
                                  "follows the editor");

    const GameBuildChoices broken = ReadGameBuildChoices(Json::parse(R"({ "build": { "gameBuildOptions": [ "dlss" ] } })"));
    CHECK(broken.mOn.empty());
    REQUIRE(broken.mProblems.size() == 1);
    CHECK(broken.mProblems[0].ends_with("is not an object, so every build option follows the editor"));
}

TEST_CASE("A template is compared with the game's choices first, then the editor", "[game-build]")
{
    using Json = nlohmann::json;
    const Json editor = Json::parse(GameBuildInfo(true, "1111111111aaaa", true, true));
    Json built = Json::parse(GameBuildInfo(false, "1111111111aaaa", true, true));
    built["dlss"] = true;
    built["rtxgi"] = true;
    const auto keys = [](const std::vector<GameBuildInfoMismatch> &mismatches) {
        std::vector<std::string> keys;
        for (const GameBuildInfoMismatch &mismatch : mismatches)
            keys.push_back(mismatch.mKey);
        return keys;
    };

    CHECK(keys(CompareGameBuildInfo(built, editor)) == std::vector<std::string>{ "dlss", "rtxgi" });

    // a difference the game chose is not one
    GameBuildChoices choices;
    choices.mOn = { { "dlss", true } };
    CHECK(keys(CompareGameBuildInfo(built, editor, choices)) == std::vector<std::string>{ "rtxgi" });

    // a template that is not what the game chose is, even where it matches the editor
    choices.mOn = { { "dlss", true }, { "largeWorld", false } };
    const std::vector<GameBuildInfoMismatch> chosen = CompareGameBuildInfo(built, editor, choices);
    REQUIRE(keys(chosen) == std::vector<std::string>{ "largeWorld", "rtxgi" });
    CHECK(chosen[0].mMessage == "the game build template was built with MIR_LARGE_WORLD ON, but the game chooses it OFF");

    // without the editor, only the choices are compared, and not the version
    CHECK(keys(CompareGameBuildInfo(built, Json(), choices)) == std::vector<std::string>{ "largeWorld" });
}

TEST_CASE("A ready-made template that is not what the game chose is warned about, and SoundBanks without audio still are",
    "[game-build]")
{
    Fixture fixture;
    const fs::path game = fixture.mCheckout / "games" / "Demo";
    const fs::path editorInfo = fixture.mSandbox.mRoot / "editor_info.json";
    Touch(editorInfo, GameBuildInfo(true, "1111111111aaaa", true, true));
    BuildRequest request = Request(game, fixture.mGameBuildDir, fixture.mOut);
    request.mEditorInfo = editorInfo;

    // chose large world OFF, but the template has it ON
    Touch(fixture.mGameBuildDir / "game_build_info.json", GameBuildInfo(false, "1111111111aaaa", true, true));
    Touch(game / "project_settings.json", R"({ "build": { "gameBuildOptions": { "largeWorld": false } } })");
    const BuildReport differs = RunGameBuild(request);
    INFO(differs.mError);
    REQUIRE(differs.mOk);
    REQUIRE(differs.mWarnings.size() == 1);
    CHECK(differs.mWarnings[0] == "the game build template was built with MIR_LARGE_WORLD ON, but the game chooses it OFF");

    // a template built as chosen is not warned about, though the editor differs
    Touch(fixture.mGameBuildDir / "game_build_info.json", GameBuildInfo(false, "1111111111aaaa", false, true));
    const BuildReport chosen = RunGameBuild(request);
    REQUIRE(chosen.mOk);
    CHECK(chosen.mWarnings.empty());

    // audio OFF by choice still leaves Demo's SoundBanks silent
    Touch(fixture.mGameBuildDir / "game_build_info.json", GameBuildInfo(false, "1111111111aaaa", true, false));
    Touch(game / "project_settings.json", R"({ "build": { "gameBuildOptions": { "audio": false } } })");
    const BuildReport silent = RunGameBuild(request);
    REQUIRE(silent.mOk);
    REQUIRE(silent.mWarnings.size() == 1);
    CHECK(HasWarning(silent, "SoundBanks"));
}

TEST_CASE("A build over a game still running from the output stops in check, and the previous build stays",
    "[game-build]")
{
    Fixture fixture;
    const fs::path game = fixture.mCheckout / "games" / "Demo";
    REQUIRE(RunGameBuild(Request(game, fixture.mGameBuildDir, fixture.mOut)).mOk);

    {
        RunningProgram running(fixture.mOut, "Demo.exe");
        const BuildReport locked = RunGameBuild(Request(game, fixture.mGameBuildDir, fixture.mOut));
        CHECK_FALSE(locked.mOk);
        CHECK(locked.mError.starts_with("Demo.exe is running from " + PathText(fixture.mOut) + " (process "));
        REQUIRE_FALSE(locked.mSteps.empty());
        CHECK(locked.mSteps[0].mName == "check");
        CHECK(locked.mSteps[0].mStatus == StepStatus::Failed);
        const BuildStep *cook = FindStep(locked, "cook");
        CHECK((cook == nullptr || cook->mStatus != StepStatus::Succeeded));
        CHECK(IsBuildDir(fixture.mOut));
        CHECK_FALSE(fs::exists(StagingDir(fixture.mOut)));
    }

    // closed, the same build goes through
    const BuildReport rebuilt = RunGameBuild(Request(game, fixture.mGameBuildDir, fixture.mOut));
    INFO(rebuilt.mError);
    CHECK(rebuilt.mOk);
}

TEST_CASE("An editor game_build_info.json that cannot be read is warned about", "[game-build]")
{
    Fixture fixture;
    BuildRequest request = Request(fixture.mCheckout / "games" / "Quiet", fixture.mGameBuildDir, fixture.mOut);
    request.mEditorInfo = fixture.mSandbox.mRoot / "missing.json";

    const BuildReport report = RunGameBuild(request);
    INFO(report.mError);
    REQUIRE(report.mOk);
    CHECK(HasWarning(report, "was not compared"));
}

TEST_CASE("A game without sound banks or terrain skips those steps", "[game-build]")
{
    Fixture fixture;

    const BuildReport report =
        RunGameBuild(Request(fixture.mCheckout / "games" / "Quiet", fixture.mGameBuildDir, fixture.mOut));
    INFO(report.mError);
    REQUIRE(report.mOk);

    REQUIRE(FindStep(report, "sound banks"));
    REQUIRE(FindStep(report, "terrain"));
    CHECK(FindStep(report, "sound banks")->mStatus == StepStatus::Skipped);
    CHECK(FindStep(report, "terrain")->mStatus == StepStatus::Skipped);
    CHECK(fs::exists(fixture.mOut / "games" / "Quiet" / "scenes" / "default" / "scene.json"));
}

TEST_CASE("A failed cook leaves the previous build in place", "[game-build]")
{
    Fixture fixture;
    const fs::path game = fixture.mCheckout / "games" / "Quiet";
    REQUIRE(RunGameBuild(Request(game, fixture.mGameBuildDir, fixture.mOut)).mOk);

    // TerrainTool rejects AssetCooker's arguments, so the cook exits non-zero
    BuildRequest request = Request(game, fixture.mGameBuildDir, fixture.mOut);
    request.mAssetCooker = request.mTerrainTool;
    Touch(game / "scenes" / "default" / "scene.json", R"({ "changed": true })");
    const BuildReport report = RunGameBuild(request);

    CHECK_FALSE(report.mOk);
    CHECK(report.mError.find("AssetCooker failed") != std::string::npos);
    REQUIRE(FindStep(report, "cook"));
    CHECK(FindStep(report, "cook")->mStatus == StepStatus::Failed);

    CHECK(IsBuildDir(fixture.mOut));
    CHECK(ReadText(fixture.mOut / "games" / "Quiet" / "scenes" / "default" / "scene.json") ==
        std::optional<std::string>(R"({ "entities": [] })"));
    CHECK_FALSE(fs::exists(StagingDir(fixture.mOut)));
}

TEST_CASE("A failed build's report carries its status, steps and error", "[game-build]")
{
    Fixture fixture;
    MakeGameBuildTemplate(fixture.mGameBuildDir, true);

    const BuildReport report =
        RunGameBuild(Request(fixture.mCheckout / "games" / "Demo", fixture.mGameBuildDir, fixture.mOut));
    const nlohmann::json json = nlohmann::json::parse(BuildReportJson(report));

    CHECK(json.at("status") == "failed");
    CHECK(json.at("steps").size() == 1);
    CHECK(json.at("steps").at(0).at("status") == "failed");
    CHECK_FALSE(json.at("error").get<std::string>().empty());
}

TEST_CASE("An editor build is refused as the game build template", "[game-build]")
{
    Fixture fixture;
    MakeGameBuildTemplate(fixture.mGameBuildDir, true);

    const BuildReport report =
        RunGameBuild(Request(fixture.mCheckout / "games" / "Demo", fixture.mGameBuildDir, fixture.mOut));

    CHECK_FALSE(report.mOk);
    CHECK(report.mError.find("editor build") != std::string::npos);
    CHECK_FALSE(fs::exists(fixture.mOut));
    CHECK_FALSE(fs::exists(StagingDir(fixture.mOut)));
}

TEST_CASE("A marked template in <out>.tmp or <out>.old is refused and kept", "[game-build]")
{
    Fixture fixture;
    const fs::path game = fixture.mCheckout / "games" / "Quiet";

    // The marker would let the build delete it, so only the output checks stand in the way
    for (const fs::path &gameBuild : { StagingDir(fixture.mOut), PreviousDir(fixture.mOut) })
    {
        INFO(PathText(gameBuild));
        MakeGameBuildTemplate(gameBuild);
        Touch(gameBuild / kBuildMarkerFileName, "{}");

        const BuildReport report = RunGameBuild(Request(game, gameBuild, fixture.mOut));
        CHECK_FALSE(report.mOk);
        CHECK(report.mError.find("game build template") != std::string::npos);
        CHECK(fs::exists(gameBuild / "MjolnirEngine.exe"));
        CHECK_FALSE(fs::exists(fixture.mOut));
    }
}

TEST_CASE("A game.json scene or SoundBank path that is not relative is refused", "[game-build]")
{
    Fixture fixture;
    const fs::path game = fixture.mCheckout / "games" / "Demo";
    const fs::path scenes = game / "scenes" / "default";

    // Both point inside the game, as an absolute and a root-relative path
    for (const std::string key : { "sceneDir", "soundBanksRoot" })
    {
        for (const std::string value : { scenes.generic_string(), "/" + scenes.relative_path().generic_string() })
        {
            INFO(key << " = " << value);
            Touch(game / "game.json", nlohmann::json{ { "name", "Demo" }, { key, value } }.dump());

            const BuildReport report = RunGameBuild(Request(game, fixture.mGameBuildDir, fixture.mOut));
            CHECK_FALSE(report.mOk);
            CHECK(report.mError.find(key) != std::string::npos);
            CHECK_FALSE(fs::exists(fixture.mOut));
        }
    }
}

TEST_CASE("A one-game cook holds the engine's and that game's assets with the editor's ids", "[game-build]")
{
    Sandbox sandbox;
    const fs::path gameBuild = sandbox.mRoot / "game-build";
    const fs::path out = sandbox.mRoot / "out";
    MakeGameBuildTemplate(gameBuild);

    const BuildReport report =
        RunGameBuild(Request(fs::path(MJOLNIR_SOURCE_DIR) / "games" / "HelloWorld", gameBuild, out));
    INFO(report.mError);
    REQUIRE(report.mOk);

    const std::optional<AssetManifest> ours = AssetManifest::LoadFile(out / "cooked" / kAssetManifestFileName);
    const std::optional<AssetManifest> engine =
        AssetManifest::LoadFile(fs::path(MJOLNIR_ENGINE_COOK_DIR) / kAssetManifestFileName);
    REQUIRE(ours);
    REQUIRE(engine);

    const auto inScope = [](std::string_view logical) {
        return logical.starts_with("assets/") || logical.starts_with("games/HelloWorld/assets/");
    };

    std::size_t named = 0;
    for (const AssetManifestEntry &entry : ours->Entries())
    {
        const std::string_view logical = ours->LogicalPathOf(entry);
        INFO(logical);
        CHECK((logical.empty() || inScope(logical)));
        named += logical.empty() ? 0 : 1;

        const AssetManifestEntry *match = engine->Find(entry.id);
        REQUIRE(match != nullptr);
        CHECK(engine->LogicalPathOf(*match) == logical);
        CHECK(match->typeTag == entry.typeTag);
        CHECK(match->contentVersion == entry.contentVersion);
    }

    const std::size_t expected = static_cast<std::size_t>(std::count_if(engine->Entries().begin(),
        engine->Entries().end(), [&](const AssetManifestEntry &entry) { return inScope(engine->LogicalPathOf(entry)); }));
    CHECK(named == expected);
}

TEST_CASE("Packaged scene IDs survive a source folder rename", "[game-build]")
{
    Fixture fixture;
    const fs::path game = fixture.mCheckout / "games" / "Demo";
    fs::rename(game / "scenes" / "default", game / "scenes" / "renamed");
    Touch(game / "game.json", R"({
        "startupScene": "intro",
        "scenes": [
            { "id": "intro", "path": "scenes/renamed" },
            { "id": "next", "path": "scenes/second" }
        ]
    })");
    Touch(game / "scenes" / "second" / "scene.json", R"({ "entities": [] })");

    const BuildReport report = RunGameBuild(Request(game, fixture.mGameBuildDir, fixture.mOut));
    INFO(report.mError);
    REQUIRE(report.mOk);
    const fs::path packaged = fixture.mOut / "games" / "Demo";
    const GameProject project = LoadGameProject(packaged);
    CHECK(project.mStartupSceneId == "intro");
    CHECK(project.mSceneDir == packaged / "scenes" / "renamed");
    REQUIRE(project.mScenes.size() == 2);
    CHECK(project.mScenes[1].mId == "next");
    CHECK(fs::exists(project.mScenes[1].mDirectory / "scene.json"));
}

TEST_CASE("Invalid scene catalogs fail before packaging", "[game-build]")
{
    Fixture fixture;
    const fs::path game = fixture.mCheckout / "games" / "Demo";
    const auto scene = nlohmann::json{ { "id", "intro" }, { "path", "scenes/default" } };
    for (const auto &scenes : { nlohmann::json::array({ scene, scene }), nlohmann::json::array() })
    {
        Touch(game / "game.json", nlohmann::json{ { "startupScene", "intro" }, { "scenes", scenes } }.dump());
        const BuildReport report = RunGameBuild(Request(game, fixture.mGameBuildDir, fixture.mOut));
        CHECK_FALSE(report.mOk);
        CHECK_FALSE(fs::exists(fixture.mOut));
    }
}

TEST_CASE("game.json's shell section turns the engine's HUD and pause menu off, and a wrong type is refused", "[game-build]")
{
    Fixture fixture;
    const fs::path game = fixture.mCheckout / "games" / "Demo";
    const GameProject defaults = LoadGameProject(game);
    CHECK(defaults.mShellHud);
    CHECK(defaults.mShellMenu);

    Touch(game / "game.json", R"({ "shell": { "menu": false } })");
    const GameProject menuOff = LoadGameProject(game);
    CHECK(menuOff.mShellHud);
    CHECK_FALSE(menuOff.mShellMenu);

    for (const std::string_view manifest : { R"({ "shell": { "hud": "no" } })", R"({ "shell": true })" })
    {
        Touch(game / "game.json", std::string(manifest));
        CHECK_THROWS_AS(LoadGameProject(game), SetupError);
    }
}

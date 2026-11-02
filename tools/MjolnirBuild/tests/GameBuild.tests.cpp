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

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string_view>

using namespace mir;
using namespace mir::build;
using namespace mir::build::test;

namespace
{
    BuildRequest Request(const fs::path &gameDir, const fs::path &playerDir, const fs::path &outDir)
    {
        BuildRequest request;
        request.mGameDir = gameDir;
        request.mPlayerDir = playerDir;
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

    // A release-like player_info.json; commit and the feature flags vary per test
    std::string PlayerInfo(bool editor, std::string_view commit, bool largeWorld, bool audio)
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
        fs::path mPlayer = mSandbox.mRoot / "player";
        fs::path mOut = mSandbox.mRoot / "out";

        Fixture()
        {
            MakeCheckout(mCheckout);
            MakePlayerTemplate(mPlayer);
        }
    };
}

TEST_CASE("A build holds the player template, the game's scene and data, and its cook", "[game-build]")
{
    Fixture fixture;
    Touch(fixture.mPlayer / "logs" / "engine.log");

    const BuildReport report =
        RunGameBuild(Request(fixture.mCheckout / "games" / "Demo", fixture.mPlayer, fixture.mOut));
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
        RunGameBuild(Request(fixture.mCheckout / "games" / "Demo", fixture.mPlayer, fixture.mOut));
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

    const BuildReport report = RunGameBuild(Request(game, fixture.mPlayer, fixture.mOut));
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

        const BuildReport report = RunGameBuild(Request(game, fixture.mPlayer, fixture.mOut));
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
    Touch(editorInfo, PlayerInfo(true, "1111111111aaaa", true, true));

    BuildRequest request = Request(game, fixture.mPlayer, fixture.mOut);
    request.mEditorInfo = editorInfo;

    Touch(fixture.mPlayer / "player_info.json", PlayerInfo(false, "1111111111aaaa", true, true));
    const BuildReport same = RunGameBuild(request);
    INFO(same.mError);
    REQUIRE(same.mOk);
    CHECK(same.mWarnings.empty());

    // Demo has SoundBanks, so a template without audio gets the one warning about that
    Touch(fixture.mPlayer / "player_info.json", PlayerInfo(false, "2222222222bbbb", false, false));
    const BuildReport differs = RunGameBuild(request);
    REQUIRE(differs.mOk);
    CHECK(differs.mWarnings.size() == 3);
    CHECK(HasWarning(differs, "commit 2222222222"));
    CHECK(HasWarning(differs, "MIR_LARGE_WORLD"));
    CHECK(HasWarning(differs, "SoundBanks"));
}

TEST_CASE("An editor player_info.json that cannot be read is warned about", "[game-build]")
{
    Fixture fixture;
    BuildRequest request = Request(fixture.mCheckout / "games" / "Quiet", fixture.mPlayer, fixture.mOut);
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
        RunGameBuild(Request(fixture.mCheckout / "games" / "Quiet", fixture.mPlayer, fixture.mOut));
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
    REQUIRE(RunGameBuild(Request(game, fixture.mPlayer, fixture.mOut)).mOk);

    // TerrainTool rejects AssetCooker's arguments, so the cook exits non-zero
    BuildRequest request = Request(game, fixture.mPlayer, fixture.mOut);
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
    MakePlayerTemplate(fixture.mPlayer, true);

    const BuildReport report =
        RunGameBuild(Request(fixture.mCheckout / "games" / "Demo", fixture.mPlayer, fixture.mOut));
    const nlohmann::json json = nlohmann::json::parse(BuildReportJson(report));

    CHECK(json.at("status") == "failed");
    CHECK(json.at("steps").size() == 1);
    CHECK(json.at("steps").at(0).at("status") == "failed");
    CHECK_FALSE(json.at("error").get<std::string>().empty());
}

TEST_CASE("An editor build is refused as the player template", "[game-build]")
{
    Fixture fixture;
    MakePlayerTemplate(fixture.mPlayer, true);

    const BuildReport report =
        RunGameBuild(Request(fixture.mCheckout / "games" / "Demo", fixture.mPlayer, fixture.mOut));

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
    for (const fs::path &player : { StagingDir(fixture.mOut), PreviousDir(fixture.mOut) })
    {
        INFO(PathText(player));
        MakePlayerTemplate(player);
        Touch(player / kBuildMarkerFileName, "{}");

        const BuildReport report = RunGameBuild(Request(game, player, fixture.mOut));
        CHECK_FALSE(report.mOk);
        CHECK(report.mError.find("player template") != std::string::npos);
        CHECK(fs::exists(player / "MjolnirEngine.exe"));
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

            const BuildReport report = RunGameBuild(Request(game, fixture.mPlayer, fixture.mOut));
            CHECK_FALSE(report.mOk);
            CHECK(report.mError.find(key) != std::string::npos);
            CHECK_FALSE(fs::exists(fixture.mOut));
        }
    }
}

TEST_CASE("A one-game cook holds the engine's and that game's assets with the editor's ids", "[game-build]")
{
    Sandbox sandbox;
    const fs::path player = sandbox.mRoot / "player";
    const fs::path out = sandbox.mRoot / "out";
    MakePlayerTemplate(player);

    const BuildReport report =
        RunGameBuild(Request(fs::path(MJOLNIR_SOURCE_DIR) / "games" / "HelloWorld", player, out));
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

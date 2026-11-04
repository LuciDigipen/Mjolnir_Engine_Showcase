/**
 * @file GameBuildCompile.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for the game build step: its checks and batch file, and whole builds with a test-only cmake.exe.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "BuildTestSupport.h"

#include <BuildOutput.h>
#include <GameBuild.h>
#include <GameBuildCompile.h>

#include <project/GameBuildInfo.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace mir;
using namespace mir::build;
using namespace mir::build::test;

namespace
{
    constexpr std::string_view kPreset = "game-release";

    void WritePresets(const fs::path &root)
    {
        Touch(root / "CMakePresets.json", R"({
  "version": 6,
  "configurePresets": [
    { "name": "base", "hidden": true, "binaryDir": "${sourceDir}/builds/${presetName}" },
    { "name": "game-release", "inherits": "base" },
    { "name": "no-build", "inherits": "base" },
    { "name": "mismatch", "inherits": "base" }
  ],
  "buildPresets": [
    { "name": "game-release", "configurePreset": "game-release" },
    { "name": "mismatch", "configurePreset": "game-release" }
  ]
})");
    }

    bool Has(const std::optional<std::string> &problem, std::string_view text)
    {
        return problem && problem->find(text) != std::string::npos;
    }

    bool Has(const std::string &message, std::string_view text) { return message.find(text) != std::string::npos; }

    const BuildStep *FindStep(const BuildReport &report, std::string_view name)
    {
        const auto it = std::find_if(report.mSteps.begin(), report.mSteps.end(),
            [&](const BuildStep &step) { return step.mName == name; });
        return it == report.mSteps.end() ? nullptr : &*it;
    }

    nlohmann::json ReadJson(const fs::path &file)
    {
        const std::optional<std::string> text = ReadText(file);
        REQUIRE(text);
        return nlohmann::json::parse(*text);
    }

    // Puts the test-only cmake.exe first on PATH, which the game build step's cmd inherits
    struct FakeCMakeOnPath
    {
        std::wstring mPath;

        FakeCMakeOnPath()
        {
            const DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
            mPath.resize(size);
            mPath.resize(GetEnvironmentVariableW(L"PATH", mPath.data(), size));
            const std::wstring fake = fs::path(MJOLNIR_BUILD_FAKE_CMAKE_DIR).make_preferred().wstring();
            REQUIRE(SetEnvironmentVariableW(L"PATH", (fake + L";" + mPath).c_str()));
        }

        ~FakeCMakeOnPath() { SetEnvironmentVariableW(L"PATH", mPath.c_str()); }

        FakeCMakeOnPath(const FakeCMakeOnPath &) = delete;
        FakeCMakeOnPath &operator=(const FakeCMakeOnPath &) = delete;
    };

    struct CompileFixture
    {
        Sandbox mSandbox;
        FakeCMakeOnPath mPath;
        fs::path mCheckout = mSandbox.mRoot / "checkout";
        fs::path mGameBuildDir = mSandbox.mRoot / "game-build";
        fs::path mOut = mSandbox.mRoot / "out";
        fs::path mEditorInfo = mSandbox.mRoot / "editor" / "game_build_info.json";
        nlohmann::json mEditor = { { "version", 1 }, { "engineVersion", "0.1.0" }, { "commit", "fake" },
            { "editor", true }, { "largeWorld", false }, { "audio", true }, { "dlss", false }, { "rtxgi", true } };

        CompileFixture()
        {
            MakeCheckout(mCheckout);
            WritePresets(mCheckout);
            Touch(mEditorInfo, mEditor.dump());
        }

        BuildRequest Request() const
        {
            BuildRequest request;
            request.mGameDir = mCheckout / "games" / "Demo";
            request.mGameBuildDir = mGameBuildDir;
            request.mGameBuildPreset = kPreset;
            request.mOutDir = mOut;
            request.mAssetCooker = MJOLNIR_BUILD_ASSET_COOKER;
            request.mTerrainTool = MJOLNIR_BUILD_TERRAIN_TOOL;
            request.mEditorInfo = mEditorInfo;
            return request;
        }

        fs::path BuildDir() const { return PresetBuildDir(mCheckout, kPreset); }

        void FailAt(std::string_view phase) const { Touch(mCheckout / "fake_cmake_fail.txt", phase); }

        // How many calls fake cmake got that start with prefix
        std::size_t Calls(std::string_view prefix) const
        {
            const std::string text = ReadText(mCheckout / "fake_cmake_calls.txt").value_or(std::string());
            std::size_t count = 0;
            for (std::size_t at = 0; at < text.size();)
            {
                const std::size_t end = std::min(text.find('\n', at), text.size());
                count += std::string_view(text).substr(at, end - at).starts_with(prefix) ? 1 : 0;
                at = end + 1;
            }
            return count;
        }
    };
}

TEST_CASE("A game build preset must be a visible configure preset with a build preset of its own", "[game-build-compile]")
{
    Sandbox sandbox;
    WritePresets(sandbox.mRoot);

    CHECK_FALSE(PresetProblem(sandbox.mRoot, kPreset));
    CHECK(Has(PresetProblem(sandbox.mRoot, ""), "empty"));
    for (std::string_view bad : { "a b", "x&y", "..\\up", "../up", ".hidden" })
    {
        INFO(bad);
        CHECK(Has(PresetProblem(sandbox.mRoot, bad), "may hold only"));
    }
    CHECK(Has(PresetProblem(sandbox.mRoot, "missing"), "define game-release, no-build, mismatch"));
    CHECK(Has(PresetProblem(sandbox.mRoot, "base"), "hidden"));
    CHECK(Has(PresetProblem(sandbox.mRoot, "no-build"), "no build preset"));
    CHECK(Has(PresetProblem(sandbox.mRoot, "mismatch"), "builds the configure preset game-release"));
    CHECK(Has(PresetProblem(sandbox.mRoot / "nowhere", kPreset), "no readable CMakePresets.json"));
}

TEST_CASE("CMakeUserPresets.json and the files it includes add presets", "[game-build-compile]")
{
    Sandbox sandbox;
    WritePresets(sandbox.mRoot);
    Touch(sandbox.mRoot / "CMakeUserPresets.json", R"({ "version": 6, "include": [ "more/presets.json" ],
        "configurePresets": [ { "name": "mine", "inherits": "game-release" } ],
        "buildPresets": [ { "name": "mine", "configurePreset": "mine" } ] })");
    Touch(sandbox.mRoot / "more" / "presets.json", R"({ "version": 6,
        "configurePresets": [ { "name": "included" } ],
        "buildPresets": [ { "name": "included", "configurePreset": "included" } ] })");

    CHECK_FALSE(PresetProblem(sandbox.mRoot, "mine"));
    CHECK_FALSE(PresetProblem(sandbox.mRoot, "included"));
    CHECK_FALSE(PresetProblem(sandbox.mRoot, kPreset));
}

TEST_CASE("Only a missing, empty or game build template folder is replaced", "[game-build-compile]")
{
    Sandbox sandbox;
    const fs::path gameBuild = sandbox.mRoot / "game-build";
    CHECK_FALSE(GameBuildDirProblem(gameBuild));
    fs::create_directories(gameBuild);
    CHECK_FALSE(GameBuildDirProblem(gameBuild));
    MakeGameBuildTemplate(gameBuild);
    CHECK_FALSE(GameBuildDirProblem(gameBuild));

    Touch(sandbox.mRoot / "cmake" / "CMakeCache.txt");
    CHECK(Has(GameBuildDirProblem(sandbox.mRoot / "cmake"), "CMake build folder"));
    Touch(sandbox.mRoot / "game" / kBuildMarkerFileName, "{}");
    CHECK(Has(GameBuildDirProblem(sandbox.mRoot / "game"), "game build"));
    MakeGameBuildTemplate(sandbox.mRoot / "editor", true);
    CHECK(Has(GameBuildDirProblem(sandbox.mRoot / "editor"), "not a game build template"));
    Touch(sandbox.mRoot / "other" / "notes.txt");
    CHECK(Has(GameBuildDirProblem(sandbox.mRoot / "other"), "not a game build template"));
    Touch(sandbox.mRoot / "file");
    CHECK(Has(GameBuildDirProblem(sandbox.mRoot / "file"), "is a file"));
}

TEST_CASE("The template and output stay out of the preset's build folder", "[game-build-compile]")
{
    Sandbox sandbox;
    const fs::path checkout = sandbox.mRoot / "checkout";
    const fs::path out = sandbox.mRoot / "out";
    WritePresets(checkout);
    const fs::path buildDir = PresetBuildDir(checkout, kPreset);

    GameBuildCompile compile{ checkout, std::string(kPreset), sandbox.mRoot / "game-build", {} };
    CHECK_FALSE(CheckGameBuildCompile(compile, out));
    CHECK(Has(CheckGameBuildCompile(compile, buildDir / "out"), "overlaps"));

    for (const fs::path &gameBuild : { buildDir / "game-build", checkout / "builds" })
    {
        INFO(PathText(gameBuild));
        compile.mGameBuildDir = gameBuild;
        CHECK(Has(CheckGameBuildCompile(compile, out), "overlaps"));
    }

    // the editor's own build folder
    compile.mGameBuildDir = sandbox.mRoot / "game-build";
    compile.mEditorInfo = buildDir / "game_build_info.json";
    CHECK(Has(CheckGameBuildCompile(compile, out), "this MjolnirBuild"));
}

TEST_CASE("The build folder this MjolnirBuild runs from is refused", "[game-build-compile]")
{
    const fs::path source = MJOLNIR_SOURCE_DIR;
    const fs::path binaryDir = fs::path(MJOLNIR_ENGINE_COOK_DIR).parent_path();
    const std::string preset = PathText(binaryDir.filename());
    if (PresetProblem(source, preset) || NormalizedPath(PresetBuildDir(source, preset)) != NormalizedPath(binaryDir))
        SKIP("these tests were not built in a preset's folder");

    Sandbox sandbox;
    CHECK(Has(CheckGameBuildCompile({ source, preset, sandbox.mRoot / "game-build", {} }, sandbox.mRoot / "out"),
        "this MjolnirBuild"));
}

TEST_CASE("An install left beside the template is removed only when it is the game build step's", "[game-build-compile]")
{
    Sandbox sandbox;
    const fs::path checkout = sandbox.mRoot / "checkout";
    WritePresets(checkout);
    const GameBuildCompile compile{ checkout, std::string(kPreset), sandbox.mRoot / "game-build", {} };
    const fs::path staging = GameBuildStagingDir(compile.mGameBuildDir);

    Touch(staging / "notes.txt");
    CHECK(Has(CheckGameBuildCompile(compile, sandbox.mRoot / "out"), "installed there"));
    Touch(staging / "mjolnir_game_build_install", "");
    CHECK_FALSE(CheckGameBuildCompile(compile, sandbox.mRoot / "out"));
}

TEST_CASE("The game build is configured with the editor's options", "[game-build-compile]")
{
    const nlohmann::json editor = { { "editor", true }, { "largeWorld", true }, { "audio", false }, { "dlss", false },
        { "rtxgi", true } };
    CHECK(FeatureDefines(editor) == std::vector<std::string>{ "-DMIR_LARGE_WORLD=ON", "-DMIR_WITH_AUDIO=OFF",
                                        "-DMIR_WITH_DLSS=OFF", "-DMIR_WITH_RTXGI=ON" });
    CHECK(FeatureDefines(nlohmann::json{ { "audio", true }, { "dlss", "yes" } }) ==
        std::vector<std::string>{ "-DMIR_WITH_AUDIO=ON" });
}

TEST_CASE("Configure runs again only when it never finished or the cache holds other values", "[game-build-compile]")
{
    Sandbox sandbox;
    const fs::path build = sandbox.mRoot / "build";
    const fs::path cache = build / "CMakeCache.txt";
    const std::vector<std::string> defines = { "-DMIR_WITH_AUDIO=ON", "-DMIR_WITH_DLSS=OFF" };
    CHECK_FALSE(CacheMatches(cache, defines));
    CHECK(NeedsConfigure(build, defines));

    Touch(cache, "// comment\r\nMIR_WITH_DLSS_SDK:PATH=C:/sdk\r\nMIR_WITH_AUDIO:BOOL=TRUE\r\nMIR_WITH_DLSS:BOOL=OFF\r\n");
    CHECK(CacheMatches(cache, defines));
    CHECK(CacheMatches(cache, {}));
    CHECK(NeedsConfigure(build, defines));
    Touch(build / "build.ninja");
    CHECK_FALSE(NeedsConfigure(build, defines));

    CHECK_FALSE(CacheMatches(cache, std::vector<std::string>{ "-DMIR_WITH_DLSS=ON" }));
    CHECK_FALSE(CacheMatches(cache, std::vector<std::string>{ "-DMIR_LARGE_WORLD=ON" }));
    CHECK(NeedsConfigure(build, std::vector<std::string>{ "-DMIR_WITH_DLSS=ON" }));
}

TEST_CASE("The batch file configures when asked, then compiles the game build's two targets and installs", "[game-build-compile]")
{
    GameBuildScript script{ "C:/My Checkout/100%", "game-release", "C:/out/game-build.tmp",
        std::vector<std::string>{ "-DMIR_WITH_AUDIO=ON", "-DMIR_WITH_DLSS=OFF" } };
    const std::string text = GameBuildScriptText(script);

    CHECK(text.starts_with("@echo off\r\n"));
    CHECK(Has(text, "cd /d \"C:\\My Checkout\\100%%\" || exit /b 10\r\n"));
    CHECK(Has(text, "cmake --preset game-release -DMIR_WITH_AUDIO=ON -DMIR_WITH_DLSS=OFF || exit /b 11\r\n"));
    CHECK(Has(text, "cmake --build --preset game-release --target MjolnirEngine MjolnirErrorReporter || exit /b 12\r\n"));
    CHECK(Has(text, "cmake --install \"builds\\game-release\" --prefix \"C:\\out\\game-build.tmp\" || exit /b 13\r\n"));
    CHECK(text.find("cmake --preset") < text.find("cmake --build"));
    CHECK(text.find("cmake --build") < text.find("cmake --install"));

    script.mConfigure.reset();
    CHECK_FALSE(Has(GameBuildScriptText(script), "cmake --preset"));
}

TEST_CASE("Without a game build preset the game build step is skipped and nothing compiles", "[game-build-compile]")
{
    Sandbox sandbox;
    const fs::path checkout = sandbox.mRoot / "checkout";
    MakeCheckout(checkout);
    MakeGameBuildTemplate(sandbox.mRoot / "game-build");

    BuildRequest request;
    request.mGameDir = checkout / "games" / "Quiet";
    request.mGameBuildDir = sandbox.mRoot / "game-build";
    request.mOutDir = sandbox.mRoot / "out";
    request.mAssetCooker = MJOLNIR_BUILD_ASSET_COOKER;
    request.mTerrainTool = MJOLNIR_BUILD_TERRAIN_TOOL;
    const BuildReport report = RunGameBuild(request);
    INFO(report.mError);
    REQUIRE(report.mOk);

    REQUIRE(report.mSteps.size() > 2);
    CHECK(report.mSteps[1].mName == "game build");
    CHECK(report.mSteps[1].mStatus == StepStatus::Skipped);
    CHECK_FALSE(fs::exists(checkout / "builds"));
}

TEST_CASE("The game build step configures once, and installs a fresh template with the editor's options each build",
    "[game-build-compile]")
{
    CompileFixture fixture;
    MakeGameBuildTemplate(fixture.mGameBuildDir);
    Touch(fixture.mGameBuildDir / "shaders" / "generations" / "0000000000000001" / "stale.spv");
    // an install a killed build left
    Touch(GameBuildStagingDir(fixture.mGameBuildDir) / "mjolnir_game_build_install", "");
    Touch(GameBuildStagingDir(fixture.mGameBuildDir) / "MjolnirEngine.exe");

    const BuildReport first = RunGameBuild(fixture.Request());
    INFO(first.mError);
    REQUIRE(first.mOk);
    REQUIRE(first.mSteps.size() > 2);
    CHECK(first.mSteps[1].mName == "game build");
    CHECK(first.mSteps[1].mStatus == StepStatus::Succeeded);
    CHECK(first.mWarnings.empty());
    CHECK(fs::exists(fixture.mOut / "Demo.exe"));
    CHECK_FALSE(fs::exists(fixture.mGameBuildDir / "shaders"));
    CHECK_FALSE(fs::exists(GameBuildStagingDir(fixture.mGameBuildDir)));

    const nlohmann::json info = ReadJson(fixture.mGameBuildDir / "game_build_info.json");
    CHECK(info.at("editor") == false);
    for (const GameBuildFeature &feature : kGameBuildFeatures)
    {
        INFO(feature.mKey);
        CHECK(info.at(std::string(feature.mKey)) == fixture.mEditor.at(std::string(feature.mKey)));
    }

    // kept for running by hand, with what it printed
    CHECK(fs::exists(fixture.BuildDir() / "mjolnir_game_build.bat"));
    const std::optional<std::string> log = ReadText(fixture.BuildDir() / "mjolnir_game_build.log");
    REQUIRE(log);
    CHECK(Has(*log, "fake cmake --install"));

    const BuildReport second = RunGameBuild(fixture.Request());
    INFO(second.mError);
    REQUIRE(second.mOk);
    CHECK(fixture.Calls("--preset game-release -DMIR_LARGE_WORLD=OFF -DMIR_WITH_AUDIO=ON -DMIR_WITH_DLSS=OFF "
                        "-DMIR_WITH_RTXGI=ON") == 1);
    CHECK(fixture.Calls("--preset") == 1);
    CHECK(fixture.Calls("--build --preset game-release --target MjolnirEngine MjolnirErrorReporter") == 2);
    CHECK(fixture.Calls("--install") == 2);
}

TEST_CASE("An editor option that changed configures the game build again", "[game-build-compile]")
{
    CompileFixture fixture;
    REQUIRE(RunGameBuild(fixture.Request()).mOk);

    fixture.mEditor["dlss"] = true;
    Touch(fixture.mEditorInfo, fixture.mEditor.dump());
    const BuildReport report = RunGameBuild(fixture.Request());
    INFO(report.mError);
    REQUIRE(report.mOk);
    CHECK(fixture.Calls("--preset") == 2);
    CHECK(ReadJson(fixture.mGameBuildDir / "game_build_info.json").at("dlss") == true);
}

TEST_CASE("A failed compile keeps the previous template and build, and leaves no install beside them",
    "[game-build-compile]")
{
    CompileFixture fixture;
    REQUIRE(RunGameBuild(fixture.Request()).mOk);
    Touch(fixture.mGameBuildDir / "kept.txt");

    fixture.FailAt("compile");
    const BuildReport report = RunGameBuild(fixture.Request());
    CHECK_FALSE(report.mOk);
    REQUIRE(FindStep(report, "game build"));
    CHECK(FindStep(report, "game build")->mStatus == StepStatus::Failed);
    CHECK(Has(report.mError, "compiling the game build with game-release failed"));
    CHECK(Has(report.mError, PathText(fixture.BuildDir() / "mjolnir_game_build.log")));
    CHECK(Has(ReadText(fixture.BuildDir() / "mjolnir_game_build.log").value_or(""), "error C2065"));

    CHECK(fs::exists(fixture.mGameBuildDir / "kept.txt"));
    CHECK(fs::exists(fixture.mGameBuildDir / "MjolnirEngine.exe"));
    CHECK_FALSE(fs::exists(GameBuildStagingDir(fixture.mGameBuildDir)));
    CHECK(IsBuildDir(fixture.mOut));
    CHECK(fs::exists(fixture.mOut / "Demo.exe"));
    CHECK_FALSE(fs::exists(StagingDir(fixture.mOut)));
}

TEST_CASE("A failed configure or install says which, and installs nothing", "[game-build-compile]")
{
    for (const auto &[phase, message] : { std::pair{ "configure", "configuring game-release failed" },
             std::pair{ "install", "installing the game build from" } })
    {
        INFO(phase);
        CompileFixture fixture;
        fixture.FailAt(phase);

        const BuildReport report = RunGameBuild(fixture.Request());
        CHECK_FALSE(report.mOk);
        CHECK(Has(report.mError, message));
        CHECK_FALSE(fs::exists(fixture.mGameBuildDir));
        CHECK_FALSE(fs::exists(GameBuildStagingDir(fixture.mGameBuildDir)));
        CHECK_FALSE(fs::exists(fixture.mOut));
    }
}

TEST_CASE("A preset that installs an editor build keeps the previous template", "[game-build-compile]")
{
    CompileFixture fixture;
    REQUIRE(RunGameBuild(fixture.Request()).mOk);

    fixture.FailAt("editor");
    const BuildReport report = RunGameBuild(fixture.Request());
    CHECK_FALSE(report.mOk);
    CHECK(Has(report.mError, "editor build"));
    CHECK(Has(report.mError, "game preset"));
    CHECK(ReadJson(fixture.mGameBuildDir / "game_build_info.json").at("editor") == false);
    CHECK_FALSE(fs::exists(GameBuildStagingDir(fixture.mGameBuildDir)));
}

TEST_CASE("A bad output folder or an unknown preset fails in check, before anything compiles", "[game-build-compile]")
{
    CompileFixture fixture;
    BuildRequest request = fixture.Request();
    request.mGameBuildPreset = "missing";
    const BuildReport unknown = RunGameBuild(request);
    CHECK_FALSE(unknown.mOk);
    REQUIRE(unknown.mSteps.size() == 1);
    CHECK(unknown.mSteps[0].mName == "check");
    CHECK(Has(unknown.mError, "no preset named missing"));

    Touch(fixture.mOut / "notes.txt");
    const BuildReport badOut = RunGameBuild(fixture.Request());
    CHECK_FALSE(badOut.mOk);
    CHECK(badOut.mSteps.size() == 1);
    CHECK(Has(badOut.mError, "not empty"));

    CHECK_FALSE(fs::exists(fixture.mCheckout / "fake_cmake_calls.txt"));
    CHECK_FALSE(fs::exists(fixture.BuildDir()));
}

TEST_CASE("A template folder that is a build folder, a packaged game or other files is refused and kept",
    "[game-build-compile]")
{
    CompileFixture fixture;
    for (const auto &[name, file] : { std::pair{ "cmake", "CMakeCache.txt" }, std::pair{ "game", "mjolnir_build.json" },
             std::pair{ "other", "notes.txt" } })
    {
        INFO(name);
        const fs::path gameBuild = fixture.mSandbox.mRoot / name;
        Touch(gameBuild / file, "{}");

        BuildRequest request = fixture.Request();
        request.mGameBuildDir = gameBuild;
        const BuildReport report = RunGameBuild(request);
        CHECK_FALSE(report.mOk);
        CHECK(report.mSteps.size() == 1);
        CHECK(fs::exists(gameBuild / file));
    }
    CHECK_FALSE(fs::exists(fixture.mCheckout / "fake_cmake_calls.txt"));
}

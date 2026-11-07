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
#include <stdexcept>
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

    std::wstring EnvironmentText(const wchar_t *name)
    {
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        std::wstring value(size, L'\0');
        value.resize(GetEnvironmentVariableW(name, value.data(), size));
        return value;
    }

    // A cl.cmd and ninja.cmd that do nothing, for the game build step's where checks
    fs::path MakeFakeTools(const fs::path &dir)
    {
        Touch(dir / "cl.cmd", "@exit /b 0\r\n");
        Touch(dir / "ninja.cmd", "@exit /b 0\r\n");
        return fs::path(dir).make_preferred();
    }

    std::wstring FakeCMakeDir() { return fs::path(MJOLNIR_BUILD_FAKE_CMAKE_DIR).make_preferred().wstring(); }

    // Only what cmd itself needs, so a tool the test leaves out is missing
    std::wstring SystemPath() { return (fs::path(EnvironmentText(L"SystemRoot")) / "System32").wstring(); }

    // The test-only cmake.exe and the fake cl and ninja first on PATH, which the game build step's cmd inherits.
    // VCToolsInstallDir marks a Developer environment, so no Visual Studio is loaded
    struct FakeToolsOnPath
    {
        fs::path mTools;
        ScopedEnvironment mPath;
        ScopedEnvironment mDeveloper;

        explicit FakeToolsOnPath(const fs::path &dir)
            : mTools(MakeFakeTools(dir)),
              mPath(L"PATH", FakeCMakeDir() + L";" + mTools.wstring() + L";" + EnvironmentText(L"PATH")),
              mDeveloper(L"VCToolsInstallDir", mTools.wstring())
        {
        }
    };

    struct CompileFixture
    {
        Sandbox mSandbox;
        FakeToolsOnPath mPath{ mSandbox.mRoot / "tools" };
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

        fs::path BuildDir() const { return PresetBuildDir(mCheckout, kPreset).value(); }

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
        "configurePresets": [ { "name": "included", "binaryDir": "${sourceDir}/builds/included" } ],
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
    const fs::path buildDir = PresetBuildDir(checkout, kPreset).value();

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
    if (PresetProblem(source, preset) || PresetBuildDir(source, preset).value_or(fs::path()) != NormalizedPath(binaryDir))
        SKIP("these tests were not built in a preset's folder");

    Sandbox sandbox;
    CHECK(Has(CheckGameBuildCompile({ source, preset, sandbox.mRoot / "game-build", {} }, sandbox.mRoot / "out"),
        "this MjolnirBuild"));
}

TEST_CASE("A folder left beside the template is removed only when it is the game build step's", "[game-build-compile]")
{
    Sandbox sandbox;
    const fs::path checkout = sandbox.mRoot / "checkout";
    WritePresets(checkout);
    const GameBuildCompile compile{ checkout, std::string(kPreset), sandbox.mRoot / "game-build", {} };
    const fs::path staging = GameBuildStagingDir(compile.mGameBuildDir);
    const fs::path previous = GameBuildPreviousDir(compile.mGameBuildDir);

    Touch(staging / "notes.txt");
    CHECK(Has(CheckGameBuildCompile(compile, sandbox.mRoot / "out"), "installed there"));
    Touch(staging / "mjolnir_game_build_install", "");
    CHECK_FALSE(CheckGameBuildCompile(compile, sandbox.mRoot / "out"));

    Touch(previous / "notes.txt");
    CHECK(Has(CheckGameBuildCompile(compile, sandbox.mRoot / "out"), "moved there"));
    Touch(previous / "mjolnir_game_build_previous", "");
    CHECK_FALSE(CheckGameBuildCompile(compile, sandbox.mRoot / "out"));
    fs::remove_all(previous);
    MakeGameBuildTemplate(previous);
    CHECK_FALSE(CheckGameBuildCompile(compile, sandbox.mRoot / "out"));
}

TEST_CASE("The build folder is the preset's binaryDir, and one MjolnirBuild cannot expand is refused",
    "[game-build-compile]")
{
    Sandbox sandbox;
    const fs::path root = sandbox.mRoot / "checkout";
    Touch(root / "CMakePresets.json", R"({
  "version": 6,
  "configurePresets": [
    { "name": "base", "hidden": true, "binaryDir": "${sourceDir}/out/${presetName}" },
    { "name": "other", "hidden": true, "binaryDir": "${sourceDir}/other" },
    { "name": "inherited", "inherits": [ "base", "other" ] },
    { "name": "relative", "binaryDir": "rel/${presetName}" },
    { "name": "parent", "binaryDir": "${sourceParentDir}/${sourceDirName}-${presetName}${dollar}" },
    { "name": "env", "binaryDir": "$env{TEMP}/${presetName}" },
    { "name": "generator", "binaryDir": "${sourceDir}/${generator}" },
    { "name": "unset" }
  ],
  "buildPresets": [
    { "name": "inherited", "configurePreset": "inherited" },
    { "name": "env", "configurePreset": "env" },
    { "name": "unset", "configurePreset": "unset" }
  ]
})");
    const fs::path source = NormalizedPath(root);

    CHECK(PresetBuildDir(root, "inherited") == source / "out" / "inherited");
    CHECK(PresetBuildDir(root, "relative") == source / "rel" / "relative");
    CHECK(PresetBuildDir(root, "parent") == source.parent_path() / "checkout-parent$");
    CHECK_FALSE(PresetBuildDir(root, "env"));
    CHECK_FALSE(PresetBuildDir(root, "generator"));
    CHECK_FALSE(PresetBuildDir(root, "unset"));

    CHECK_FALSE(PresetProblem(root, "inherited"));
    CHECK(Has(PresetProblem(root, "env"), "does not expand"));
    CHECK(Has(PresetProblem(root, "unset"), "sets no binaryDir"));
}

TEST_CASE("A checkout path with non-ASCII characters expands as written", "[game-build-compile]")
{
    Sandbox sandbox;
    const fs::path root = sandbox.mRoot / fs::path(u8"ch\u00e9ckout \u2713 \u65e5\u672c");
    Touch(root / "CMakePresets.json", R"({
  "version": 6,
  "configurePresets": [
    { "name": "game-release", "binaryDir": "${sourceDir}/builds/${presetName}" },
    { "name": "parent", "binaryDir": "${sourceParentDir}/${sourceDirName}-out" },
    { "name": "file", "binaryDir": "${fileDir}/file" }
  ],
  "buildPresets": [ { "name": "game-release", "configurePreset": "game-release" } ]
})");
    const fs::path source = NormalizedPath(root);

    CHECK(PresetBuildDir(root, "game-release") == source / "builds" / "game-release");
    CHECK(PresetBuildDir(root, "parent") == source.parent_path() / fs::path(u8"ch\u00e9ckout \u2713 \u65e5\u672c-out"));
    CHECK(PresetBuildDir(root, "file") == source / "file");
    CHECK_FALSE(PresetProblem(root, "game-release"));
}

TEST_CASE("No folder the game build step writes may overlap another", "[game-build-compile]")
{
    Sandbox sandbox;
    const fs::path checkout = sandbox.mRoot / "checkout";
    const fs::path gameBuild = sandbox.mRoot / "game-build";
    const fs::path out = sandbox.mRoot / "out";
    Touch(checkout / "CMakePresets.json", R"({
  "version": 6,
  "configurePresets": [
    { "name": "ok", "binaryDir": "${sourceDir}/builds/${presetName}" },
    { "name": "staging", "binaryDir": "${sourceParentDir}/game-build.tmp" },
    { "name": "previous", "binaryDir": "${sourceParentDir}/game-build.old/build" },
    { "name": "out-staging", "binaryDir": "${sourceParentDir}/out.tmp" },
    { "name": "out-previous", "binaryDir": "${sourceParentDir}/out.old" }
  ],
  "buildPresets": [
    { "name": "ok", "configurePreset": "ok" },
    { "name": "staging", "configurePreset": "staging" },
    { "name": "previous", "configurePreset": "previous" },
    { "name": "out-staging", "configurePreset": "out-staging" },
    { "name": "out-previous", "configurePreset": "out-previous" }
  ]
})");

    CHECK_FALSE(CheckGameBuildCompile({ checkout, "ok", gameBuild, {} }, out));
    for (const auto &[preset, folder] : { std::pair{ "staging", "the template's install folder" },
             std::pair{ "previous", "the folder the previous template moves to" },
             std::pair{ "out-staging", "the output's staging folder" },
             std::pair{ "out-previous", "the folder the previous output moves to" } })
    {
        INFO(preset);
        // a fresh build folder, which only the overlap check can catch
        CHECK_FALSE(fs::exists(PresetBuildDir(checkout, preset).value()));
        const std::optional<std::string> problem = CheckGameBuildCompile({ checkout, preset, gameBuild, {} }, out);
        CHECK(Has(problem, "the preset's build folder"));
        CHECK(Has(problem, folder));
    }

    // the template's folders and the output's
    CHECK(Has(CheckGameBuildCompile({ checkout, "ok", gameBuild, {} }, sandbox.mRoot / "game-build.tmp"),
        "the template's install folder"));
    CHECK(Has(CheckGameBuildCompile({ checkout, "ok", gameBuild, {} }, sandbox.mRoot / "game-build.old" / "out"),
        "the folder the previous template moves to"));
    CHECK(Has(CheckGameBuildCompile({ checkout, "ok", sandbox.mRoot / "out.tmp", {} }, out), "the output's staging"));

    // the compile refuses too, without the output folder to compare
    std::string error;
    try
    {
        CompileGameBuild({ checkout, "staging", gameBuild, {} });
    }
    catch (const std::runtime_error &e)
    {
        error = e.what();
    }
    CHECK(Has(error, "overlaps"));
    CHECK_FALSE(fs::exists(GameBuildStagingDir(gameBuild)));
}

TEST_CASE("The game build is configured with the editor's options", "[game-build-compile]")
{
    const nlohmann::json editor = { { "editor", true }, { "largeWorld", true }, { "audio", false }, { "dlss", false },
        { "rtxgi", true } };
    CHECK(FeatureDefines(editor) == std::vector<std::string>{ "-DMIR_LARGE_WORLD=ON", "-DMIR_WITH_AUDIO=OFF",
                                        "-DMIR_WITH_DLSS=OFF", "-DMIR_WITH_RTXGI=ON" });
    // an option the editor does not name is unset, for the preset's default
    CHECK(FeatureDefines(nlohmann::json{ { "audio", true }, { "dlss", "yes" } }) ==
        std::vector<std::string>{ "-UMIR_LARGE_WORLD", "-DMIR_WITH_AUDIO=ON", "-UMIR_WITH_DLSS", "-UMIR_WITH_RTXGI" });
}

TEST_CASE("The game's choices replace the editor's options, and the rest follow the editor", "[game-build-compile]")
{
    const nlohmann::json editor = { { "editor", true }, { "largeWorld", true }, { "audio", false }, { "dlss", false },
        { "rtxgi", true } };
    GameBuildChoices choices;
    choices.mOn = { { "dlss", true }, { "largeWorld", false } };
    CHECK(FeatureDefines(editor, choices) == std::vector<std::string>{ "-DMIR_LARGE_WORLD=OFF", "-DMIR_WITH_AUDIO=OFF",
                                                 "-DMIR_WITH_DLSS=ON", "-DMIR_WITH_RTXGI=ON" });

    // without the editor's, what the game chose is set and the rest unset, except what the preset sets every configure
    CHECK(FeatureDefines(nlohmann::json(), choices) == std::vector<std::string>{ "-DMIR_LARGE_WORLD=OFF",
                                                           "-UMIR_WITH_AUDIO", "-DMIR_WITH_DLSS=ON", "-UMIR_WITH_RTXGI" });
    CHECK(FeatureDefines(nlohmann::json(), choices, { "MIR_WITH_AUDIO" }) ==
        std::vector<std::string>{ "-DMIR_LARGE_WORLD=OFF", "-DMIR_WITH_DLSS=ON", "-UMIR_WITH_RTXGI" });
}

TEST_CASE("The options a preset sets are found through what it inherits, as CMake reads them", "[game-build-compile]")
{
    Sandbox sandbox;
    Touch(sandbox.mRoot / "CMakePresets.json", R"({
  "version": 6,
  "configurePresets": [
    { "name": "base", "hidden": true, "cacheVariables": { "MIR_WITH_DLSS": "OFF", "MIR_WITH_AUDIO": false } },
    { "name": "other", "hidden": true,
      "cacheVariables": { "MIR_WITH_DLSS": null, "MIR_WITH_RTXGI": { "type": "BOOL", "value": "ON" } } },
    { "name": "game", "inherits": [ "base", "other" ], "cacheVariables": { "MIR_WITH_AUDIO": null } },
    { "name": "plain" }
  ]
})");

    // its own null unsets audio, and the first parent's DLSS wins over the second's null
    CHECK(PresetCacheOptions(sandbox.mRoot, "game") == std::vector<std::string>{ "MIR_WITH_DLSS", "MIR_WITH_RTXGI" });
    CHECK(PresetCacheOptions(sandbox.mRoot, "base") == std::vector<std::string>{ "MIR_WITH_AUDIO", "MIR_WITH_DLSS" });
    CHECK(PresetCacheOptions(sandbox.mRoot, "plain").empty());
    CHECK(PresetCacheOptions(sandbox.mRoot, "missing").empty());
}

TEST_CASE("An option neither the game nor the editor names is unset, so an earlier build's choice is not kept",
    "[game-build-compile]")
{
    CompileFixture fixture;
    BuildRequest request = fixture.Request();
    request.mEditorInfo = fixture.mSandbox.mRoot / "no-editor-info.json";
    const fs::path settings = fixture.mCheckout / "games" / "Demo" / "project_settings.json";

    Touch(settings, R"({ "build": { "gameBuildOptions": { "dlss": true } } })");
    const BuildReport chosen = RunGameBuild(request);
    INFO(chosen.mError);
    REQUIRE(chosen.mOk);
    CHECK(fixture.Calls("--preset game-release -UMIR_LARGE_WORLD -UMIR_WITH_AUDIO -DMIR_WITH_DLSS=ON -UMIR_WITH_RTXGI") ==
        1);
    CHECK(ReadJson(fixture.mGameBuildDir / "game_build_info.json").at("dlss") == true);

    // fake cmake keeps its cache as CMake does, and has no option defaults, so an unset option is left out
    Touch(settings, "{}");
    const BuildReport unchosen = RunGameBuild(request);
    INFO(unchosen.mError);
    REQUIRE(unchosen.mOk);
    CHECK(fixture.Calls("--preset game-release -UMIR_LARGE_WORLD -UMIR_WITH_AUDIO -UMIR_WITH_DLSS -UMIR_WITH_RTXGI") ==
        1);
    CHECK_FALSE(ReadJson(fixture.mGameBuildDir / "game_build_info.json").contains("dlss"));
}

TEST_CASE("A game's chosen options configure the game build, and the template is not warned about them",
    "[game-build-compile]")
{
    CompileFixture fixture;
    Touch(fixture.mCheckout / "games" / "Demo" / "project_settings.json",
        R"({ "rendering": { "rayTracing": true },
             "build": { "gameBuildOptions": { "dlss": true, "rtxgi": false, "DLSS": false } } })");

    const BuildReport report = RunGameBuild(fixture.Request());
    INFO(report.mError);
    REQUIRE(report.mOk);
    CHECK(fixture.Calls("--preset game-release -DMIR_LARGE_WORLD=OFF -DMIR_WITH_AUDIO=ON -DMIR_WITH_DLSS=ON "
                        "-DMIR_WITH_RTXGI=OFF") == 1);
    const nlohmann::json info = ReadJson(fixture.mGameBuildDir / "game_build_info.json");
    CHECK(info.at("dlss") == true);
    CHECK(info.at("rtxgi") == false);

    // the template differs from the editor only where the game chose, so the one warning is the bad entry
    REQUIRE(report.mWarnings.size() == 1);
    CHECK(Has(report.mWarnings[0], "build.gameBuildOptions.DLSS is not a build option (largeWorld, audio, dlss, rtxgi)"));

    // Reset to the editor's, the next build configures with the editor's again
    Touch(fixture.mCheckout / "games" / "Demo" / "project_settings.json", R"({ "rendering": { "rayTracing": true } })");
    const BuildReport reset = RunGameBuild(fixture.Request());
    INFO(reset.mError);
    REQUIRE(reset.mOk);
    CHECK(reset.mWarnings.empty());
    CHECK(fixture.Calls("--preset game-release -DMIR_LARGE_WORLD=OFF -DMIR_WITH_AUDIO=ON -DMIR_WITH_DLSS=OFF "
                        "-DMIR_WITH_RTXGI=ON") == 1);
    CHECK(ReadJson(fixture.mGameBuildDir / "game_build_info.json").at("dlss") == false);
}

TEST_CASE("The batch file configures, then compiles the game build's two targets and installs", "[game-build-compile]")
{
    const GameBuildScript script{ "C:/My Checkout/100%", "game-release", "C:/My Checkout/100%/out/game-release",
        "C:/out/game-build.tmp",
        std::vector<std::string>{ "-DMIR_WITH_AUDIO=ON", "-DMIR_WITH_DLSS=OFF" } };
    const std::string text = GameBuildScriptText(script);

    CHECK(text.starts_with("@echo off\r\n"));
    CHECK(Has(text, "cd /d \"C:\\My Checkout\\100%%\" || exit /b 10\r\n"));
    CHECK(Has(text, "cmake --preset game-release -DMIR_WITH_AUDIO=ON -DMIR_WITH_DLSS=OFF || exit /b 11\r\n"));
    CHECK(Has(text, "cmake --build --preset game-release --target MjolnirEngine MjolnirErrorReporter || exit /b 12\r\n"));
    CHECK(Has(text, "cmake --install \"C:\\My Checkout\\100%%\\out\\game-release\" --prefix \"C:\\out\\game-build.tmp\" "
                    "|| exit /b 13\r\n"));
    CHECK(text.find("cmake --preset") < text.find("cmake --build"));
    CHECK(text.find("cmake --build") < text.find("cmake --install"));

    // a Developer environment loads nothing, but the tools are still checked
    CHECK_FALSE(Has(text, "call "));
    CHECK(Has(text, "where /q cl || exit /b 15\r\n"));
    CHECK(Has(text, "where /q cmake || exit /b 16\r\n"));
    CHECK(Has(text, "where /q ninja || exit /b 16\r\n"));
    CHECK(text.find("where /q") < text.find("cd /d"));

    // each phase says so in the log as it starts
    std::size_t at = 0;
    for (const auto &[marker, command] : { std::pair{ "echo mjolnir_game_build: environment\r\n", "where /q cl" },
             std::pair{ "echo mjolnir_game_build: configure\r\n", "cmake --preset" },
             std::pair{ "echo mjolnir_game_build: compile\r\n", "cmake --build" },
             std::pair{ "echo mjolnir_game_build: install\r\n", "cmake --install" } })
    {
        INFO(marker);
        const std::size_t found = text.find(marker, at);
        REQUIRE(found != std::string::npos);
        CHECK(found < text.find(command));
        at = found + 1;
    }
}

TEST_CASE("The batch file loads Visual Studio first, with the toolset the build folder was configured with",
    "[game-build-compile]")
{
    GameBuildScript script{ "C:/checkout", "game-release", "C:/checkout/builds/game-release", "C:/out/game-build.tmp",
        {}, { "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat", "14.51.36231" } };
    std::string text = GameBuildScriptText(script);
    CHECK(Has(text, "cd /d \"C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\" "
                    "|| exit /b 14\r\n"));
    CHECK(Has(text, "call .\\vcvars64.bat -vcvars_ver=14.51.36231 || exit /b 14\r\n"));
    CHECK(text.find("call ") < text.find("where /q cl"));
    CHECK(text.find("call ") < text.find("cd /d \"C:\\checkout\""));

    script.mVisualStudio.mToolset.clear();
    text = GameBuildScriptText(script);
    CHECK(Has(text, "call .\\vcvars64.bat || exit /b 14\r\n"));
    CHECK_FALSE(Has(text, "-vcvars_ver"));
}

TEST_CASE("Visual Studio under a folder with % and ^ in its name is loaded as named", "[game-build-compile]")
{
    CompileFixture fixture;
    const ScopedEnvironment plain(L"VCToolsInstallDir", std::nullopt);
    // what %SDK% would expand to, were the path expanded twice
    const ScopedEnvironment sdk(L"SDK", L"wrong");

    const fs::path install = fixture.mSandbox.mRoot / "D 100%SDK%^x" / "Visual Studio";
    const fs::path build = install / "VC" / "Auxiliary" / "Build";
    Touch(build / "vcvars64.bat", "@echo %*> called.txt\r\n@exit /b 0\r\n");
    const fs::path cl = install / "VC" / "Tools" / "MSVC" / "14.51.36231" / "bin" / "Hostx64" / "x64" / "cl.exe";
    Touch(cl);
    Touch(fixture.BuildDir() / "CMakeCache.txt", "CMAKE_CXX_COMPILER:FILEPATH=" + PathText(cl) + "\r\n");

    const BuildReport report = RunGameBuild(fixture.Request());
    INFO(report.mError);
    INFO(ReadText(fixture.BuildDir() / "mjolnir_game_build.log").value_or(""));
    REQUIRE(report.mOk);
    CHECK(Has(ReadText(build / "called.txt").value_or(""), "-vcvars_ver=14.51.36231"));
    CHECK(fixture.Calls("--install") == 1);
}

TEST_CASE("A missing compiler or CMake fails naming the Visual Studio component to install", "[game-build-compile]")
{
    for (const auto &[missing, message] : { std::pair{ "cl", "\"Desktop development with C++\" workload" },
             std::pair{ "cmake", "\"C++ CMake tools for Windows\"" } })
    {
        INFO(missing);
        CompileFixture fixture;
        MakeGameBuildTemplate(fixture.mGameBuildDir);
        const std::wstring tools = fixture.mPath.mTools.wstring();
        const ScopedEnvironment path(L"PATH",
            (std::string_view(missing) == "cl" ? FakeCMakeDir() : tools) + L";" + SystemPath());

        const BuildReport report = RunGameBuild(fixture.Request());
        CHECK_FALSE(report.mOk);
        REQUIRE(FindStep(report, "game build"));
        CHECK(FindStep(report, "game build")->mStatus == StepStatus::Failed);
        CHECK(Has(report.mError, message));
        CHECK(fixture.Calls("--preset") == 0);
        CHECK(fs::exists(fixture.mGameBuildDir / "MjolnirEngine.exe"));
        CHECK_FALSE(fs::exists(GameBuildStagingDir(fixture.mGameBuildDir)));
    }
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

TEST_CASE("The game build step configures and installs a fresh template with the editor's options each build",
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
                        "-DMIR_WITH_RTXGI=ON") == 2);
    CHECK(fixture.Calls("--preset") == 2);
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
    // the phase, the compiler's own line and the log, not Ninja's FAILED: line
    CHECK(Has(report.mError, "compiling the game build with game-release failed:\n"
                             "  Engine.cpp(1): error C2065: 'oops': undeclared identifier\n"
                             "The whole output is in " +
            PathText(fixture.BuildDir() / "mjolnir_game_build.log")));
    CHECK_FALSE(Has(report.mError, "FAILED:"));

    CHECK(fs::exists(fixture.mGameBuildDir / "kept.txt"));
    CHECK(fs::exists(fixture.mGameBuildDir / "MjolnirEngine.exe"));
    CHECK_FALSE(fs::exists(GameBuildStagingDir(fixture.mGameBuildDir)));
    CHECK(IsBuildDir(fixture.mOut));
    CHECK(fs::exists(fixture.mOut / "Demo.exe"));
    CHECK_FALSE(fs::exists(StagingDir(fixture.mOut)));
}

TEST_CASE("A failed configure or install says which, and installs nothing", "[game-build-compile]")
{
    for (const auto &[phase, message] :
        { std::pair{ "configure", "configuring game-release failed:\n  CMake Error at CMakeLists.txt:1 (project): No "
                                  "CMAKE_CXX_COMPILER could be found.\nThe whole output is in " },
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

TEST_CASE("A template with a locked file is kept whole, and the next compile replaces it", "[game-build-compile]")
{
    CompileFixture fixture;
    REQUIRE(RunGameBuild(fixture.Request()).mOk);
    Touch(fixture.mGameBuildDir / "kept.txt");
    const fs::path out = fixture.mOut;
    fs::remove_all(out);

    {
        // as a running game or a virus scan holds it
        const HANDLE locked = CreateFileW((fixture.mGameBuildDir / "MjolnirEngine.exe").c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(locked != INVALID_HANDLE_VALUE);
        const BuildReport report = RunGameBuild(fixture.Request());
        CloseHandle(locked);

        CHECK_FALSE(report.mOk);
        CHECK(Has(report.mError, "aside"));
        CHECK(fs::exists(fixture.mGameBuildDir / "kept.txt"));
        CHECK(fs::exists(fixture.mGameBuildDir / "game_build_info.json"));
        CHECK(fs::exists(fixture.mGameBuildDir / "MjolnirEngine.exe"));
        CHECK_FALSE(fs::exists(GameBuildStagingDir(fixture.mGameBuildDir)));
        CHECK_FALSE(fs::exists(GameBuildPreviousDir(fixture.mGameBuildDir)));
    }

    // what an earlier compile could not finish removing
    MakeGameBuildTemplate(GameBuildPreviousDir(fixture.mGameBuildDir));
    Touch(GameBuildPreviousDir(fixture.mGameBuildDir) / "mjolnir_game_build_previous", "");
    REQUIRE(RunGameBuild(fixture.Request()).mOk);
    CHECK_FALSE(fs::exists(fixture.mGameBuildDir / "kept.txt"));
    CHECK_FALSE(fs::exists(fixture.mGameBuildDir / "mjolnir_game_build_install"));
    CHECK_FALSE(fs::exists(GameBuildPreviousDir(fixture.mGameBuildDir)));
}

TEST_CASE("A preset with its own binaryDir is compiled and installed from there", "[game-build-compile]")
{
    CompileFixture fixture;
    Touch(fixture.mCheckout / "CMakePresets.json", R"({
  "version": 6,
  "configurePresets": [ { "name": "game-release", "binaryDir": "${sourceDir}/out/custom" } ],
  "buildPresets": [ { "name": "game-release", "configurePreset": "game-release" } ]
})");
    Touch(fixture.mCheckout / "fake_cmake_binary_dir.txt", "out/custom");

    REQUIRE(fixture.BuildDir() == NormalizedPath(fixture.mCheckout / "out" / "custom"));
    const BuildReport report = RunGameBuild(fixture.Request());
    INFO(report.mError);
    REQUIRE(report.mOk);
    CHECK(fs::exists(fixture.BuildDir() / "mjolnir_game_build.bat"));
    CHECK(fs::exists(fixture.BuildDir() / "mjolnir_game_build.log"));
    CHECK_FALSE(fs::exists(fixture.mCheckout / "builds"));
    CHECK(ReadJson(fixture.mGameBuildDir / "game_build_info.json").at("rtxgi") == true);
}

TEST_CASE("Every step is told as it starts, and the compile's phases and Ninja's count as they change",
    "[game-build-compile]")
{
    CompileFixture fixture;
    Touch(fixture.mCheckout / "fake_cmake_slow.txt", "");
    std::vector<BuildProgress> told;
    BuildRequest request = fixture.Request();
    request.mOnProgress = [&](const BuildProgress &progress) { told.push_back(progress); };
    const BuildReport report = RunGameBuild(request);
    INFO(report.mError);
    REQUIRE(report.mOk);

    std::vector<std::string> steps;
    std::vector<std::string> phases;
    std::vector<std::size_t> done;
    for (const BuildProgress &progress : told)
    {
        if (steps.empty() || steps.back() != progress.mStep)
            steps.push_back(progress.mStep);
        if (progress.mStep != "game build")
        {
            CHECK(progress.mPhase.empty());
            continue;
        }
        if (!progress.mPhase.empty() && (phases.empty() || phases.back() != progress.mPhase))
            phases.push_back(progress.mPhase);
        if (progress.mTotal > 0)
        {
            CHECK(progress.mPhase == "compile");
            CHECK(progress.mTotal == 3);
            done.push_back(progress.mDone);
        }
    }

    // every step once, in order
    std::vector<std::string> ran;
    for (const BuildStep &step : report.mSteps)
        ran.push_back(step.mName);
    CHECK(steps == ran);

    // the log is read every 250 ms, so a quick phase may pass unseen, but none goes back
    const std::vector<std::string> order = { "environment", "configure", "compile", "install" };
    std::size_t at = 0;
    for (const std::string &phase : phases)
    {
        INFO(phase);
        const auto found = std::find(order.begin() + static_cast<std::ptrdiff_t>(at), order.end(), phase);
        REQUIRE(found != order.end());
        at = static_cast<std::size_t>(found - order.begin()) + 1;
    }
    CHECK(std::find(phases.begin(), phases.end(), "compile") != phases.end());
    CHECK(phases.back() == "install");

    // Ninja's count rises to its total
    REQUIRE(done.size() >= 2);
    CHECK(std::is_sorted(done.begin(), done.end()));
    CHECK(done.front() < 3);
    CHECK(done.back() == 3);
}

TEST_CASE("The progress file holds the step, and the phase and Ninja's count once there are any", "[game-build-compile]")
{
    CHECK(BuildProgressJson({ "cook" }) == "{\"version\":1,\"step\":\"cook\"}\n");
    CHECK(BuildProgressJson({ "game build", "configure" }) ==
        "{\"version\":1,\"step\":\"game build\",\"phase\":\"configure\"}\n");
    CHECK(BuildProgressJson({ "game build", "compile", 412, 1310 }) ==
        "{\"version\":1,\"step\":\"game build\",\"phase\":\"compile\",\"done\":412,\"total\":1310}\n");
}

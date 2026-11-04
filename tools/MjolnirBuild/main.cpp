/**
 * @file main.cpp
 * @author Quy Dong Ta
 * @brief MjolnirBuild command line: packages one game with a game build template, compiled first with
 *        --game-build-preset, into a folder.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "BuildOutput.h"
#include "GameBuild.h"

#include <core/fs/FileIO.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
    namespace fs = std::filesystem;
    using namespace mir::build;

    // Exit codes: 0 built, 1 the build failed, 2 the command line is wrong.
    constexpr int kExitFailed = 1;
    constexpr int kExitUsage = 2;

    constexpr const char *kUsage =
        "usage:\n"
        "  MjolnirBuild --game <checkout>/games/<name> --game-build <template> --out <dir>\n"
        "               [--game-build-preset <preset>] [--result <json>]\n"
        "Cooks the game, then copies it with the game build template into <dir>, where <dir>/<name>.exe\n"
        "starts it. Built in <dir>.tmp and moved into place only when every step succeeds, and\n"
        "compared with this build's game_build_info.json. <dir> must be empty, a previous build, or\n"
        "missing, and lie outside the checkout or under its builds/.\n"
        "With --game-build-preset, the game build is first compiled from the checkout with that CMake\n"
        "preset, e.g. windows-msvc-game-release, with this build's large world, audio, DLSS and\n"
        "RTXGI options, and installed fresh into <template>, which must then be missing, empty or\n"
        "a game build template. The commands are kept in the preset's build folder as\n"
        "mjolnir_game_build.bat, with their output in mjolnir_game_build.log. Run it from a\n"
        "Developer Command Prompt.\n"
        "Without it, <template> is used as it is; make one with\n"
        "  cmake --install builds/windows-msvc-game-release --prefix <template>\n"
        "Exit 0: built; 1: the build failed; 2: bad arguments.\n";

    struct UsageError : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    std::map<std::string, std::string, std::less<>> Parse(int argc, char **argv)
    {
        constexpr std::string_view kAllowed[] = { "--game", "--game-build", "--out", "--game-build-preset", "--result" };

        std::map<std::string, std::string, std::less<>> values;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (std::find(std::begin(kAllowed), std::end(kAllowed), arg) == std::end(kAllowed))
                throw UsageError("unknown argument: " + arg);
            if (i + 1 >= argc)
                throw UsageError("missing value after " + arg);
            if (!values.emplace(arg, argv[++i]).second)
                throw UsageError(arg + " is given twice");
        }
        for (std::string_view name : { "--game", "--game-build", "--out" })
        {
            if (!values.contains(name))
                throw UsageError(std::string(name) + " is required");
        }
        return values;
    }

    void Print(const BuildReport &report)
    {
        for (const BuildStep &step : report.mSteps)
        {
            const char *status = step.mStatus == StepStatus::Succeeded ? "ok"
                : step.mStatus == StepStatus::Skipped                  ? "skipped"
                                                                       : "FAILED";
            std::printf("MjolnirBuild: %-11s %-7s %6.2f s\n", step.mName.c_str(), status, step.mSeconds);
        }
        
        std::fflush(stdout);
        for (const std::string &warning : report.mWarnings)
            std::fprintf(stderr, "MjolnirBuild: WARNING: %s\n", warning.c_str());

        if (report.mOk)
            std::printf("MjolnirBuild: built %s into %s (%zu assets, %.1f MB); run %s\n", report.mGame.c_str(),
                PathText(report.mOutDir).c_str(), report.mAssetCount, static_cast<double>(report.mBytes) / 1e6,
                PathText(report.mExecutable.filename()).c_str());
        else
            std::fprintf(stderr, "MjolnirBuild: error: %s\n", report.mError.c_str());
    }
}

int main(int argc, char **argv)
{
    
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h"))
    {
        std::fputs(kUsage, stdout);
        return 0;
    }

    try
    {
        const auto values = Parse(argc, argv);
        const fs::path result = values.contains("--result") ? fs::path(values.find("--result")->second) : fs::path();
        const fs::path out = values.find("--out")->second;
        for (const fs::path &dir : { NormalizedPath(out), StagingDir(out), PreviousDir(out) })
        {
            if (!result.empty() && IsWithin(NormalizedPath(result), dir))
                throw UsageError("--result must not be inside --out or its .tmp and .old folders");
        }

        BuildRequest request;
        request.mGameDir = values.find("--game")->second;
        request.mGameBuildDir = values.find("--game-build")->second;
        if (const auto preset = values.find("--game-build-preset"); preset != values.end())
        {
            if (preset->second.empty())
                throw UsageError("--game-build-preset needs a preset name");
            request.mGameBuildPreset = preset->second;
        }
        request.mOutDir = out;
        request.mAssetCooker = MJOLNIR_BUILD_ASSET_COOKER;
        request.mTerrainTool = MJOLNIR_BUILD_TERRAIN_TOOL;
        request.mEditorInfo = MJOLNIR_BUILD_EDITOR_INFO;

        const BuildReport report = RunGameBuild(request);
        Print(report);

        if (!result.empty() && !mir::WriteTextAtomic(result, BuildReportJson(report)))
        {
            std::fprintf(stderr, "MjolnirBuild: error: could not write %s\n", PathText(result).c_str());
            return kExitFailed;
        }
        return report.mOk ? 0 : kExitFailed;
    }
    catch (const UsageError &error)
    {
        std::fprintf(stderr, "MjolnirBuild: %s\n%s", error.what(), kUsage);
        return kExitUsage;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "MjolnirBuild: error: %s\n", error.what());
        return kExitFailed;
    }
}

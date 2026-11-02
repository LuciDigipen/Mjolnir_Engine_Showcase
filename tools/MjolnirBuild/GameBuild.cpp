/**
 * @file GameBuild.cpp
 * @author Quy Dong Ta
 * @brief The build steps: check, template, cook, game data, sound banks, terrain, finish.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "GameBuild.h"

#include "BuildOutput.h"

#include <assets/AssetIdRoot.h>
#include <assets/AssetManifest.h>
#include <core/log/SetupError.h>
#include <core/platform/Process.h>
#include <project/GameProject.h>
#include <serial/JsonFile.h>
#include <terrain/TerrainStreamingSettings.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace mir::build
{
    namespace fs = std::filesystem;
    using Json = nlohmann::json;

    namespace
    {
        constexpr std::string_view kPlayerExecutable = "MjolnirEngine.exe";
        constexpr std::string_view kPlayerInfoFileName = "player_info.json";

        // "cooked/" plus an id and a content hash, as AssetCooker names a blob
        constexpr std::size_t kCookedPathLength = 45;

        // What a template can pick up from a run or a hand-copied game
        constexpr std::string_view kNotTemplate[] = { "logs", "games", "cooked", "engine.json", kBuildMarkerFileName };

        struct BuildError : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        struct GameInputs
        {
            fs::path mSourceRoot;
            fs::path mGameDir;
            std::string mDirName;
            GameProject mProject;
            fs::path mPlayerDir;
            fs::path mOutDir;
        };

        template <typename Work>
        void RunStep(BuildReport &report, std::string name, Work &&work)
        {
            const std::size_t index = report.mSteps.size();
            report.mSteps.push_back({ std::move(name) });
            const auto start = std::chrono::steady_clock::now();
            const auto finish = [&](StepStatus status) {
                report.mSteps[index].mStatus = status;
                report.mSteps[index].mSeconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            };

            try
            {
                // work returns false when there was nothing to do
                const bool did = work();
                finish(did ? StepStatus::Succeeded : StepStatus::Skipped);
            }
            catch (...)
            {
                finish(StepStatus::Failed);
                throw;
            }
        }

        std::string_view StatusText(StepStatus status)
        {
            switch (status)
            {
            case StepStatus::Succeeded:
                return "succeeded";
            case StepStatus::Skipped:
                return "skipped";
            default:
                return "failed";
            }
        }

        fs::path TempFile(std::string_view tag)
        {
            static std::atomic_uint32_t sequence{ 0 };
            return fs::temp_directory_path() / ("mjolnir-build-" + std::to_string(CurrentProcessId()) + "-" +
                std::to_string(sequence.fetch_add(1)) + "-" + std::string(tag) + ".json");
        }

        void Copy(const fs::path &from, const fs::path &to)
        {
            std::error_code ec;
            fs::create_directories(to.parent_path(), ec);
            fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec)
                throw BuildError("could not copy " + PathText(from) + " to " + PathText(to) + ": " + ec.message());
        }

        std::size_t LongestPath(const fs::path &dir)
        {
            std::size_t longest = 0;
            for (const fs::directory_entry &entry : fs::recursive_directory_iterator(dir))
                longest = std::max(longest, entry.path().lexically_relative(dir).native().size());
            return longest;
        }

        std::uintmax_t FolderBytes(const fs::path &dir)
        {
            std::uintmax_t bytes = 0;
            for (const fs::directory_entry &entry : fs::recursive_directory_iterator(dir))
            {
                if (entry.is_regular_file())
                    bytes += entry.file_size();
            }
            return bytes;
        }

        int RunTool(std::string_view name, const fs::path &program, const std::vector<std::string> &args,
            const fs::path &workingDir)
        {
            std::error_code ec;
            if (!fs::is_regular_file(program, ec))
                throw BuildError(std::string(name) + " was not found at " + PathText(program) + "; build it first");

            ChildProcess process;
            if (!process.Start(program, args, workingDir))
                throw BuildError("could not start " + std::string(name) + " at " + PathText(program));
            const std::optional<int> exit = process.Wait();
            if (!exit)
                throw BuildError("lost track of " + std::string(name));
            return *exit;
        }

        // The message a tool left in its result file, if any, and the file removed
        std::string TakeMessage(const fs::path &resultFile, const Json::json_pointer &where)
        {
            std::string message;
            if (const std::optional<Json> json = ReadJsonFile(resultFile); json && json->contains(where))
            {
                const Json &value = json->at(where);
                if (value.is_string())
                    message = value.get<std::string>();
            }
            std::error_code ec;
            fs::remove(resultFile, ec);
            return message;
        }

        GameInputs ResolveInputs(const BuildRequest &request)
        {
            GameInputs inputs;
            std::error_code ec;

            inputs.mGameDir = NormalizedPath(request.mGameDir);
            inputs.mDirName = PathText(inputs.mGameDir.filename());
            if (!fs::is_regular_file(inputs.mGameDir / kGameManifestFileName, ec))
                throw BuildError(PathText(inputs.mGameDir) + " is not a game: it has no game.json");
            if (inputs.mGameDir.parent_path().filename() != "games")
                throw BuildError(PathText(inputs.mGameDir) +
                    " is not in a games/ folder, so its asset ids would not match the editor's");

            inputs.mSourceRoot = inputs.mGameDir.parent_path().parent_path();
            if (!fs::is_directory(inputs.mSourceRoot / "assets", ec))
                throw BuildError(PathText(inputs.mSourceRoot) + " has no engine assets/ folder");

            // An absolute directory keeps ProjectRoot() out of it; ids still need the games/<name> form
            inputs.mProject = LoadGameProject(inputs.mGameDir);
            inputs.mProject.mDir = "games/" + inputs.mDirName;
            ResolveAssetIdRoot(inputs.mProject);

            // The package keeps this game.json, so a rooted path would still read from the checkout
            const std::optional<Json> manifest = ReadJsonFile(inputs.mGameDir / kGameManifestFileName);
            for (const std::string key : { "sceneDir", "soundBanksRoot" })
            {
                const std::string value = manifest ? manifest->value(key, std::string()) : std::string();
                if (fs::path(value).has_root_path())
                    throw BuildError("game.json's " + key + " \"" + value +
                        "\" is not a relative path, so the packaged game would read it from this checkout; "
                        "make it relative to the game folder");
            }
            for (const fs::path &dir : { inputs.mProject.mSceneDir, inputs.mProject.mSoundBankDir })
            {
                if (!IsWithin(dir, inputs.mGameDir))
                    throw BuildError("game.json names " + PathText(dir) + ", which is outside the game folder");
            }

            inputs.mPlayerDir = NormalizedPath(request.mPlayerDir);
            const std::string install =
                "; install one with cmake --install builds/windows-msvc-game-release --prefix <dir>";
            if (!fs::is_directory(inputs.mPlayerDir, ec))
                throw BuildError("the player template " + PathText(inputs.mPlayerDir) + " does not exist" + install);
            if (!fs::is_regular_file(inputs.mPlayerDir / kPlayerExecutable, ec))
                throw BuildError(PathText(inputs.mPlayerDir) + " has no " + std::string(kPlayerExecutable) + install);
            const std::optional<Json> info = ReadJsonFile(inputs.mPlayerDir / kPlayerInfoFileName);
            if (!info || !info->is_object())
                throw BuildError(PathText(inputs.mPlayerDir) + " has no readable " + std::string(kPlayerInfoFileName) +
                    ", so it is not a player template" + install);
            if (info->value("editor", true))
                throw BuildError(PathText(inputs.mPlayerDir) + " is an editor build, not a player" + install);

            if (const std::optional<std::string> problem =
                    CheckOutputDir(request.mOutDir, { inputs.mSourceRoot, inputs.mPlayerDir }))
                throw BuildError(*problem);
            inputs.mOutDir = NormalizedPath(request.mOutDir);

            // The whole game folder, terrain releases included, lands under games/<name>/
            const std::size_t longest = std::max({ LongestPath(inputs.mPlayerDir),
                ("games/" + inputs.mDirName + "/").size() + LongestPath(inputs.mGameDir), kCookedPathLength });
            if (const std::optional<std::string> problem = CheckPathLength(inputs.mOutDir, longest))
                throw BuildError(*problem);

            return inputs;
        }

        bool CopyTemplate(const GameInputs &inputs, const fs::path &staging, BuildReport &report)
        {
            for (const fs::directory_entry &entry : fs::directory_iterator(inputs.mPlayerDir))
            {
                const std::string name = PathText(entry.path().filename());
                if (std::find(std::begin(kNotTemplate), std::end(kNotTemplate), name) != std::end(kNotTemplate))
                {
                    report.mWarnings.push_back("the player template holds " + name + ", which it should not; not copied");
                    continue;
                }
                Copy(entry.path(), staging / entry.path().filename());
            }
            return true;
        }

        std::size_t Cook(const GameInputs &inputs, const fs::path &staging, const fs::path &cooker)
        {
            const fs::path cooked = staging / "cooked";
            const fs::path resultFile = TempFile("cook");

            // The same id roots the CMake cook passes, so ids match the editor's
            std::vector<std::string> args = { "--source-dir", PathText(inputs.mSourceRoot / "assets"), "--id-root",
                "assets" };
            std::error_code ec;
            if (fs::is_directory(inputs.mProject.mAssetsDir, ec))
                args.insert(args.end(), { "--source-dir", PathText(inputs.mProject.mAssetsDir), "--id-root",
                    inputs.mProject.mAssetIdRoot });
            args.insert(args.end(), { "--out", PathText(cooked), "--result", PathText(resultFile) });

            const int exit = RunTool("AssetCooker", cooker, args, inputs.mSourceRoot);
            const std::string message = TakeMessage(resultFile, Json::json_pointer("/diagnostic/message"));
            if (exit != 0)
                throw BuildError("AssetCooker failed (exit " + std::to_string(exit) + ")" +
                    (message.empty() ? "" : ": " + message));

            const std::optional<AssetManifest> manifest = AssetManifest::LoadFile(cooked / kAssetManifestFileName);
            if (!manifest)
                throw BuildError("AssetCooker left no readable manifest in " + PathText(cooked));
            return manifest->Count();
        }

        bool CopyGameData(const GameInputs &inputs, const fs::path &gameOut, BuildReport &report)
        {
            const GameProject &project = inputs.mProject;
            std::error_code ec;

            Copy(inputs.mGameDir / kGameManifestFileName, gameOut / kGameManifestFileName);
            if (fs::is_regular_file(project.mProjectSettingsPath, ec))
                Copy(project.mProjectSettingsPath, gameOut / kProjectSettingsFileName);
            if (fs::is_regular_file(inputs.mGameDir / kTerrainStreamingSettingsFileName, ec))
                Copy(inputs.mGameDir / kTerrainStreamingSettingsFileName, gameOut / kTerrainStreamingSettingsFileName);

            if (fs::is_directory(project.mSceneDir, ec))
                Copy(project.mSceneDir, gameOut / project.mSceneDir.lexically_relative(inputs.mGameDir));
            else
                report.mWarnings.push_back(PathText(project.mSceneDir) + " does not exist, so the game starts empty");

            if (fs::is_directory(project.mAssetsDir, ec))
            {
                const fs::path assetsOut = gameOut / project.mAssetsDir.lexically_relative(inputs.mGameDir);
                // A player reads cooked Prefabs only
                for (const fs::directory_entry &entry : fs::directory_iterator(project.mAssetsDir))
                {
                    if (entry.path().filename() != "prefabs")
                        Copy(entry.path(), assetsOut / entry.path().filename());
                }
            }
            return true;
        }

        bool CopySoundBanks(const GameInputs &inputs, const fs::path &gameOut)
        {
            const GameProject &project = inputs.mProject;
            std::error_code ec;
            if (!fs::is_directory(project.mSoundBankDir, ec))
                return false;

            // Inside assetsRoot they came with the assets
            if (!IsWithin(project.mSoundBankDir, project.mAssetsDir))
                Copy(project.mSoundBankDir, gameOut / project.mSoundBankDir.lexically_relative(inputs.mGameDir));
            return true;
        }

        // Copies each recipe's current release, as the last engine build published it
        bool PackageTerrain(const GameInputs &inputs, const fs::path &gameOut, const fs::path &terrainTool)
        {
            constexpr std::string_view kRecipeSuffix = ".terrain.json";

            std::vector<fs::path> recipes;
            std::error_code ec;
            const fs::path recipeDir = inputs.mProject.mAssetsDir / "terrain";
            if (fs::is_directory(recipeDir, ec))
            {
                for (const fs::directory_entry &entry : fs::directory_iterator(recipeDir))
                {
                    const std::string name = PathText(entry.path().filename());
                    if (entry.is_regular_file() && name.size() > kRecipeSuffix.size() && name.ends_with(kRecipeSuffix))
                        recipes.push_back(entry.path());
                }
            }
            if (recipes.empty())
                return false;
            std::sort(recipes.begin(), recipes.end());

            for (const fs::path &recipe : recipes)
            {
                const std::string sourcePath = "terrain/" + PathText(recipe.filename());
                const fs::path reportFile = TempFile("terrain");
                const int exit = RunTool("TerrainTool", terrainTool,
                    { "package", "--id-root", inputs.mProject.mAssetIdRoot, "--source-path", sourcePath, "--game-dir",
                        PathText(inputs.mGameDir), "--out", PathText(gameOut), "--report", PathText(reportFile) },
                    inputs.mSourceRoot);
                const std::string message = TakeMessage(reportFile, Json::json_pointer("/diagnostics/0/message"));
                if (exit != 0)
                    throw BuildError("TerrainTool could not package " + sourcePath + " (exit " + std::to_string(exit) +
                        ")" + (message.empty() ? "" : ": " + message));
            }
            return true;
        }
    }

    BuildReport RunGameBuild(const BuildRequest &request)
    {
        BuildReport report;
        report.mOutDir = request.mOutDir;
        fs::path staging;

        try
        {
            GameInputs inputs;
            RunStep(report, "check", [&] {
                inputs = ResolveInputs(request);
                report.mGame = inputs.mDirName;
                report.mOutDir = inputs.mOutDir;
                return true;
            });

            const fs::path gameOut = StagingDir(inputs.mOutDir) / "games" / inputs.mDirName;
            RunStep(report, "template", [&] {
                staging = BeginStaging(inputs.mOutDir);
                return CopyTemplate(inputs, staging, report);
            });
            RunStep(report, "cook", [&] {
                report.mAssetCount = Cook(inputs, staging, request.mAssetCooker);
                return true;
            });
            RunStep(report, "game data", [&] { return CopyGameData(inputs, gameOut, report); });
            RunStep(report, "sound banks", [&] { return CopySoundBanks(inputs, gameOut); });
            RunStep(report, "terrain", [&] { return PackageTerrain(inputs, gameOut, request.mTerrainTool); });
            RunStep(report, "finish", [&] {
                report.mBytes = FolderBytes(staging);
                CommitStaging(staging, inputs.mOutDir);
                staging.clear();
                return true;
            });
            report.mOk = true;
        }
        catch (const SetupError &error)
        {
            report.mError = error.What() + ": " + error.Cause() + ". " + error.Fix();
        }
        catch (const std::exception &error)
        {
            report.mError = error.what();
        }

        if (!staging.empty())
            AbandonStaging(staging);
        return report;
    }

    std::string BuildReportJson(const BuildReport &report)
    {
        Json steps = Json::array();
        for (const BuildStep &step : report.mSteps)
            steps.push_back({ { "name", step.mName }, { "status", StatusText(step.mStatus) },
                { "seconds", step.mSeconds } });

        Json json = {
            { "version", 1 },
            { "status", report.mOk ? "succeeded" : "failed" },
            { "game", report.mGame },
            { "output", PathText(report.mOutDir) },
            { "steps", steps },
            { "warnings", report.mWarnings },
            { "assets", report.mAssetCount },
            { "bytes", report.mBytes },
        };
        if (!report.mOk)
            json["error"] = report.mError;
        return json.dump(2) + "\n";
    }
}

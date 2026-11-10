/**
 * @file GameBuild.cpp
 * @author Quy Dong Ta
 * @brief The build steps: check, game build, template, cook, game data, sound banks, terrain, finish.
 *        The output starts its game with no arguments, from <game name>.exe and engine.json.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "GameBuild.h"

#include "BuildOutput.h"
#include "GameBuildCompile.h"

#include <assets/AssetIdRoot.h>
#include <assets/AssetManifest.h>
#include <core/fs/FileIO.h>
#include <core/log/SetupError.h>
#include <core/platform/Process.h>
#include <project/GameProject.h>
#include <project/GameBuildInfo.h>
#include <project/ProjectSettings.h>
#include <serial/JsonFile.h>
#include <terrain/TerrainStreamingSettings.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace mir::build
{
    namespace fs = std::filesystem;
    using Json = nlohmann::json;

    namespace
    {
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
            fs::path mGameBuildDir;
            Json mGameBuildInfo;
            // project_settings.json's build.gameBuildOptions
            GameBuildChoices mChoices;
            // MjolnirEngine.exe's name in the build
            std::string mExeName;
            fs::path mOutDir;
            // below the output, without the template
            std::size_t mLongestGamePath = 0;
        };

        template <typename Work>
        void RunStep(BuildReport &report, const BuildRequest &request, std::string name, Work &&work)
        {
            if (request.mOnProgress)
                request.mOnProgress({ name });
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

        bool EqualsIgnoringCase(std::string_view a, std::string_view b)
        {
            return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
                return std::tolower(x) == std::tolower(y);
            });
        }

        // Why Windows would not take name.exe as a file name, or nullopt
        std::optional<std::string> ExeNameProblem(std::string_view name)
        {
            constexpr std::string_view kNotAllowed = R"(<>:"/\|?*)";
            if (name.empty())
                return "it is empty";
            for (const char c : name)
            {
                if (static_cast<unsigned char>(c) < 0x20 || kNotAllowed.find(c) != std::string_view::npos)
                    return "Windows does not allow <>:\"/\\|?* or control characters in a file name";
            }
            if (name.back() == '.' || name.back() == ' ')
                return "a file name cannot end in a dot or a space";

            // CON.exe and lpt1.anything.exe are devices
            const std::string_view device = name.substr(0, name.find('.'));
            for (std::string_view reserved : { "CON", "PRN", "AUX", "NUL" })
            {
                if (EqualsIgnoringCase(device, reserved))
                    return "Windows reserves it for a device";
            }
            if (device.size() == 4 && (EqualsIgnoringCase(device.substr(0, 3), "COM") ||
                    EqualsIgnoringCase(device.substr(0, 3), "LPT")) && device[3] >= '1' && device[3] <= '9')
                return "Windows reserves it for a device";
            return std::nullopt;
        }

        // Warns where the template was built differently from the game's choices, or the editor the game is made in
        void WarnAboutTemplate(const GameInputs &inputs, const fs::path &editorInfo, BuildReport &report)
        {
            std::error_code ec;
            const bool hasBanks = fs::is_directory(inputs.mProject.mSoundBankDir, ec) &&
                !fs::is_empty(inputs.mProject.mSoundBankDir, ec);
            const bool silent = hasBanks && GameBuildInfoFlag(inputs.mGameBuildInfo, "audio") == false;
            if (silent)
                report.mWarnings.push_back("the game has SoundBanks, but the game build template was built with "
                                           "MIR_WITH_AUDIO OFF, so the packaged game plays no sound");

            // the choices are compared even without the editor's
            Json editor;
            if (!editorInfo.empty())
            {
                if (std::optional<Json> read = ReadJsonFile(editorInfo); read && read->is_object())
                    editor = std::move(*read);
                else
                    report.mWarnings.push_back("could not read the editor's " + PathText(editorInfo) +
                        ", so the game build template was not compared with it");
            }

            for (const GameBuildInfoMismatch &mismatch :
                CompareGameBuildInfo(inputs.mGameBuildInfo, editor, inputs.mChoices))
            {
                if (!(silent && mismatch.mKey == "audio"))
                    report.mWarnings.push_back(mismatch.mMessage);
            }
        }

        // The game and the output folder, which need no template
        GameInputs ResolveGame(const BuildRequest &request)
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
            inputs.mChoices = ReadGameBuildChoices(inputs.mProject.mProjectSettingsPath);

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

            for (const ProjectScene &scene : inputs.mProject.mScenes)
            {
                if (!IsWithin(scene.mDirectory, inputs.mGameDir))
                    throw BuildError("scene " + scene.mId + " is outside the game folder");
                if (manifest && manifest->contains("scenes") &&
                    !fs::is_regular_file(scene.mDirectory / "scene.json", ec))
                    throw BuildError("scene " + scene.mId + " has no scene.json in " + PathText(scene.mDirectory));
            }

            const std::string &name = inputs.mProject.mName;
            if (const std::optional<std::string> problem = ExeNameProblem(name))
                throw BuildError("game.json's name \"" + name + "\" cannot name the exe: " + *problem +
                    "; change it in game.json");
            inputs.mExeName = name + ".exe";

            inputs.mGameBuildDir = NormalizedPath(request.mGameBuildDir);
            if (const std::optional<std::string> problem =
                    CheckOutputDir(request.mOutDir, { inputs.mSourceRoot, inputs.mGameBuildDir }))
                throw BuildError(*problem);
            inputs.mOutDir = NormalizedPath(request.mOutDir);
            // the game from the last build, started by Build and Run or a double-click, would fail the swap at the end
            if (const std::optional<std::string> problem = LockedBuildProblem(inputs.mOutDir, "the previous build"))
                throw BuildError(*problem);

            // The whole game folder, terrain releases included, lands under games/<name>/
            inputs.mLongestGamePath = std::max({ inputs.mExeName.size(),
                ("games/" + inputs.mDirName + "/").size() + LongestPath(inputs.mGameDir), kCookedPathLength });
            if (const std::optional<std::string> problem = CheckPathLength(inputs.mOutDir, inputs.mLongestGamePath))
                throw BuildError(*problem);

            return inputs;
        }

        // In check, or on what the game build step installed
        void CheckTemplate(GameInputs &inputs, const BuildRequest &request, BuildReport &report)
        {
            if (const std::optional<std::string> problem = GameBuildTemplateProblem(inputs.mGameBuildDir, &inputs.mGameBuildInfo))
                throw BuildError(*problem + "; compile one with --game-build-preset windows-msvc-game-release, or install "
                                            "one with cmake --install builds/windows-msvc-game-release --prefix <dir>");

            for (const fs::directory_entry &entry : fs::directory_iterator(inputs.mGameBuildDir))
            {
                const std::string file = PathText(entry.path().filename());
                if (file != kGameBuildExecutableFileName && EqualsIgnoringCase(file, inputs.mExeName))
                    throw BuildError("game.json's name \"" + inputs.mProject.mName + "\" would make " +
                        inputs.mExeName + ", which the game build template already holds; change it in game.json");
            }

            const std::size_t longest = std::max(LongestPath(inputs.mGameBuildDir), inputs.mLongestGamePath);
            if (const std::optional<std::string> problem = CheckPathLength(inputs.mOutDir, longest))
                throw BuildError(*problem);

            WarnAboutTemplate(inputs, request.mEditorInfo, report);
        }

        bool CopyTemplate(const GameInputs &inputs, const fs::path &staging, BuildReport &report)
        {
            for (const fs::directory_entry &entry : fs::directory_iterator(inputs.mGameBuildDir))
            {
                const std::string name = PathText(entry.path().filename());
                if (std::find(std::begin(kNotTemplate), std::end(kNotTemplate), name) != std::end(kNotTemplate))
                {
                    report.mWarnings.push_back("the game build template holds " + name + ", which it should not; not copied");
                    continue;
                }
                Copy(entry.path(), staging / (name == kGameBuildExecutableFileName ? inputs.mExeName : name));
            }
            return true;
        }

        // With no --game, the game build starts the game engine.json names
        void WriteEngineManifest(const GameInputs &inputs, const fs::path &staging)
        {
            const Json manifest = { { "defaultGame", "games/" + inputs.mDirName } };
            if (!WriteText(staging / kEngineManifestFileName, manifest.dump(4) + "\n"))
                throw BuildError("could not write " + PathText(staging / kEngineManifestFileName));
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
            fs::create_directories(gameOut, ec);
            if (ec)
                throw BuildError("could not create packaged game directory " + PathText(gameOut) + ": " + ec.message());

            Json manifest = ReadJsonFile(inputs.mGameDir / kGameManifestFileName).value();
            if (manifest.contains("scenes") || fs::is_regular_file(project.mSceneDir / "scene.json", ec))
            {
                manifest["startupScene"] = project.mStartupSceneId;
                manifest["scenes"] = Json::array();
                for (const ProjectScene &scene : project.mScenes)
                    manifest["scenes"].push_back({ { "id", scene.mId },
                        { "path", PathText(scene.mDirectory.lexically_relative(inputs.mGameDir)) } });
                manifest["sceneDir"] = PathText(project.mSceneDir.lexically_relative(inputs.mGameDir));
            }
            if (!WriteText(gameOut / kGameManifestFileName, manifest.dump(2) + "\n"))
                throw BuildError("could not write packaged game.json");
            if (std::optional<Json> settings = ReadProjectSettings(project.mProjectSettingsPath))
            {
                for (std::string_view section : kMachineSettingsSections)
                    settings->erase(std::string(section));
                if (!WriteText(gameOut / kProjectSettingsFileName, settings->dump(2) + "\n"))
                    throw BuildError("could not write " + PathText(gameOut / kProjectSettingsFileName));
            }
            if (fs::is_regular_file(inputs.mGameDir / kTerrainStreamingSettingsFileName, ec))
                Copy(inputs.mGameDir / kTerrainStreamingSettingsFileName, gameOut / kTerrainStreamingSettingsFileName);

            for (const ProjectScene &scene : project.mScenes)
            {
                if (fs::is_directory(scene.mDirectory, ec))
                    Copy(scene.mDirectory, gameOut / scene.mDirectory.lexically_relative(inputs.mGameDir));
                else
                    report.mWarnings.push_back(PathText(scene.mDirectory) + " does not exist, so the game starts empty");
            }

            if (fs::is_directory(project.mAssetsDir, ec))
            {
                const fs::path assetsOut = gameOut / project.mAssetsDir.lexically_relative(inputs.mGameDir);
                // A game build reads cooked Prefabs only
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
            GameBuildCompile gameBuild;
            const bool compile = !request.mGameBuildPreset.empty();
            RunStep(report, request, "check", [&] {
                inputs = ResolveGame(request);
                report.mGame = inputs.mDirName;
                report.mOutDir = inputs.mOutDir;
                report.mWarnings.insert(
                    report.mWarnings.end(), inputs.mChoices.mProblems.begin(), inputs.mChoices.mProblems.end());
                if (!compile)
                {
                    CheckTemplate(inputs, request, report);
                    return true;
                }

                // A bad output or template folder fails here, not after a compile
                gameBuild = { inputs.mSourceRoot, request.mGameBuildPreset, inputs.mGameBuildDir, request.mEditorInfo,
                    inputs.mChoices };
                if (const std::optional<std::string> problem = CheckGameBuildCompile(gameBuild, inputs.mOutDir))
                    throw BuildError(*problem);
                return true;
            });
            RunStep(report, request, "game build", [&] {
                if (!compile)
                    return false;
                CompileGameBuild(gameBuild, [&](const CompileProgress &progress) {
                    if (request.mOnProgress)
                        request.mOnProgress({ "game build", progress.mPhase, progress.mDone, progress.mTotal });
                });
                return true;
            });

            const fs::path gameOut = StagingDir(inputs.mOutDir) / "games" / inputs.mDirName;
            RunStep(report, request, "template", [&] {
                if (compile)
                    CheckTemplate(inputs, request, report);
                staging = BeginStaging(inputs.mOutDir);
                return CopyTemplate(inputs, staging, report);
            });
            RunStep(report, request, "cook", [&] {
                report.mAssetCount = Cook(inputs, staging, request.mAssetCooker);
                return true;
            });
            RunStep(report, request, "game data", [&] {
                WriteEngineManifest(inputs, staging);
                return CopyGameData(inputs, gameOut, report);
            });
            RunStep(report, request, "sound banks", [&] { return CopySoundBanks(inputs, gameOut); });
            RunStep(report, request, "terrain", [&] { return PackageTerrain(inputs, gameOut, request.mTerrainTool); });
            RunStep(report, request, "finish", [&] {
                report.mBytes = FolderBytes(staging);
                CommitStaging(staging, inputs.mOutDir);
                staging.clear();
                report.mExecutable = inputs.mOutDir / inputs.mExeName;
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
        if (report.mOk)
            json["executable"] = PathText(report.mExecutable);
        else
            json["error"] = report.mError;
        return json.dump(2) + "\n";
    }

    std::string BuildProgressJson(const BuildProgress &progress)
    {
        // ordered, so the file reads as documented
        nlohmann::ordered_json json = { { "version", 1 }, { "step", progress.mStep } };
        if (!progress.mPhase.empty())
            json["phase"] = progress.mPhase;
        if (progress.mTotal > 0)
        {
            json["done"] = progress.mDone;
            json["total"] = progress.mTotal;
        }
        return json.dump() + "\n";
    }
}

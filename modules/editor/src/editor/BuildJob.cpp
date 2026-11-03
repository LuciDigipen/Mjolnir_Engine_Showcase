/**
 * @file BuildJob.cpp
 * @author Quy Dong Ta
 * @brief File > Build Settings' Build: MjolnirBuild run in the background, and what it reported.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/BuildJob.h>

#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

#include <string_view>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        namespace fs = std::filesystem;
        using Json = nlohmann::json;

        // MjolnirBuild's names: it marks every build with this file, and moves the previous
        // build to <out>.old while the new one moves in
        constexpr std::string_view kBuildMarkerFileName = "mjolnir_build.json";
        constexpr std::string_view kPreviousSuffix = ".old";

        std::string PathText(const fs::path &path)
        {
            const std::u8string text = path.u8string();
            return { reinterpret_cast<const char *>(text.data()), text.size() };
        }

        fs::path FromText(const std::string &text)
        {
            return fs::path(std::u8string(text.begin(), text.end()));
        }

        std::string Text(const Json &json, std::string_view key)
        {
            const auto it = json.find(std::string(key));
            return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
        }

        template <typename T>
        T Number(const Json &json, std::string_view key)
        {
            const auto it = json.find(std::string(key));
            return it != json.end() && it->is_number() ? it->get<T>() : T{};
        }

        // MjolnirBuild writes a new marker into every build, so a different time means a different build
        std::optional<fs::file_time_type> MarkerTime(const fs::path &outDir)
        {
            std::error_code ec;
            const fs::file_time_type time = fs::last_write_time(outDir / kBuildMarkerFileName, ec);
            return ec ? std::nullopt : std::optional(time);
        }

        void Log(const BuildResult &result)
        {
            for (const BuildStepResult &step : result.mSteps)
                MIR_LOG_INFO("Build: {:<11} {:<9} {:6.2f} s", step.mName, step.mStatus, step.mSeconds);
            for (const std::string &warning : result.mWarnings)
                MIR_LOG_WARN("Build: {}", warning);

            switch (result.mOutcome)
            {
            case BuildOutcome::Succeeded:
                MIR_LOG_INFO("Build: built {} in {:.1f} s ({} assets, {:.1f} MB)", PathText(result.mExecutable),
                    result.mSeconds, result.mAssets, static_cast<double>(result.mBytes) / 1e6);
                break;
            case BuildOutcome::Failed:
                MIR_LOG_ERROR("Build failed: {}", result.mError);
                break;
            case BuildOutcome::Canceled:
                MIR_LOG_INFO("Build canceled; {} is as it was", PathText(result.mOutDir));
                break;
            case BuildOutcome::CanceledTooLate:
                MIR_LOG_WARN("Build: too late to cancel; the new build is already in {}", PathText(result.mOutDir));
                break;
            }
        }
    }

    fs::path BuildToolPath()
    {
        const std::optional<Json> binding = ReadJsonFile(ExecutableDir() / "asset_workspace.json");
        if (!binding || !binding->is_object())
            return {};
        const std::string tool = Text(*binding, "buildTool");
        return tool.empty() ? fs::path() : FromText(tool);
    }

    std::vector<std::string> BuildArguments(const BuildJobRequest &request)
    {
        return { "--game", PathText(request.mGameDir), "--player", PathText(request.mPlayerDir), "--out",
            PathText(request.mOutDir), "--result", PathText(request.mResultFile) };
    }

    BuildResult ReadBuildResult(const fs::path &resultFile, int exitCode)
    {
        BuildResult result;
        const std::optional<Json> report = ReadJsonFile(resultFile);
        if (!report || !report->is_object())
        {
            result.mError = "MjolnirBuild exited with code " + std::to_string(exitCode) + " and wrote no report";
            return result;
        }

        // the report is MjolnirBuild's last word, so it decides even when the exit code was lost
        result.mOutcome = Text(*report, "status") == "succeeded" ? BuildOutcome::Succeeded : BuildOutcome::Failed;
        result.mError = Text(*report, "error");
        if (result.mOutcome == BuildOutcome::Failed && result.mError.empty())
            result.mError = "MjolnirBuild exited with code " + std::to_string(exitCode);
        result.mOutDir = FromText(Text(*report, "output"));
        result.mExecutable = FromText(Text(*report, "executable"));
        result.mAssets = Number<std::size_t>(*report, "assets");
        result.mBytes = Number<std::uintmax_t>(*report, "bytes");

        if (const auto warnings = report->find("warnings"); warnings != report->end() && warnings->is_array())
        {
            for (const Json &warning : *warnings)
            {
                if (warning.is_string())
                    result.mWarnings.push_back(warning.get<std::string>());
            }
        }
        if (const auto steps = report->find("steps"); steps != report->end() && steps->is_array())
        {
            for (const Json &step : *steps)
            {
                if (step.is_object())
                    result.mSteps.push_back({ Text(step, "name"), Text(step, "status"), Number<double>(step, "seconds") });
            }
        }
        return result;
    }

    void RestorePreviousBuild(const fs::path &outDir)
    {
        fs::path out = outDir;
        if (!out.has_filename())
            out = out.parent_path();
        fs::path previous = out;
        previous += kPreviousSuffix;

        std::error_code ec;
        if (fs::exists(out, ec) || !fs::is_regular_file(previous / kBuildMarkerFileName, ec))
            return;
        fs::rename(previous, out, ec);
        if (ec)
            MIR_LOG_ERROR("Build: could not move the previous build {} back to {}: {}", PathText(previous),
                PathText(out), ec.message());
    }

    BuildJob::~BuildJob() { Cancel(); }

    bool BuildJob::Start(BuildJobRequest request)
    {
        if (mRunning)
            return false;

        mRequest = std::move(request);
        mStarted = std::chrono::steady_clock::now();
        std::error_code ec;
        fs::remove(mRequest.mResultFile, ec);
        mPreviousMarker = MarkerTime(mRequest.mOutDir);

        if (mRequest.mTool.empty() || !mProcess.Start(mRequest.mTool, BuildArguments(mRequest)))
        {
            Fail(mRequest.mTool.empty() ? "the editor does not know where MjolnirBuild is; rebuild the editor"
                                        : "could not start MjolnirBuild at " + PathText(mRequest.mTool));
            return false;
        }

        mRunning = true;
        mResult.reset();
        MIR_LOG_INFO("Build: packaging {} into {}", PathText(mRequest.mGameDir), PathText(mRequest.mOutDir));
        return true;
    }

    void BuildJob::Update()
    {
        if (!mRunning)
            return;
        const std::optional<int> exitCode = mProcess.Poll();
        if (!exitCode)
            return;

        BuildResult result = ReadBuildResult(mRequest.mResultFile, *exitCode);
        std::error_code ec;
        fs::remove(mRequest.mResultFile, ec);
        Finish(std::move(result));
    }

    void BuildJob::Cancel()
    {
        if (!mRunning)
            return;

        mProcess.Terminate();
        // a report means it finished before it was stopped
        if (ReadJsonFile(mRequest.mResultFile))
        {
            Update();
            return;
        }

        RestorePreviousBuild(mRequest.mOutDir);
        BuildResult result;
        result.mOutcome = BuildOutcome::Canceled;
        result.mOutDir = mRequest.mOutDir;

        // stopped between the swap and the report
        const std::optional<fs::file_time_type> marker = MarkerTime(mRequest.mOutDir);
        if (marker && marker != mPreviousMarker)
            result.mOutcome = BuildOutcome::CanceledTooLate;
        else if (!marker && mPreviousMarker)
        {
            result.mOutcome = BuildOutcome::Failed;
            result.mError = "Cancel stopped MjolnirBuild while it swapped builds, and the previous build could not be "
                            "moved back into " + PathText(mRequest.mOutDir) + "; see the log";
        }
        Finish(std::move(result));
    }

    void BuildJob::Fail(std::string error)
    {
        if (mRunning)
            return;
        mStarted = std::chrono::steady_clock::now();
        BuildResult result;
        result.mError = std::move(error);
        Finish(std::move(result));
    }

    double BuildJob::Seconds() const
    {
        if (!mRunning)
            return mResult ? mResult->mSeconds : 0.0;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - mStarted).count();
    }

    void BuildJob::Finish(BuildResult result)
    {
        result.mSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - mStarted).count();
        mRunning = false;
        Log(result);
        mResult = std::move(result);
    }
}

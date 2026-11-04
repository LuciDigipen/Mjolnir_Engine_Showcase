/**
 * @file BuildJob.h
 * @author Quy Dong Ta
 * @brief File > Build Settings' Build: MjolnirBuild run in the background, and what it reported.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/platform/Process.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mir
{
    enum class BuildOutcome
    {
        Succeeded,
        Failed,
        Canceled,
        // Cancel came after the new build moved into the output, before MjolnirBuild wrote its report
        CanceledTooLate,
    };

    struct BuildStepResult
    {
        std::string mName;
        // "succeeded", "skipped" or "failed"
        std::string mStatus;
        double mSeconds = 0.0;
    };

    struct BuildResult
    {
        BuildOutcome mOutcome = BuildOutcome::Failed;
        std::string mError;
        std::vector<std::string> mWarnings;
        std::vector<BuildStepResult> mSteps;
        std::filesystem::path mOutDir;
        // set when the build succeeded
        std::filesystem::path mExecutable;
        std::size_t mAssets = 0;
        std::uintmax_t mBytes = 0;
        double mSeconds = 0.0;
    };

    struct BuildJobRequest
    {
        // MjolnirBuild
        std::filesystem::path mTool;
        std::filesystem::path mGameDir;
        std::filesystem::path mPlayerDir;
        std::filesystem::path mOutDir;
        // where MjolnirBuild writes its report, outside mOutDir
        std::filesystem::path mResultFile;
    };

    // What the Build Settings window shows of the build, made each frame by EditorModule
    struct BuildJobView
    {
        bool mRunning = false;
        // Save was chosen; the build starts once the scene is written
        bool mSaving = false;
        double mSeconds = 0.0;
        
        std::string mBlocked;
        // set while the Save or Cancel prompt is up
        std::string mAskSave;
        const BuildResult *mResult = nullptr;
    };

    // MjolnirBuild beside the editor, from asset_workspace.json; empty when it names none
    std::filesystem::path BuildToolPath();

    std::vector<std::string> BuildArguments(const BuildJobRequest &request);

    // What MjolnirBuild reported in @p resultFile, or why there is no report when it exited with @p exitCode
    BuildResult ReadBuildResult(const std::filesystem::path &resultFile, int exitCode);

    // After MjolnirBuild was stopped while it swapped the new build in: moves the previous one back
    void RestorePreviousBuild(const std::filesystem::path &outDir);

    // One build at a time, checked each frame; nothing here waits for MjolnirBuild
    class BuildJob
    {
    public:
        BuildJob() = default;
        ~BuildJob();

        BuildJob(const BuildJob &) = delete;
        BuildJob &operator=(const BuildJob &) = delete;

        // false when MjolnirBuild could not start, with Result() saying why
        bool Start(BuildJobRequest request);

       
        void Update();

        
        void Cancel();

        // a build that never started, such as one whose save failed
        void Fail(std::string error);

        bool Running() const { return mRunning; }
        double Seconds() const;

        // the last build, empty before the first
        const std::optional<BuildResult> &Result() const { return mResult; }

    private:
        void Finish(BuildResult result);

        ChildProcess mProcess;
        BuildJobRequest mRequest;
        std::chrono::steady_clock::time_point mStarted;
        // the output's marker before the build, so Cancel can tell whether the new build moved in
        std::optional<std::filesystem::file_time_type> mPreviousMarker;
        bool mRunning = false;
        std::optional<BuildResult> mResult;
    };
}

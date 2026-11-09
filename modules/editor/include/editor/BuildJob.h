/**
 * @file BuildJob.h
 * @author Quy Dong Ta
 * @brief File > Build and Build Settings' Build: MjolnirBuild run in the background, the game started once it is
 *        built, and what it reported.
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
#include <string_view>
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
        // a build asked to run the game: it started, or why not
        bool mStarted = false;
        std::string mStartError;
    };

    // What the editor's toast says when a build ends
    struct BuildNotice
    {
        enum class Kind
        {
            Info,
            Success,
            Warning,
            Failure,
        };
        Kind mKind = Kind::Info;
        std::string mText;
        // the exe to show in Explorer, empty when there is none
        std::filesystem::path mExecutable;
        // the whole message and the Log are in Build Settings
        bool mOpenSettings = false;
        // stays until it is closed, rather than fading
        bool mStays = false;
    };

    BuildNotice BuildNoticeFor(const BuildResult &result, std::string_view game);

    struct BuildJobRequest
    {
        // MjolnirBuild
        std::filesystem::path mTool;
        std::filesystem::path mGameDir;
        std::filesystem::path mGameBuildDir;
        std::filesystem::path mOutDir;
        // where MjolnirBuild writes its report, outside mOutDir
        std::filesystem::path mResultFile;
        // compiles the game build into mGameBuildDir with this CMake preset first, empty uses the template as it is
        std::string mGameBuildPreset;
        // MjolnirBuild rewrites it as each step starts, beside mResultFile
        std::filesystem::path mProgressFile;
        // starts the built game, detached, once the build succeeds
        bool mRun = false;
    };

    // What MjolnirBuild's --progress file says
    struct BuildJobProgress
    {
        std::string mStep;
        // in the game build step: environment, configure, compile or install
        std::string mPhase;
        // Ninja's count, once it has printed one
        std::size_t mDone = 0;
        std::size_t mTotal = 0;
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
        BuildJobProgress mProgress;
        // how many builds have ended, so a new result is noticed
        std::uint64_t mFinished = 0;
    };

    // MjolnirBuild beside the editor, from asset_workspace.json; empty when it names none
    std::filesystem::path BuildToolPath();

    std::vector<std::string> BuildArguments(const BuildJobRequest &request);

    // What MjolnirBuild reported in @p resultFile, or why there is no report when it exited with @p exitCode
    BuildResult ReadBuildResult(const std::filesystem::path &resultFile, int exitCode);

    // nullopt while there is no file, or none with a step
    std::optional<BuildJobProgress> ReadBuildProgress(const std::filesystem::path &progressFile);

    // The running line's text, such as "Compiling the game build: 412 of 1310"
    std::string BuildProgressText(const BuildJobProgress &progress);

    // After MjolnirBuild was stopped while it swapped the new build in: moves the previous one back
    void RestorePreviousBuild(const std::filesystem::path &outDir);

    // After MjolnirBuild was stopped while a compile replaced the game build template: moves the previous one back.
    // Why it could not, or nullopt
    std::optional<std::string> RestorePreviousGameBuild(const std::filesystem::path &gameBuildDir);

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

        // the running build's, read from its progress file a few times a second
        const BuildJobProgress &Progress() const { return mProgress; }

        std::uint64_t Finished() const { return mFinished; }

    private:
        void Finish(BuildResult result);

        ChildProcess mProcess;
        BuildJobRequest mRequest;
        std::chrono::steady_clock::time_point mStarted;
        std::chrono::steady_clock::time_point mProgressReadAt;
        BuildJobProgress mProgress;
        // the output's marker before the build, so Cancel can tell whether the new build moved in
        std::optional<std::filesystem::file_time_type> mPreviousMarker;
        bool mRunning = false;
        std::optional<BuildResult> mResult;
        std::uint64_t mFinished = 0;
    };
}

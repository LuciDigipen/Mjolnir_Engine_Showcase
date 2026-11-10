/**
 * @file BuildJob.tests.cpp
 * @author Quy Dong Ta
 * @brief File > Build Settings' Build: the real MjolnirBuild run in the background, canceled, and its report read.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/BuildJob.h>

#include <core/fs/FileIO.h>
#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace mir;

namespace
{
    namespace fs = std::filesystem;

#if defined(_WIN32)
    // Stops what a test left running from dir, such as the game Build and Run started, so its sandbox can go
    struct StopRunningFrom
    {
        fs::path mDir;

        ~StopRunningFrom()
        {
            for (const RunningProcess &process : ProcessesRunningFrom(mDir))
            {
                if (HANDLE handle = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, process.mId))
                {
                    TerminateProcess(handle, 1);
                    WaitForSingleObject(handle, 5000);
                    CloseHandle(handle);
                }
            }
        }
    };
#endif

    // A checkout with one engine asset and a game, a game build template, and room for the output
    struct BuildSandbox
    {
        fs::path mRoot;

        BuildSandbox()
        {
            static int sequence = 0;
            mRoot = fs::temp_directory_path() /
                ("build-job-tests-" + std::to_string(CurrentProcessId()) + "-" + std::to_string(sequence++));
            fs::remove_all(mRoot);
            Write(Checkout() / "assets" / "engine.txt", "x");
            Write(Game() / "game.json", R"({ "name": "Demo" })");
            Write(Game() / "scenes" / "default" / "scene.json", R"({ "entities": [] })");
            Write(GameBuildDir() / "MjolnirEngine.exe", "game-build");
            Write(GameBuildDir() / "game_build_info.json", R"({ "version": 1, "editor": false })");
        }

        ~BuildSandbox()
        {
            std::error_code ec;
            fs::remove_all(mRoot, ec);
        }

        BuildSandbox(const BuildSandbox &) = delete;
        BuildSandbox &operator=(const BuildSandbox &) = delete;

        fs::path Checkout() const { return mRoot / "checkout"; }
        fs::path Game() const { return Checkout() / "games" / "Demo"; }
        fs::path GameBuildDir() const { return mRoot / "game-build"; }
        fs::path Out() const { return mRoot / "out"; }

        BuildJobRequest Request() const
        {
            return BuildJobRequest{ MIR_EDITOR_BUILD_TOOL, Game(), GameBuildDir(), Out(), mRoot / "result.json", {},
                mRoot / "progress.json" };
        }

        static void Write(const fs::path &file, const std::string &text)
        {
            fs::create_directories(file.parent_path());
            REQUIRE(WriteText(file, text));
        }
    };

    // Updates once a frame, as EditorModule does, until the build ends
    void RunToEnd(BuildJob &job)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        while (job.Running() && std::chrono::steady_clock::now() < deadline)
        {
            job.Update();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        REQUIRE_FALSE(job.Running());
        REQUIRE(job.Result());
    }

    // BuildProbe in place of MjolnirBuild, stopping at @p phase
    BuildJobRequest ProbeRequest(const BuildSandbox &sandbox, std::string_view phase)
    {
        BuildJobRequest request = sandbox.Request();
        request.mTool = MIR_EDITOR_BUILD_PROBE;
        request.mGameDir = sandbox.mRoot / phase;
        return request;
    }

    template <typename Reached>
    void WaitUntil(Reached reached)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!reached() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(reached());
    }

    void WritePreviousBuild(const BuildSandbox &sandbox)
    {
        BuildSandbox::Write(sandbox.Out() / "mjolnir_build.json", R"({ "version": 1 })");
        BuildSandbox::Write(sandbox.Out() / "Demo.exe", "previous");
        // made by an earlier build, so its marker is older than any this build writes
        fs::last_write_time(sandbox.Out() / "mjolnir_build.json",
            fs::file_time_type::clock::now() - std::chrono::hours(1));
    }

    fs::path PreviousDir(const BuildSandbox &sandbox)
    {
        fs::path previous = sandbox.Out();
        previous += ".old";
        return previous;
    }
}

TEST_CASE("A build runs in the background and its report is read", "[build-job]")
{
    BuildSandbox sandbox;
    BuildJob job;
    REQUIRE(job.Start(sandbox.Request()));

    // Start returns at once, and only Update notices the end
    CHECK(job.Running());
    RunToEnd(job);

    const BuildResult &result = *job.Result();
    INFO(result.mError);
    REQUIRE(result.mOutcome == BuildOutcome::Succeeded);
    CHECK(result.mExecutable.filename() == "Demo.exe");
    CHECK(fs::is_regular_file(result.mExecutable));
    CHECK(fs::equivalent(result.mOutDir, sandbox.Out()));
    CHECK(result.mAssets > 0);
    CHECK(result.mBytes > 0);
    CHECK(std::ranges::any_of(result.mSteps,
        [](const BuildStepResult &step) { return step.mName == "cook" && step.mStatus == "succeeded"; }));
    CHECK_FALSE(fs::exists(sandbox.Request().mResultFile));
    CHECK_FALSE(fs::exists(sandbox.Request().mProgressFile));
    CHECK(job.Progress().mStep.empty());
    // a plain Build does not try to run it
    CHECK_FALSE(result.mStarted);
    CHECK(result.mStartError.empty());
}

TEST_CASE("A build that compiles the game build passes the preset on, and MjolnirBuild's check stops a bad one",
    "[build-job]")
{
    BuildSandbox sandbox;
    BuildJobRequest request = sandbox.Request();
    request.mGameBuildPreset = "windows-msvc-game-release";

    BuildJob job;
    REQUIRE(job.Start(request));
    RunToEnd(job);

    // the sandbox checkout has no CMakePresets.json, so nothing compiles and the template is untouched
    const BuildResult &result = *job.Result();
    CHECK(result.mOutcome == BuildOutcome::Failed);
    CHECK(result.mError.find("has no readable CMakePresets.json") != std::string::npos);
    CHECK(ReadText(sandbox.GameBuildDir() / "MjolnirEngine.exe") == "game-build");
    CHECK_FALSE(fs::exists(request.mProgressFile));
}

TEST_CASE("Progress is read while the build runs, and cleared with its file when it ends", "[build-job]")
{
    BuildSandbox sandbox;
    const BuildJobRequest request = ProbeRequest(sandbox, "progress");

    BuildJob job;
    REQUIRE(job.Start(request));
    CHECK(job.Progress().mStep.empty());
    WaitUntil([&] {
        job.Update();
        return job.Progress().mDone == 2;
    });
    CHECK(job.Progress().mStep == "game build");
    CHECK(job.Progress().mPhase == "compile");
    CHECK(job.Progress().mTotal == 5);

    job.Cancel();
    CHECK(job.Progress().mStep.empty());
    CHECK_FALSE(fs::exists(request.mProgressFile));
}

TEST_CASE("The progress file gives the step, phase and count, and nothing until it names a step", "[build-job]")
{
    BuildSandbox sandbox;
    const fs::path file = sandbox.mRoot / "progress.json";
    CHECK_FALSE(ReadBuildProgress(file));

    BuildSandbox::Write(file, R"({"version":1,"step":"game build","phase":"compile","done":412,"total":1310})");
    const std::optional<BuildJobProgress> compiling = ReadBuildProgress(file);
    REQUIRE(compiling);
    CHECK(compiling->mStep == "game build");
    CHECK(compiling->mPhase == "compile");
    CHECK(compiling->mDone == 412);
    CHECK(compiling->mTotal == 1310);

    BuildSandbox::Write(file, R"({"version":1,"step":"cook"})");
    const std::optional<BuildJobProgress> cooking = ReadBuildProgress(file);
    REQUIRE(cooking);
    CHECK(cooking->mStep == "cook");
    CHECK(cooking->mPhase.empty());
    CHECK(cooking->mTotal == 0);

    for (const std::string text : { std::string(), std::string(R"({"version":1,"st)"), std::string("[ 1 ]"),
             std::string(R"({"version":1})") })
    {
        INFO(text);
        BuildSandbox::Write(file, text);
        CHECK_FALSE(ReadBuildProgress(file));
    }
}

TEST_CASE("The running line names each step, and the compile's phase and count", "[build-job]")
{
    const auto text = [](std::string step, std::string phase = {}, std::size_t done = 0, std::size_t total = 0) {
        return BuildProgressText({ std::move(step), std::move(phase), done, total });
    };
    CHECK(text("check") == "Checking the game and folders");
    CHECK(text("game build") == "Compiling the game build");
    CHECK(text("game build", "environment") == "Compiling the game build: loading Visual Studio");
    CHECK(text("game build", "configure") == "Compiling the game build: configuring");
    CHECK(text("game build", "compile") == "Compiling the game build");
    CHECK(text("game build", "compile", 412, 1310) == "Compiling the game build: 412 of 1310");
    CHECK(text("game build", "install") == "Compiling the game build: installing");
    CHECK(text("template") == "Copying the game build template");
    CHECK(text("cook") == "Cooking assets");
    CHECK(text("game data") == "Copying the game data");
    CHECK(text("sound banks") == "Copying SoundBanks");
    CHECK(text("terrain") == "Packaging terrain");
    CHECK(text("finish") == "Moving the build into place");
    CHECK(text("a later step") == "a later step");
}

TEST_CASE("Cancel stops the build and keeps the previous one", "[build-job]")
{
    BuildSandbox sandbox;
    BuildSandbox::Write(sandbox.Out() / "mjolnir_build.json", R"({ "version": 1 })");
    BuildSandbox::Write(sandbox.Out() / "Demo.exe", "previous");

    BuildJob job;
    REQUIRE(job.Start(sandbox.Request()));
    job.Cancel();

    CHECK_FALSE(job.Running());
    REQUIRE(job.Result());
    CHECK(job.Result()->mOutcome == BuildOutcome::Canceled);
    CHECK(ReadText(sandbox.Out() / "Demo.exe") == "previous");

    // and the next build replaces it
    REQUIRE(job.Start(sandbox.Request()));
    RunToEnd(job);
    INFO(job.Result()->mError);
    CHECK(job.Result()->mOutcome == BuildOutcome::Succeeded);
    CHECK(ReadText(sandbox.Out() / "Demo.exe") == "game-build");
}

TEST_CASE("Cancel after the new build moved in says the new build is there", "[build-job]")
{
    BuildSandbox sandbox;
    SECTION("over a previous build") { WritePreviousBuild(sandbox); }
    SECTION("with no previous build") {}

    BuildJob job;
    REQUIRE(job.Start(ProbeRequest(sandbox, "moved-in")));
    WaitUntil([&] { return ReadText(sandbox.Out() / "Demo.exe") == "new"; });
    job.Cancel();

    REQUIRE(job.Result());
    CHECK(job.Result()->mOutcome == BuildOutcome::CanceledTooLate);
    CHECK(job.Result()->mOutDir == sandbox.Out());
    CHECK(ReadText(sandbox.Out() / "Demo.exe") == "new");
}

TEST_CASE("Cancel mid-swap puts the previous build back, or says it could not", "[build-job]")
{
    BuildSandbox sandbox;
    WritePreviousBuild(sandbox);
    const fs::path previous = PreviousDir(sandbox);

    BuildJob job;
    REQUIRE(job.Start(ProbeRequest(sandbox, "mid-swap")));
    WaitUntil([&] { return fs::exists(previous) && !fs::exists(sandbox.Out()); });

    SECTION("put back")
    {
        job.Cancel();
        REQUIRE(job.Result());
        CHECK(job.Result()->mOutcome == BuildOutcome::Canceled);
        CHECK(ReadText(sandbox.Out() / "Demo.exe") == "previous");
    }
    SECTION("held open, so it cannot move")
    {
        std::ifstream held(previous / "Demo.exe");
        REQUIRE(held);
        job.Cancel();
        REQUIRE(job.Result());
        CHECK(job.Result()->mOutcome == BuildOutcome::Failed);
        CHECK(job.Result()->mError.find("could not be moved back") != std::string::npos);
        CHECK_FALSE(fs::exists(sandbox.Out()));
    }
}

TEST_CASE("Cancel while a compile replaces the template puts the previous one back, or says where it is",
    "[build-job]")
{
    BuildSandbox sandbox;
    const fs::path gameBuildDir = sandbox.GameBuildDir();
    fs::path previous = gameBuildDir;
    previous += ".old";
    BuildJobRequest request = ProbeRequest(sandbox, "template-mid-swap");
    request.mGameBuildPreset = "windows-msvc-game-release";

    BuildJob job;
    REQUIRE(job.Start(request));
    WaitUntil([&] { return fs::exists(previous / "mjolnir_game_build_previous") && !fs::exists(gameBuildDir); });

    SECTION("put back")
    {
        job.Cancel();
        REQUIRE(job.Result());
        CHECK(job.Result()->mOutcome == BuildOutcome::Canceled);
        CHECK(ReadText(gameBuildDir / "MjolnirEngine.exe") == "game-build");
        CHECK_FALSE(fs::exists(gameBuildDir / "mjolnir_game_build_previous"));
        CHECK_FALSE(fs::exists(previous));
    }
    SECTION("held open, so it cannot move")
    {
        std::ifstream held(previous / "MjolnirEngine.exe");
        REQUIRE(held);
        job.Cancel();
        REQUIRE(job.Result());
        CHECK(job.Result()->mOutcome == BuildOutcome::Failed);
        const std::string &error = job.Result()->mError;
        INFO(error);
        CHECK(error.find("replaced the game build template") != std::string::npos);
        CHECK(error.find("could not be moved back") != std::string::npos);
        CHECK(error.find(previous.filename().string()) != std::string::npos);
        CHECK_FALSE(fs::exists(gameBuildDir));
    }
}

#if defined(_WIN32)
TEST_CASE("Build and Run starts the built game, which outlives the build, and a rebuild while it runs says it is locked",
    "[build-job]")
{
    BuildSandbox sandbox;
    const StopRunningFrom stop{ sandbox.Out() };
    // a real program as the template's exe: the probe, which with no arguments stays up as a game does
    fs::copy_file(MIR_EDITOR_BUILD_PROBE, sandbox.GameBuildDir() / "MjolnirEngine.exe", fs::copy_options::overwrite_existing);
    BuildJobRequest request = sandbox.Request();
    request.mRun = true;
    {
        BuildJob job;
        REQUIRE(job.Start(request));
        RunToEnd(job);
        const BuildResult &result = *job.Result();
        INFO(result.mError);
        REQUIRE(result.mOutcome == BuildOutcome::Succeeded);
        CHECK(result.mStarted);
        CHECK(result.mStartError.empty());
        CHECK(BuildNoticeFor(result, "Demo").mText.starts_with("Built and started Demo in "));
    }

    // the job is gone, and the game still runs from the output
    const std::vector<RunningProcess> running = ProcessesRunningFrom(sandbox.Out());
    REQUIRE(running.size() == 1);
    CHECK(running.front().mExecutable.filename() == "Demo.exe");

    BuildJob again;
    REQUIRE(again.Start(sandbox.Request()));
    RunToEnd(again);
    const BuildResult &locked = *again.Result();
    CHECK(locked.mOutcome == BuildOutcome::Failed);
    CHECK(locked.mError.starts_with("Demo.exe is running from "));
    CHECK(locked.mError.ends_with("which keeps the previous build locked; close it and build again"));
    CHECK(ProcessesRunningFrom(sandbox.Out()).size() == 1);
}

TEST_CASE("A Build and Run canceled after its report is written keeps the build and starts nothing", "[build-job]")
{
    BuildSandbox sandbox;
    const StopRunningFrom stop{ sandbox.Out() };
    fs::copy_file(MIR_EDITOR_BUILD_PROBE, sandbox.GameBuildDir() / "MjolnirEngine.exe", fs::copy_options::overwrite_existing);
    BuildJobRequest request = sandbox.Request();
    request.mRun = true;

    std::optional<BuildJob> job(std::in_place);
    REQUIRE(job->Start(request));
    // finished, and not yet seen by Update
    WaitUntil([&] { return fs::exists(request.mResultFile); });

    SECTION("Cancel")
    {
        job->Cancel();
        REQUIRE(job->Result());
        const BuildResult &result = *job->Result();
        INFO(result.mError);
        CHECK(result.mOutcome == BuildOutcome::Succeeded);
        CHECK_FALSE(result.mStarted);
        CHECK(result.mStartError.empty());
        CHECK(BuildNoticeFor(result, "Demo").mText.starts_with("Built Demo in "));
    }
    SECTION("the editor closing") { job.reset(); }

    CHECK(fs::exists(sandbox.Out() / "Demo.exe"));
    CHECK(ProcessesRunningFrom(sandbox.Out()).empty());
}
#endif

TEST_CASE("Build and Run says when the built game cannot start, and the build still counts", "[build-job]")
{
    // the sandbox template's exe is a text file
    BuildSandbox sandbox;
    BuildJobRequest request = sandbox.Request();
    request.mRun = true;

    BuildJob job;
    REQUIRE(job.Start(request));
    RunToEnd(job);
    const BuildResult &result = *job.Result();
    INFO(result.mError);
    REQUIRE(result.mOutcome == BuildOutcome::Succeeded);
    CHECK_FALSE(result.mStarted);
    CHECK(result.mStartError.starts_with("could not start "));

    const BuildNotice notice = BuildNoticeFor(result, "Demo");
    CHECK(notice.mKind == BuildNotice::Kind::Warning);
    CHECK(notice.mStays);
    CHECK(notice.mOpenSettings);
    CHECK(notice.mExecutable == result.mExecutable);
}

TEST_CASE("The toast says how a build ended, offering Explorer for a built game and Build Settings for the rest",
    "[build-job]")
{
    BuildResult built;
    built.mOutcome = BuildOutcome::Succeeded;
    built.mExecutable = "C:/out/Demo.exe";
    built.mSeconds = 12.34;
    BuildNotice notice = BuildNoticeFor(built, "Demo");
    CHECK(notice.mKind == BuildNotice::Kind::Success);
    CHECK(notice.mText == "Built Demo in 12.3 s");
    CHECK(notice.mExecutable == built.mExecutable);
    CHECK_FALSE(notice.mOpenSettings);
    CHECK_FALSE(notice.mStays);

    built.mStarted = true;
    built.mWarnings = { "one", "two" };
    notice = BuildNoticeFor(built, "Demo");
    CHECK(notice.mKind == BuildNotice::Kind::Warning);
    CHECK(notice.mText == "Built and started Demo in 12.3 s, with 2 warnings");
    CHECK(notice.mOpenSettings);

    // a game that could not start keeps Build Settings, where its warnings and the start error stay
    built.mStarted = false;
    built.mStartError = "could not start C:/out/Demo.exe; see the log";
    notice = BuildNoticeFor(built, "Demo");
    CHECK(notice.mKind == BuildNotice::Kind::Warning);
    CHECK(notice.mText == "Built Demo in 12.3 s, but could not start C:/out/Demo.exe; see the log");
    CHECK(notice.mOpenSettings);
    CHECK(notice.mStays);
    CHECK(notice.mExecutable == built.mExecutable);

    BuildResult failed;
    failed.mError = "compiling the game build with game-release failed:\n  a.cpp(1): error C2065: 'x'\n"
                    "  b.cpp(2): error C2065: 'y'\nThe whole output is in log.txt";
    notice = BuildNoticeFor(failed, "Demo");
    CHECK(notice.mKind == BuildNotice::Kind::Failure);
    CHECK(notice.mText ==
        "Build failed: compiling the game build with game-release failed:\n  a.cpp(1): error C2065: 'x' ...");
    CHECK(notice.mStays);
    CHECK(notice.mOpenSettings);
    CHECK(notice.mExecutable.empty());

    BuildResult canceled;
    canceled.mOutcome = BuildOutcome::Canceled;
    CHECK(BuildNoticeFor(canceled, "Demo").mKind == BuildNotice::Kind::Info);
    canceled.mOutcome = BuildOutcome::CanceledTooLate;
    CHECK(BuildNoticeFor(canceled, "Demo").mKind == BuildNotice::Kind::Warning);
    CHECK_FALSE(BuildNoticeFor(canceled, "Demo").mStays);
}

TEST_CASE("A build that cannot start says why", "[build-job]")
{
    BuildSandbox sandbox;
    BuildJobRequest request = sandbox.Request();

    BuildJob job;
    request.mTool.clear();
    CHECK_FALSE(job.Start(request));
    CHECK_FALSE(job.Running());
    REQUIRE(job.Result());
    CHECK(job.Result()->mOutcome == BuildOutcome::Failed);
    CHECK(job.Result()->mError.find("rebuild the editor") != std::string::npos);

    request.mTool = sandbox.mRoot / "NoSuchBuild.exe";
    CHECK_FALSE(job.Start(request));
    CHECK(job.Result()->mError.find("could not start MjolnirBuild") != std::string::npos);
}

TEST_CASE("MjolnirBuild gets the paths, and the preset and progress file when there are any", "[build-job]")
{
    BuildJobRequest request{ "C:/tools/MjolnirBuild.exe", "C:/game/games/Demo", "C:/game-build", "C:/My Builds/Demo",
        "C:/temp/result.json" };
    const std::vector<std::string> paths = { "--game", "C:/game/games/Demo", "--game-build", "C:/game-build", "--out",
        "C:/My Builds/Demo", "--result", "C:/temp/result.json" };
    CHECK(BuildArguments(request) == paths);

    request.mGameBuildPreset = "windows-msvc-game-release";
    request.mProgressFile = "C:/temp/progress.json";
    std::vector<std::string> compiling = paths;
    compiling.insert(compiling.end(),
        { "--game-build-preset", "windows-msvc-game-release", "--progress", "C:/temp/progress.json" });
    CHECK(BuildArguments(request) == compiling);
}

TEST_CASE("The report decides the outcome, and a missing one says how MjolnirBuild exited", "[build-job]")
{
    BuildSandbox sandbox;
    const fs::path file = sandbox.mRoot / "report.json";

    BuildSandbox::Write(file, R"({ "version": 1, "status": "succeeded", "game": "Demo", "output": "C:/out",
        "executable": "C:/out/Demo.exe", "assets": 12, "bytes": 3400000, "warnings": [ "stale template", 4 ],
        "steps": [ { "name": "check", "status": "succeeded", "seconds": 0.5 },
                   { "name": "terrain", "status": "skipped", "seconds": 0.0 } ] })");
    const BuildResult built = ReadBuildResult(file, 0);
    CHECK(built.mOutcome == BuildOutcome::Succeeded);
    CHECK(built.mExecutable == fs::path("C:/out/Demo.exe"));
    CHECK(built.mAssets == 12);
    CHECK(built.mBytes == 3400000);
    CHECK(built.mWarnings == std::vector<std::string>{ "stale template" });
    REQUIRE(built.mSteps.size() == 2);
    CHECK(built.mSteps[1].mName == "terrain");
    CHECK(built.mSteps[1].mStatus == "skipped");
    CHECK(built.mSteps[0].mSeconds == 0.5);

    BuildSandbox::Write(file, R"json({ "status": "failed", "error": "AssetCooker failed (exit 1)" })json");
    const BuildResult failed = ReadBuildResult(file, 1);
    CHECK(failed.mOutcome == BuildOutcome::Failed);
    CHECK(failed.mError == "AssetCooker failed (exit 1)");

    BuildSandbox::Write(file, R"({ "status": "failed" })");
    CHECK(ReadBuildResult(file, 1).mError == "MjolnirBuild exited with code 1");

    for (const std::string text : { std::string("not json"), std::string("[ 1 ]") })
    {
        INFO(text);
        BuildSandbox::Write(file, text);
        CHECK(ReadBuildResult(file, 3).mError == "MjolnirBuild exited with code 3 and wrote no report");
    }
    fs::remove(file);
    CHECK(ReadBuildResult(file, 2).mOutcome == BuildOutcome::Failed);
}

TEST_CASE("A build stopped mid-swap gets the previous build back", "[build-job]")
{
    BuildSandbox sandbox;
    const fs::path out = sandbox.mRoot / "swap" / "Demo";
    const fs::path previous = sandbox.mRoot / "swap" / "Demo.old";

    // stopped after the previous build moved aside, before the new one moved in
    BuildSandbox::Write(previous / "mjolnir_build.json", "{}");
    RestorePreviousBuild(out / "");
    CHECK(fs::is_regular_file(out / "mjolnir_build.json"));
    CHECK_FALSE(fs::exists(previous));

    // with the output in place, a leftover .old is not touched
    BuildSandbox::Write(previous / "mjolnir_build.json", "{}");
    RestorePreviousBuild(out);
    CHECK(fs::exists(previous));

    // nor is a folder that is not a build
    fs::remove_all(out);
    fs::remove(previous / "mjolnir_build.json");
    RestorePreviousBuild(out);
    CHECK_FALSE(fs::exists(out));
    CHECK(fs::exists(previous));
}

TEST_CASE("A game build template stopped mid-swap is moved back, and nothing else is", "[build-job]")
{
    BuildSandbox sandbox;
    const fs::path dir = sandbox.GameBuildDir();
    fs::path previous = dir;
    previous += ".old";

    // moved aside and marked
    fs::rename(dir, previous);
    BuildSandbox::Write(previous / "mjolnir_game_build_previous", "");
    CHECK_FALSE(RestorePreviousGameBuild(dir / ""));
    CHECK(ReadText(dir / "MjolnirEngine.exe") == "game-build");
    CHECK_FALSE(fs::exists(dir / "mjolnir_game_build_previous"));
    CHECK_FALSE(fs::exists(previous));

    // stopped before the marker was written, so known by being a template
    fs::rename(dir, previous);
    CHECK_FALSE(RestorePreviousGameBuild(dir));
    CHECK(fs::exists(dir / "MjolnirEngine.exe"));

    // with the template in place, a leftover .old is not touched
    BuildSandbox::Write(previous / "mjolnir_game_build_previous", "");
    CHECK_FALSE(RestorePreviousGameBuild(dir));
    CHECK(fs::exists(previous));

    // nor is a folder that is neither marked nor a template
    fs::remove_all(dir);
    fs::remove(previous / "mjolnir_game_build_previous");
    BuildSandbox::Write(previous / "notes.txt", "x");
    CHECK_FALSE(RestorePreviousGameBuild(dir));
    CHECK_FALSE(fs::exists(dir));
    CHECK(fs::exists(previous / "notes.txt"));
}

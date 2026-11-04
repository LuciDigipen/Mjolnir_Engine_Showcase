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
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace mir;

namespace
{
    namespace fs = std::filesystem;

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
            return BuildJobRequest{ MIR_EDITOR_BUILD_TOOL, Game(), GameBuildDir(), Out(), mRoot / "result.json" };
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

TEST_CASE("MjolnirBuild gets the game, template, output and report paths", "[build-job]")
{
    const BuildJobRequest request{ "C:/tools/MjolnirBuild.exe", "C:/game/games/Demo", "C:/game-build", "C:/My Builds/Demo",
        "C:/temp/result.json" };
    CHECK(BuildArguments(request) == std::vector<std::string>{ "--game", "C:/game/games/Demo", "--game-build",
        "C:/game-build", "--out", "C:/My Builds/Demo", "--result", "C:/temp/result.json" });
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

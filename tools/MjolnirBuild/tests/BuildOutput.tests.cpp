/**
 * @file BuildOutput.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for the output folders MjolnirBuild refuses and its staged swap.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "BuildTestSupport.h"

#include <BuildOutput.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <fstream>
#include <optional>
#include <thread>

using namespace mir::build;
using namespace mir::build::test;

namespace
{
    struct Layout
    {
        Sandbox mSandbox;
        fs::path mSource = mSandbox.mRoot / "checkout";
        fs::path mGameBuildDir = mSandbox.mRoot / "game-build";

        Layout()
        {
            fs::create_directories(mSource / "games" / "Demo");
            fs::create_directories(mSource / "builds");
            fs::create_directories(mGameBuildDir);
        }

        OutputRules Rules() const { return { mSource, mGameBuildDir }; }
    };
}

TEST_CASE("An output folder outside the checkout is accepted, missing or empty", "[build-output]")
{
    Layout layout;
    CHECK_FALSE(CheckOutputDir(layout.mSandbox.mRoot / "out", layout.Rules()));

    fs::create_directories(layout.mSandbox.mRoot / "empty");
    CHECK_FALSE(CheckOutputDir(layout.mSandbox.mRoot / "empty", layout.Rules()));
}

TEST_CASE("An output folder under the checkout's builds/ is accepted", "[build-output]")
{
    Layout layout;
    CHECK_FALSE(CheckOutputDir(layout.mSource / "builds" / "game-build-out" / "Demo", layout.Rules()));
}

TEST_CASE("An output folder elsewhere in the checkout is refused", "[build-output]")
{
    Layout layout;
    CHECK(CheckOutputDir(layout.mSource / "games" / "Demo" / "out", layout.Rules()));
    CHECK(CheckOutputDir(layout.mSource / "dist", layout.Rules()));
    CHECK(CheckOutputDir(layout.mSource / "builds", layout.Rules()));
}

TEST_CASE("An output folder holding the checkout is refused", "[build-output]")
{
    Layout layout;
    CHECK(CheckOutputDir(layout.mSource, layout.Rules()));
    CHECK(CheckOutputDir(layout.mSandbox.mRoot, layout.Rules()));
    CHECK(CheckOutputDir(layout.mSource.root_path(), layout.Rules()));
}

TEST_CASE("An output folder inside a CMake build folder is refused", "[build-output]")
{
    Layout layout;
    Touch(layout.mSandbox.mRoot / "tree" / "CMakeCache.txt");

    const auto problem = CheckOutputDir(layout.mSandbox.mRoot / "tree" / "run" / "out", layout.Rules());
    REQUIRE(problem);
    CHECK(problem->find("build folder") != std::string::npos);
}

TEST_CASE("An output folder overlapping the game build template is refused", "[build-output]")
{
    Layout layout;
    CHECK(CheckOutputDir(layout.mGameBuildDir, layout.Rules()));
    CHECK(CheckOutputDir(layout.mGameBuildDir / "out", layout.Rules()));
}

TEST_CASE("A game build template where the build stages or moves the previous build aside is refused", "[build-output]")
{
    Layout layout;
    const fs::path out = layout.mSandbox.mRoot / "Demo";
    CHECK(CheckOutputDir(out, { layout.mSource, StagingDir(out) }));
    CHECK(CheckOutputDir(out, { layout.mSource, PreviousDir(out) }));
    CHECK(CheckOutputDir(out, { layout.mSource, StagingDir(out) / "game-build" }));
}

TEST_CASE("A .tmp or .old folder a build did not make is refused", "[build-output]")
{
    Layout layout;
    const fs::path out = layout.mSandbox.mRoot / "out";
    for (const fs::path &dir : { StagingDir(out), PreviousDir(out) })
    {
        Touch(dir / "mine.txt");
        CHECK(CheckOutputDir(out, layout.Rules()));

        Touch(dir / kBuildMarkerFileName, "{}");
        CHECK_FALSE(CheckOutputDir(out, layout.Rules()));
    }
}

TEST_CASE("A non-empty output folder is accepted only when it holds a previous build", "[build-output]")
{
    Layout layout;
    const fs::path out = layout.mSandbox.mRoot / "docs";
    Touch(out / "notes.txt");
    CHECK(CheckOutputDir(out, layout.Rules()));

    Touch(out / kBuildMarkerFileName, "{}");
    CHECK_FALSE(CheckOutputDir(out, layout.Rules()));
}

TEST_CASE("A file is refused as the output folder", "[build-output]")
{
    Layout layout;
    Touch(layout.mSandbox.mRoot / "out");
    CHECK(CheckOutputDir(layout.mSandbox.mRoot / "out", layout.Rules()));
}

TEST_CASE("A build whose longest path would pass the Windows limit is refused", "[build-output]")
{
    Sandbox sandbox;
    const fs::path out = sandbox.mRoot / "out";
    const std::size_t room = kMaxPathLength - StagingDir(out).native().size() - 1;

    CHECK_FALSE(CheckPathLength(out, room));
    CHECK(CheckPathLength(out, room + 1));
}

TEST_CASE("Staging replaces what a failed build left", "[build-output]")
{
    Sandbox sandbox;
    const fs::path out = sandbox.mRoot / "out";
    Touch(StagingDir(out) / kBuildMarkerFileName, "{}");
    Touch(StagingDir(out) / "stale.txt");

    const fs::path staging = BeginStaging(out);
    CHECK(staging == StagingDir(out));
    CHECK(IsBuildDir(staging));
    CHECK_FALSE(fs::exists(staging / "stale.txt"));
}

TEST_CASE("Staging refuses a folder a build did not make", "[build-output]")
{
    Sandbox sandbox;
    const fs::path out = sandbox.mRoot / "out";
    Touch(StagingDir(out) / "mine.txt");

    CHECK_THROWS(BeginStaging(out));
    CHECK(fs::exists(StagingDir(out) / "mine.txt"));
}

TEST_CASE("Committing replaces the previous build", "[build-output]")
{
    Sandbox sandbox;
    const fs::path out = sandbox.mRoot / "out";
    Touch(out / kBuildMarkerFileName, "{}");
    Touch(out / "old.txt");

    const fs::path staging = BeginStaging(out);
    Touch(staging / "new.txt");
    CommitStaging(staging, out);

    CHECK(fs::exists(out / "new.txt"));
    CHECK_FALSE(fs::exists(out / "old.txt"));
    CHECK_FALSE(fs::exists(StagingDir(out)));
    CHECK_FALSE(fs::exists(PreviousDir(out)));
}

TEST_CASE("Committing creates a missing output folder", "[build-output]")
{
    Sandbox sandbox;
    const fs::path out = sandbox.mRoot / "nested" / "out";
    fs::create_directories(out.parent_path());

    const fs::path staging = BeginStaging(out);
    Touch(staging / "new.txt");
    CommitStaging(staging, out);

    CHECK(fs::exists(out / "new.txt"));
    CHECK(IsBuildDir(out));
}

TEST_CASE("A commit blocked by a file in use leaves the previous build", "[build-output]")
{
    Sandbox sandbox;
    const fs::path out = sandbox.mRoot / "out";
    Touch(out / kBuildMarkerFileName, "{}");
    Touch(out / "running.txt");

    const fs::path staging = BeginStaging(out);
    Touch(staging / "new.txt");
    {
        // Stands in for a game still running from the previous build
        std::ofstream inUse(out / "running.txt", std::ios::app);
        REQUIRE(inUse);
        CHECK_THROWS(CommitStaging(staging, out));
    }

    CHECK(fs::exists(out / "running.txt"));
    CHECK_FALSE(fs::exists(out / "new.txt"));
    CHECK(fs::exists(staging / "new.txt"));
}

TEST_CASE("A commit retries until a temporary file handle is released", "[build-output]")
{
    Sandbox sandbox;
    const fs::path out = sandbox.mRoot / "out";
    Touch(out / kBuildMarkerFileName, "{}");
    Touch(out / "running.txt");

    const fs::path staging = BeginStaging(out);
    Touch(staging / "new.txt");

    std::ofstream inUse(out / "running.txt", std::ios::app);
    REQUIRE(inUse);
    std::thread release([stream = std::move(inUse)]() mutable
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            stream.close();
        });

    bool committed = false;
    try
    {
        CommitStaging(staging, out);
        committed = true;
    }
    catch (const std::exception &)
    {}
    release.join();

    CHECK(committed);
    CHECK(fs::exists(out / "new.txt"));
    CHECK_FALSE(fs::exists(out / "running.txt"));
}

TEST_CASE("The result and progress files are refused inside the output, its .tmp or its .old", "[build-output]")
{
    Sandbox sandbox;
    const fs::path out = sandbox.mRoot / "out";

    CHECK_FALSE(ReportFileProblem(sandbox.mRoot / "result.json", out, "--result"));
    CHECK_FALSE(ReportFileProblem(sandbox.mRoot / "out.tmpx" / "progress.json", out, "--progress"));
    for (const fs::path &file : { out, out / "progress.json", sandbox.mRoot / "out.tmp" / "progress.json",
             sandbox.mRoot / "out.old" / "deep" / "progress.json" })
    {
        INFO(PathText(file));
        const std::optional<std::string> problem = ReportFileProblem(file, out, "--progress");
        REQUIRE(problem);
        CHECK(*problem == "--progress must not be inside --out or its .tmp and .old folders");
    }
}

TEST_CASE("A report file is written into a folder not made yet, and past a reader holding it for a moment",
    "[build-output]")
{
    Sandbox sandbox;
    const fs::path file = sandbox.mRoot / "reports" / "new" / "progress.json";
    REQUIRE(WriteReportFile(file, "first\n"));
    CHECK(mir::ReadText(file) == "first\n");

    // MSVC's ifstream does not share delete, so the file cannot be replaced while it is open
    std::optional<std::ifstream> reader(std::in_place, file, std::ios::binary);
    REQUIRE(reader->is_open());
    CHECK_FALSE(mir::WriteTextAtomic(file, "blocked\n"));

    std::thread release([&reader] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        reader.reset();
    });
    const bool written = WriteReportFile(file, "second\n");
    release.join();
    CHECK(written);
    CHECK(mir::ReadText(file) == "second\n");
}

TEST_CASE("A folder a program runs from is locked, and the message names the program", "[build-output]")
{
    Sandbox sandbox;
    const fs::path out = sandbox.mRoot / "out";
    fs::create_directories(out);
    CHECK_FALSE(LockedBuildProblem(out, "the previous build"));

    {
        RunningProgram game(out, "Demo.exe");
        const std::optional<std::string> problem = LockedBuildProblem(out, "the previous build");
        REQUIRE(problem);
        CHECK(problem->starts_with("Demo.exe is running from " + PathText(out) + " (process "));
        CHECK(problem->ends_with("), which keeps the previous build locked; close it and build again"));
    }
    CHECK_FALSE(LockedBuildProblem(out, "the previous build"));

    // as is one left in the .old folder beside it
    RunningProgram previous(PreviousDir(out), "Demo.exe");
    const std::optional<std::string> problem = LockedBuildProblem(out, "the previous build");
    REQUIRE(problem);
    CHECK(problem->find(PathText(PreviousDir(out))) != std::string::npos);
}

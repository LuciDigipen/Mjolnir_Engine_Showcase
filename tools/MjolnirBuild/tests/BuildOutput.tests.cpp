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

#include <fstream>

using namespace mir::build;
using namespace mir::build::test;

namespace
{
    struct Layout
    {
        Sandbox mSandbox;
        fs::path mSource = mSandbox.mRoot / "checkout";
        fs::path mPlayer = mSandbox.mRoot / "player";

        Layout()
        {
            fs::create_directories(mSource / "games" / "Demo");
            fs::create_directories(mSource / "builds");
            fs::create_directories(mPlayer);
        }

        OutputRules Rules() const { return { mSource, mPlayer }; }
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
    CHECK_FALSE(CheckOutputDir(layout.mSource / "builds" / "player-out" / "Demo", layout.Rules()));
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

TEST_CASE("An output folder overlapping the player template is refused", "[build-output]")
{
    Layout layout;
    CHECK(CheckOutputDir(layout.mPlayer, layout.Rules()));
    CHECK(CheckOutputDir(layout.mPlayer / "out", layout.Rules()));
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
    CHECK_FALSE(fs::exists(fs::path(out).concat(".old")));
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

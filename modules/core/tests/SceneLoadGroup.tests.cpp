/**
 * @file SceneLoadGroup.tests.cpp
 * @author sumin.park
 * @brief Scene readiness: discovery, settled work, monotonic progress and a restart per load.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/SceneLoadGroup.h>

#include <catch2/catch_test_macros.hpp>

using namespace mir;

namespace
{
    int gFirst, gSecond;
}

TEST_CASE("A group that never began is ready", "[scene-load]")
{
    SceneLoadGroup group;
    CHECK(group.Ready());
    CHECK(group.Progress() == 1.0f);
}

TEST_CASE("A load is not ready until a frame has reported its work and all of it settled", "[scene-load]")
{
    SceneLoadGroup group;
    group.Begin();
    CHECK_FALSE(group.Ready());
    CHECK_FALSE(group.Discovered());

    // the first frame only learns the work, even if it already looks finished
    group.Report(&gFirst, 2, 2);
    group.EndFrame();
    CHECK(group.Discovered());
    CHECK_FALSE(group.Ready());

    group.Report(&gFirst, 2, 1);
    group.EndFrame();
    CHECK_FALSE(group.Ready());
    CHECK(group.Progress() == 0.5f);

    group.Report(&gFirst, 2, 2);
    group.EndFrame();
    CHECK(group.Ready());
    CHECK(group.Progress() == 1.0f);
}

TEST_CASE("Every reporting source must settle, and a source's later report replaces its earlier one",
    "[scene-load]")
{
    SceneLoadGroup group;
    group.Begin();
    group.EndFrame();
    group.Report(&gFirst, 1, 0);
    group.Report(&gFirst, 1, 1);
    group.Report(&gSecond, 3, 1);
    group.EndFrame();
    CHECK_FALSE(group.Ready());
    group.Report(&gFirst, 1, 1);
    group.Report(&gSecond, 3, 3);
    group.EndFrame();
    CHECK(group.Ready());
}

TEST_CASE("Progress never decreases while more work is found", "[scene-load]")
{
    SceneLoadGroup group;
    group.Begin();
    group.EndFrame();
    group.Report(&gFirst, 2, 2);
    group.Report(&gSecond, 2, 1);
    group.EndFrame();
    const float before = group.Progress();
    CHECK(before == 0.75f);

    // a dependency closure grows, the fraction would fall to a third
    group.Report(&gFirst, 6, 2);
    group.EndFrame();
    CHECK(group.Progress() == before);
    CHECK_FALSE(group.Ready());
}

TEST_CASE("A ready group stays ready until the next load begins", "[scene-load]")
{
    SceneLoadGroup group;
    group.Begin();
    group.EndFrame();
    group.Report(&gFirst, 1, 1);
    group.EndFrame();
    REQUIRE(group.Ready());

    // work a script creates later does not reopen the gate
    group.Report(&gFirst, 5, 0);
    group.EndFrame();
    CHECK(group.Ready());

    group.Begin();
    CHECK_FALSE(group.Ready());
    CHECK(group.Progress() == 0.0f);
}

TEST_CASE("A load can be given up on", "[scene-load]")
{
    SceneLoadGroup group;
    group.Begin();
    group.EndFrame();
    group.Report(&gFirst, 1, 0);
    group.EndFrame();
    REQUIRE_FALSE(group.Ready());
    group.ForceReady();
    CHECK(group.Ready());
    CHECK(group.Progress() == 1.0f);
}

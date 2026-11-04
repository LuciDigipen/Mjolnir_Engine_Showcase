/**
 * @file StartupGate.tests.cpp
 * @author Rahul Nair
 * @brief Verifies startup waits for loading and never waits forever.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/StartupGate.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace mir;

namespace
{
    StartupGate::Result Settle(StartupGate &gate, double seconds)
    {
        StartupGate::Result result = StartupGate::Result::Waiting;
        for (int i = 0; i < StartupGate::kSettledUpdates; ++i)
            result = gate.Update(0, seconds);
        return result;
    }
}

TEST_CASE("StartupGate is ready once nothing is pending for the settled updates", "[startup]")
{
    StartupGate gate;
    for (int i = 0; i < StartupGate::kSettledUpdates - 1; ++i)
        CHECK(gate.Update(0, 0.0) == StartupGate::Result::Waiting);
    CHECK(gate.Update(0, 0.0) == StartupGate::Result::Ready);
}

TEST_CASE("StartupGate restarts settling when something new becomes pending", "[startup]")
{
    StartupGate gate;
    for (int i = 0; i < StartupGate::kSettledUpdates - 1; ++i)
        gate.Update(0, 0.0);
    CHECK(gate.Update(5, 0.1) == StartupGate::Result::Waiting);
    for (int i = 0; i < StartupGate::kSettledUpdates - 1; ++i)
        CHECK(gate.Update(0, 0.2) == StartupGate::Result::Waiting);
    CHECK(gate.Update(0, 0.2) == StartupGate::Result::Ready);
}

TEST_CASE("StartupGate keeps waiting while pending keeps falling", "[startup]")
{
    StartupGate gate;
    size_t pending = 5;
    for (double seconds = 0.0; pending > 0; seconds += StartupGate::kStallSeconds - 1.0, --pending)
        CHECK(gate.Update(pending, seconds) == StartupGate::Result::Waiting);
    CHECK(Settle(gate, 40.0) == StartupGate::Result::Ready);
}

TEST_CASE("StartupGate measures progress from when views first ask for work", "[startup]")
{
    StartupGate gate;
    CHECK(gate.Update(std::nullopt, 0.0) == StartupGate::Result::Waiting);
    size_t pending = 5;
    for (double seconds = 1.0; pending > 0; seconds += StartupGate::kStallSeconds - 1.0, --pending)
        CHECK(gate.Update(pending, seconds) == StartupGate::Result::Waiting);
    CHECK(Settle(gate, 40.0) == StartupGate::Result::Ready);
}

TEST_CASE("StartupGate gives work that follows none pending a fresh baseline", "[startup]")
{
    StartupGate gate;
    CHECK(gate.Update(0, 0.0) == StartupGate::Result::Waiting);
    CHECK(gate.Update(5, StartupGate::kStallSeconds - 1.0) == StartupGate::Result::Waiting);
    CHECK(gate.Update(4, 2.0 * StartupGate::kStallSeconds - 2.0) == StartupGate::Result::Waiting);
    CHECK(gate.Update(4, 3.0 * StartupGate::kStallSeconds - 2.0) == StartupGate::Result::Stalled);
}

TEST_CASE("StartupGate gives up when no view ever asks for work", "[startup]")
{
    StartupGate gate;
    CHECK(gate.Update(std::nullopt, 0.0) == StartupGate::Result::Waiting);
    CHECK(gate.Update(std::nullopt, StartupGate::kStallSeconds - 0.1) == StartupGate::Result::Waiting);
    CHECK(gate.Update(std::nullopt, StartupGate::kStallSeconds) == StartupGate::Result::Stalled);
}

TEST_CASE("StartupGate does not settle on updates with no view", "[startup]")
{
    StartupGate gate;
    for (int i = 0; i < StartupGate::kSettledUpdates - 1; ++i)
        gate.Update(0, 0.0);
    CHECK(gate.Update(std::nullopt, 0.1) == StartupGate::Result::Waiting);
    CHECK(gate.Update(0, 0.2) == StartupGate::Result::Waiting);
}

TEST_CASE("StartupGate gives up when pending stops falling", "[startup]")
{
    StartupGate gate;
    CHECK(gate.Update(3, 0.0) == StartupGate::Result::Waiting);
    CHECK(gate.Update(3, StartupGate::kStallSeconds - 0.1) == StartupGate::Result::Waiting);
    CHECK(gate.Update(3, StartupGate::kStallSeconds) == StartupGate::Result::Stalled);
}

TEST_CASE("StartupGate does not count a rise and fall back as progress", "[startup]")
{
    StartupGate gate;
    CHECK(gate.Update(3, 0.0) == StartupGate::Result::Waiting);
    CHECK(gate.Update(9, 1.0) == StartupGate::Result::Waiting);
    CHECK(gate.Update(3, StartupGate::kStallSeconds) == StartupGate::Result::Stalled);
}

TEST_CASE("StartupGate gives up at the limit even while pending falls", "[startup]")
{
    StartupGate gate;
    size_t pending = 1000;
    double seconds = 0.0;
    StartupGate::Result result = StartupGate::Result::Waiting;
    while (result == StartupGate::Result::Waiting && seconds < 2.0 * StartupGate::kMaxSeconds)
    {
        result = gate.Update(pending--, seconds);
        seconds += 1.0;
    }
    CHECK(result == StartupGate::Result::TimedOut);
    CHECK(seconds == Catch::Approx(StartupGate::kMaxSeconds + 1.0));
}

TEST_CASE("StartupGate reports progress against the most that was pending", "[startup]")
{
    StartupGate gate;
    CHECK(gate.Progress() == Catch::Approx(1.0f));
    gate.Update(8, 0.0);
    CHECK(gate.Progress() == Catch::Approx(0.0f));
    gate.Update(2, 1.0);
    CHECK(gate.Pending() == 2);
    CHECK(gate.Progress() == Catch::Approx(0.75f));
    gate.Update(0, 2.0);
    CHECK(gate.Progress() == Catch::Approx(1.0f));
}

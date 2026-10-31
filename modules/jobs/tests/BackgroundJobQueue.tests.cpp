/**
 * @file BackgroundJobQueue.tests.cpp
 * @author Rahul Nair
 * @brief Checks draining and reuse of shared background lanes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <jobs/BackgroundJobQueue.h>
#include <jobs/JobExecutor.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>

using namespace mir;
using namespace std::chrono_literals;

TEST_CASE("Waiting for a background lane drains running and queued callbacks without closing it", "[jobs]")
{
    JobExecutor jobs(1);
    std::promise<void> started, resume;
    auto resumed = resume.get_future().share();
    std::atomic<int> completed = 0;
    BackgroundJobQueue lane(jobs, 1);
    lane.Post([&]
        {
            started.set_value();
            // Bound the gate even when an assertion fails.
            resumed.wait_for(5s);
            ++completed;
        });
    REQUIRE(started.get_future().wait_for(5s) == std::future_status::ready);
    lane.Post([&] { ++completed; });
    lane.Post([&] { ++completed; });

    auto drained = std::async(std::launch::async, [&] { lane.WaitIdle(); });
    CHECK(drained.wait_for(20ms) == std::future_status::timeout);
    resume.set_value();
    REQUIRE(drained.wait_for(5s) == std::future_status::ready);
    drained.get();
    CHECK(completed == 3);

    lane.Post([&] { ++completed; });
    lane.WaitIdle();
    CHECK(completed == 4);
}

TEST_CASE("Waiting for an inline or closed background lane returns", "[jobs]")
{
    JobExecutor jobs(0);
    BackgroundJobQueue lane(jobs, 1);
    int completed = 0;
    lane.WaitIdle();
    lane.Post([&] { ++completed; });
    lane.WaitIdle();
    CHECK(completed == 1);
    lane.CloseAndWait();
    lane.WaitIdle();
    lane.Post([&] { ++completed; });
    CHECK(completed == 1);
}

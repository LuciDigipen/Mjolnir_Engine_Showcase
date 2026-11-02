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
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>

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

TEST_CASE("Submitted background tasks return values and exceptions on shared or serial execution", "[jobs]")
{
    const auto workers = GENERATE(0u, 1u);
    CAPTURE(workers);
    JobExecutor jobs(workers);
    BackgroundJobQueue lane(jobs, 1);
    auto value = lane.Submit([owned = std::make_unique<int>(42)]() mutable { return std::move(owned); });
    REQUIRE(value.wait_for(2s) == std::future_status::ready);
    CHECK(*value.get() == 42);
    bool ran = false;
    auto done = lane.Submit([&] { ran = true; });
    REQUIRE(done.wait_for(2s) == std::future_status::ready);
    done.get();
    CHECK(ran);
    auto failure = lane.Submit([]() -> int { throw std::runtime_error("injected task failure"); });
    REQUIRE(failure.wait_for(2s) == std::future_status::ready);
    CHECK_THROWS_AS(failure.get(), std::runtime_error);
    auto afterFailure = lane.Submit([] { return 7; });
    REQUIRE(afterFailure.wait_for(2s) == std::future_status::ready);
    CHECK(afterFailure.get() == 7);
}

TEST_CASE("Closing a background lane completes cancelled futures without running their tasks", "[jobs]")
{
    JobExecutor jobs(1);
    std::promise<void> release, started;
    auto resumed = release.get_future().share();
    auto running = started.get_future();
    BackgroundJobQueue other(jobs, 1), lane(jobs, 1);
    struct ReleaseWorker
    {
        std::promise<void> &release;
        ~ReleaseWorker() { release.set_value(); }
    } releaseWorker{release};
    other.Post([resumed, &started] { started.set_value(); resumed.wait(); });
    REQUIRE(running.wait_for(2s) == std::future_status::ready);
    bool ran = false;
    auto cancelled = lane.Submit([&] { ran = true; });
    CHECK(cancelled.wait_for(0ms) == std::future_status::timeout);
    lane.CloseAndWait();
    REQUIRE(cancelled.wait_for(0ms) == std::future_status::ready);
    try { cancelled.get(); FAIL("cancelled task unexpectedly completed"); }
    catch (const std::future_error &error) { CHECK(error.code() == std::make_error_code(std::future_errc::broken_promise)); }
    auto closed = lane.Submit([&] { ran = true; });
    REQUIRE(closed.wait_for(0ms) == std::future_status::ready);
    CHECK_THROWS_AS(closed.get(), std::future_error);
    CHECK_FALSE(ran);
}

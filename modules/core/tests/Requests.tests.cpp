/**
 * @file Requests.tests.cpp
 * @author sumin.park
 * @brief Tests for the RequestQueue merge rules.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/Requests.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace mir;

TEST_CASE("Requests come out in push order with duplicates dropped", "[ecs][requests]")
{
    RequestQueue queue;
    queue.Push(Request::Stop);
    queue.Push(Request::Play);
    queue.Push(Request::Stop);

    REQUIRE(queue.Take() == std::vector<Request>{ Request::Stop, Request::Play });
    REQUIRE(queue.Empty());
}

TEST_CASE("Quit applies after every other request", "[ecs][requests]")
{
    RequestQueue queue;
    queue.Push(Request::Quit);
    queue.Push(Request::SaveScene);
    queue.Push(Request::Stop);

    REQUIRE(queue.Take() == std::vector<Request>{ Request::SaveScene, Request::Stop, Request::Quit });
}

TEST_CASE("Requests made while held are dropped and the held ones come back", "[ecs][requests]")
{
    RequestQueue queue;
    queue.Push(Request::SaveScene);
    queue.PushOpenProject("games/Other");

    RequestQueue::Held held = queue.Hold();
    CHECK(queue.Empty());
    // a click on a button, a project switch, anything made while the scene loads
    queue.Push(Request::Pause);
    queue.Push(Request::Quit);
    queue.PushOpenProject("games/Wrong");

    CHECK(queue.Restore(std::move(held)) == 3);
    CHECK(queue.TakeOpenProject() == std::optional<std::string>("games/Other"));
    CHECK(queue.Take() == std::vector<Request>{ Request::SaveScene });
}

TEST_CASE("Holding and restoring an empty queue changes nothing", "[ecs][requests]")
{
    RequestQueue queue;
    RequestQueue::Held held = queue.Hold();
    CHECK(queue.Restore(std::move(held)) == 0);
    CHECK(queue.Empty());
}

TEST_CASE("A request pushed while applying waits for the next Take", "[ecs][requests]")
{
    RequestQueue queue;
    queue.Push(Request::Play);

    std::vector<Request> applied;
    for (const Request request : queue.Take())
    {
        applied.push_back(request);
        queue.Push(Request::Pause);
    }

    REQUIRE(applied == std::vector<Request>{ Request::Play });
    REQUIRE(queue.Take() == std::vector<Request>{ Request::Pause });
}

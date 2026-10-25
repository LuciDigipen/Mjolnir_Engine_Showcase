/**
 * @file RegistrationList.tests.cpp
 * @author sumin.park
 * @brief Tests for core/RegistrationList: order, tokens, and a throwing function.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/RegistrationList.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace mir;

TEST_CASE("A list runs its functions in registration order, with the run's arguments", "[registration]")
{
    RegistrationList<std::function<void(std::vector<int> &, int)>> list;
    std::vector<int> calls;
    const Registration first = list.Add([](std::vector<int> &out, int value) { out.push_back(value); });
    const Registration second = list.Add([](std::vector<int> &out, int value) { out.push_back(value * 10); });

    list.Run(calls, 2);
    CHECK(calls == std::vector<int>{ 2, 20 });
    CHECK(list.Size() == 2);
}

TEST_CASE("A dropped token takes its function out; a moved one keeps it", "[registration]")
{
    RegistrationList<std::function<void(int &)>> list;
    int calls = 0;
    Registration kept;
    {
        Registration original = list.Add([](int &count) { ++count; });
        const Registration dropped = list.Add([](int &count) { count += 100; });
        kept = std::move(original);
    }

    list.Run(calls);
    CHECK(calls == 1);
    kept.Reset();
    CHECK(list.Size() == 0);
}

TEST_CASE("A throwing function stops the run, and the list can change again after", "[registration]")
{
    RegistrationList<std::function<void()>> list;
    const Registration throws = list.Add([] { throw std::runtime_error("failed"); });

    REQUIRE_THROWS_AS(list.Run(), std::runtime_error);

    // asserts if the throw left the list marked as running
    const Registration after = list.Add([] {});
    CHECK(list.Size() == 2);
}

/**
 * @file RunGuarded.tests.cpp
 * @author sumin.park
 * @brief Tests for RunGuarded, the worker job guard.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain/RunGuarded.h>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

using namespace mir;

namespace
{
    std::string ToError(std::string_view what) { return "failed: " + std::string(what); }
}

TEST_CASE("RunGuarded returns the job's own result when it does not throw", "[terrain][guard]")
{
    CHECK(RunGuarded([] { return std::string("built"); }, ToError) == "built");
}

TEST_CASE("RunGuarded turns a std::exception into the job's error result", "[terrain][guard]")
{
    const std::string result = RunGuarded([]() -> std::string { throw std::runtime_error("bad path"); }, ToError);
    CHECK(result == "failed: bad path");
}

TEST_CASE("RunGuarded names an exception that is not a std::exception", "[terrain][guard]")
{
    const std::string result = RunGuarded([]() -> std::string { throw 42; }, ToError);
    CHECK(result == "failed: unknown exception");
}

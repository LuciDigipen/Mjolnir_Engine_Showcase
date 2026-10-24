/**
 * @file SetupError.tests.cpp
 * @author sumin.park
 * @brief Tests for core/log/SetupError.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/log/SetupError.h>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using mir::ErrorCategory;

TEST_CASE("Every category has its own exit code, clear of 0 and 1", "[setup-error]")
{
    std::set<int> codes;
    for (ErrorCategory category : { ErrorCategory::Usage, ErrorCategory::ProjectData, ErrorCategory::BuildOutput,
        ErrorCategory::System, ErrorCategory::Internal })
    {
        const int code = mir::ExitCodeOf(category);
        CHECK(code > 1);
        CHECK(code < 256);
        codes.insert(code);
    }
    CHECK(codes.size() == 5);
}

TEST_CASE("Exit codes keep their published values", "[setup-error]")
{
    CHECK(mir::ExitCodeOf(ErrorCategory::Usage) == 10);
    CHECK(mir::ExitCodeOf(ErrorCategory::ProjectData) == 11);
    CHECK(mir::ExitCodeOf(ErrorCategory::BuildOutput) == 12);
    CHECK(mir::ExitCodeOf(ErrorCategory::System) == 13);
    CHECK(mir::ExitCodeOf(ErrorCategory::Internal) == 14);
}

TEST_CASE("SetupError carries its fields and reports the summary as what()", "[setup-error]")
{
    const mir::SetupError error(ErrorCategory::ProjectData, "game.json is not valid JSON", "games/A/game.json",
        "syntax error at line 3", "Fix the JSON at games/A/game.json");

    CHECK(error.Category() == ErrorCategory::ProjectData);
    CHECK(std::string(error.what()) == "game.json is not valid JSON");
    CHECK(error.Path() == std::filesystem::path("games/A/game.json"));
    CHECK(error.Cause() == "syntax error at line 3");
    CHECK(error.Fix() == "Fix the JSON at games/A/game.json");
}

TEST_CASE("SetupError is caught as a std::exception", "[setup-error]")
{
    try
    {
        throw mir::SetupError(ErrorCategory::System, "no window", {}, "", "");
    }
    catch (const std::exception &error)
    {
        CHECK(std::string(error.what()) == "no window");
        return;
    }
    FAIL("not caught");
}

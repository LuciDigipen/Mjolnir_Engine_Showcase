/**
 * @file TerrainCrash.tests.cpp
 * @author sumin.park
 * @brief Out of memory in a terrain job reaches the crash handler instead of the job's error result.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/platform/CrashHandler.h>
#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

TEST_CASE("std::bad_alloc in a terrain job ends the process through the crash handler", "[terrain][guard][crash]")
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("mir_terrain_crash_" + std::to_string(mir::CurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    mir::ChildProcess probe;
    const std::vector<std::string> args{ dir.string() };
    REQUIRE(probe.Start(MIR_TERRAIN_CRASH_PROBE_PATH, args));
    const std::optional<int> exitCode = probe.Wait();

    std::ifstream input(dir / "last-error.json", std::ios::binary);
    const std::string report{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    input.close();
    std::filesystem::remove_all(dir, ec);

    UNSCOPED_INFO(report);
    CHECK(exitCode == mir::kCrashExitCode);
    CHECK(report.find("std::terminate on thread") != std::string::npos);
    CHECK(report.find(": bad allocation") != std::string::npos);
}

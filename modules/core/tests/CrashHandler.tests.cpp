/**
 * @file CrashHandler.tests.cpp
 * @author sumin.park
 * @brief Tests for the crash handler, each crash in its own CrashProbe child process.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/platform/CrashHandler.h>
#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    struct CrashRun
    {
        std::optional<int> exitCode;
        std::string report;
        std::uintmax_t dumpSize = 0;
    };

    std::size_t Count(const std::string &text, const std::string &what)
    {
        std::size_t count = 0;
        for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
            ++count;
        return count;
    }

    CrashRun RunProbe(const std::string &mode)
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() /
            ("mir_crash_" + mode + "_" + std::to_string(mir::CurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);

        CrashRun run;
        mir::ChildProcess probe;
        const std::vector<std::string> args{ mode, dir.string() };
        REQUIRE(probe.Start(MIR_CRASH_PROBE_PATH, args));
        run.exitCode = probe.Wait();

        std::ifstream input(dir / "last-error.json", std::ios::binary);
        run.report.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        input.close();
        run.dumpSize = std::filesystem::file_size(dir / "crash.dmp", ec);
        if (ec)
            run.dumpSize = 0;

        std::filesystem::remove_all(dir, ec);
        return run;
    }

    void RequireCrashReport(const CrashRun &run)
    {
        UNSCOPED_INFO(run.report);
        REQUIRE(run.exitCode == mir::kCrashExitCode);
        CHECK(Count(run.report, "\"kind\":\"crash\"") == 1);
        CHECK(run.report.ends_with("}\n"));
        CHECK(run.dumpSize > 0);
    }
}

TEST_CASE("std::terminate on a worker thread reports the exception's message", "[platform][crash]")
{
    const CrashRun run = RunProbe("terminate-worker");
    RequireCrashReport(run);
    CHECK(run.report.find("std::terminate on thread") != std::string::npos);
    CHECK(run.report.find(": worker failed") != std::string::npos);
}

TEST_CASE("std::terminate with no active exception says so", "[platform][crash]")
{
    const CrashRun run = RunProbe("terminate-none");
    RequireCrashReport(run);
    CHECK(run.report.find("no active exception") != std::string::npos);
}

TEST_CASE("Two threads terminating at once write one report", "[platform][crash]")
{
    const CrashRun run = RunProbe("terminate-two");
    RequireCrashReport(run);
    const bool named = run.report.find(": first") != std::string::npos || run.report.find(": second") != std::string::npos;
    CHECK(named);
}

TEST_CASE("An access violation is reported with its code", "[platform][crash]")
{
    const CrashRun run = RunProbe("access-violation");
    RequireCrashReport(run);
    CHECK(run.report.find("access violation 0xC0000005") != std::string::npos);
}

TEST_CASE("A stack overflow is reported with its code", "[platform][crash]")
{
    const CrashRun run = RunProbe("stack-overflow");
    RequireCrashReport(run);
    CHECK(run.report.find("stack overflow 0xC00000FD") != std::string::npos);
}

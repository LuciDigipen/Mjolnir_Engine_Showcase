/**
 * @file LogRepeats.tests.cpp
 * @author Dong Ta
 * @brief Tests for how core/log collapses a warning or error that repeats.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/fs/FileIO.h>
#include <core/log/Log.h>
#include <core/log/LogHistory.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace
{
    std::filesystem::path StartLog(const char *name)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove(path);
        mir::InitLog({ .filePath = path.string(), .consoleLevel = mir::LogLevel::Off });
        mir::LogHistory::Clear();
        return path;
    }

    std::size_t Occurrences(std::string_view text, std::string_view needle)
    {
        std::size_t count = 0;
        for (std::size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + 1))
            ++count;
        return count;
    }

    void LogBadValue(int frames)
    {
        for (int i = 0; i < frames; ++i)
            MIR_LOG_ERROR("bad value on entity {}", 7);
    }
}

TEST_CASE("An error repeated every frame is one entry with a count, written out once", "[log]")
{
    const std::filesystem::path path = StartLog("mir_log_repeats.log");
    const uint64_t errorsBefore = mir::ErrorCount();

    LogBadValue(100);

    const auto entries = mir::LogHistory::Snapshot();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].message == "bad value on entity 7");
    CHECK(entries[0].level == mir::LogLevel::Error);
    CHECK(entries[0].count == 100);
    CHECK(mir::ErrorCount() - errorsBefore == 100);

    mir::ShutdownLog();
    const auto text = mir::ReadText(path);
    REQUIRE(text);
    CHECK(Occurrences(*text, "bad value on entity 7") == 2);
    CHECK(Occurrences(*text, "bad value on entity 7 (repeated 99 more times)") == 1);
}

TEST_CASE("Errors that repeat in turn keep their own entries, and a new one is never hidden", "[log]")
{
    StartLog("mir_log_interleaved.log");

    for (int frame = 0; frame < 50; ++frame)
    {
        for (int entity = 1; entity <= 2; ++entity)
            MIR_LOG_ERROR("entity {} has no mesh", entity);
    }
    MIR_LOG_ERROR("entity {} has no mesh", 3);
    MIR_LOG_WARN("entity {} has no mesh", 1);

    const auto entries = mir::LogHistory::Snapshot();
    REQUIRE(entries.size() == 4);
    CHECK(entries[0].message == "entity 1 has no mesh");
    CHECK(entries[0].count == 50);
    CHECK(entries[1].message == "entity 2 has no mesh");
    CHECK(entries[1].count == 50);
    CHECK(entries[2].message == "entity 3 has no mesh");
    CHECK(entries[2].count == 1);
    CHECK(entries[3].level == mir::LogLevel::Warn);
    CHECK(entries[3].count == 1);
    mir::ShutdownLog();
}

TEST_CASE("Info lines are not collapsed", "[log]")
{
    StartLog("mir_log_info.log");

    for (int i = 0; i < 3; ++i)
        MIR_LOG_INFO("tick");

    const auto entries = mir::LogHistory::Snapshot();
    REQUIRE(entries.size() == 3);
    for (const mir::LogEntry &entry : entries)
        CHECK(entry.count == 1);
    mir::ShutdownLog();
}

TEST_CASE("An error still repeating after the Log panel is cleared shows again", "[log]")
{
    const std::filesystem::path path = StartLog("mir_log_cleared.log");

    LogBadValue(10);
    mir::LogHistory::Clear();
    LogBadValue(5);

    const auto entries = mir::LogHistory::Snapshot();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].count == 5);

    // the file was not cleared, so it still holds one line and the count
    mir::ShutdownLog();
    const auto text = mir::ReadText(path);
    REQUIRE(text);
    CHECK(Occurrences(*text, "bad value on entity 7") == 2);
    CHECK(Occurrences(*text, "(repeated 14 more times)") == 1);
}

TEST_CASE("FlushLog writes the count so far and the error stays collapsed", "[log]")
{
    const std::filesystem::path path = StartLog("mir_log_flush.log");

    LogBadValue(2);
    mir::FlushLog();
    LogBadValue(1);

    CHECK(mir::LogHistory::Snapshot().size() == 1);
    CHECK(mir::LogHistory::Snapshot()[0].count == 3);

    mir::ShutdownLog();
    const auto text = mir::ReadText(path);
    REQUIRE(text);
    CHECK(Occurrences(*text, "bad value on entity 7") == 3);
    CHECK(Occurrences(*text, "(repeated 1 more time)") == 2);
}

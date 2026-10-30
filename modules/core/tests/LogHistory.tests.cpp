/**
 * @file LogHistory.tests.cpp
 * @author Rahul Nair
 * @brief Tests for core/log/LogHistory.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/log/LogHistory.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
    mir::LogEntry MakeEntry(std::string message)
    {
        mir::LogEntry entry;
        entry.level = mir::LogLevel::Info;
        entry.message = std::move(message);
        return entry;
    }
}

TEST_CASE("LogHistory keeps pushed entries in order", "[log]")
{
    mir::LogHistory::SetCapacity(10);
    mir::LogHistory::Clear();

    mir::LogHistory::Push(MakeEntry("first"));
    mir::LogHistory::Push(MakeEntry("second"));

    const auto snapshot = mir::LogHistory::Snapshot();
    REQUIRE(snapshot.size() == 2);
    REQUIRE(snapshot[0].message == "first");
    REQUIRE(snapshot[1].message == "second");
}

TEST_CASE("LogHistory evicts oldest entries past capacity", "[log]")
{
    mir::LogHistory::SetCapacity(3);
    mir::LogHistory::Clear();

    mir::LogHistory::Push(MakeEntry("a"));
    mir::LogHistory::Push(MakeEntry("b"));
    mir::LogHistory::Push(MakeEntry("c"));
    mir::LogHistory::Push(MakeEntry("d"));

    const auto snapshot = mir::LogHistory::Snapshot();
    REQUIRE(snapshot.size() == 3);
    REQUIRE(snapshot[0].message == "b");
    REQUIRE(snapshot[1].message == "c");
    REQUIRE(snapshot[2].message == "d");
}

TEST_CASE("LogHistory::Clear empties the buffer", "[log]")
{
    mir::LogHistory::SetCapacity(10);
    mir::LogHistory::Push(MakeEntry("x"));

    mir::LogHistory::Clear();

    REQUIRE(mir::LogHistory::Snapshot().empty());
}

TEST_CASE("LogHistory::AddRepeat counts onto the entry it names until that entry is gone", "[log]")
{
    mir::LogHistory::SetCapacity(2);
    mir::LogHistory::Clear();

    const uint64_t first = mir::LogHistory::Push(MakeEntry("a"));
    CHECK(mir::LogHistory::AddRepeat(first));
    CHECK(mir::LogHistory::AddRepeat(first));
    CHECK(mir::LogHistory::Snapshot()[0].count == 3);

    mir::LogHistory::Push(MakeEntry("b"));
    const uint64_t last = mir::LogHistory::Push(MakeEntry("c"));
    CHECK_FALSE(mir::LogHistory::AddRepeat(first));
    CHECK(mir::LogHistory::AddRepeat(last));
    CHECK(mir::LogHistory::Snapshot()[1].count == 2);

    mir::LogHistory::Clear();
    CHECK_FALSE(mir::LogHistory::AddRepeat(last));
    CHECK_FALSE(mir::LogHistory::AddRepeat(last + 1));
}

TEST_CASE("LogHistory::SetCapacity trims existing entries", "[log]")
{
    mir::LogHistory::SetCapacity(10);
    mir::LogHistory::Clear();

    mir::LogHistory::Push(MakeEntry("a"));
    mir::LogHistory::Push(MakeEntry("b"));
    mir::LogHistory::Push(MakeEntry("c"));

    mir::LogHistory::SetCapacity(1);

    const auto snapshot = mir::LogHistory::Snapshot();
    REQUIRE(snapshot.size() == 1);
    REQUIRE(snapshot[0].message == "c");
}

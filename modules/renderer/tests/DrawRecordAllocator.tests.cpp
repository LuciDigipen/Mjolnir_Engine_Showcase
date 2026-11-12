/**
 * @file DrawRecordAllocator.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/DrawRecordAllocator.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/DrawRecord.h>
#include <renderer/DrawRecordAllocator.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

TEST_CASE("Only a surface material with a shared record consumes a draw record", "[renderer][material]")
{
    CHECK_FALSE(mir::NeedsDrawRecord(false, false));
    CHECK_FALSE(mir::NeedsDrawRecord(false, true));
    CHECK_FALSE(mir::NeedsDrawRecord(true, false));
    CHECK(mir::NeedsDrawRecord(true, true));
}

TEST_CASE("Draw record indices are sequential and reset per frame", "[renderer][material]")
{
    mir::DrawRecordAllocator allocator;
    CHECK(allocator.Used() == 0);

    REQUIRE(allocator.Acquire() == 0u);
    REQUIRE(allocator.Acquire() == 1u);
    REQUIRE(allocator.Acquire() == 2u);
    CHECK(allocator.Used() == 3);

    allocator.Reset();
    CHECK(allocator.Used() == 0);
    REQUIRE(allocator.Acquire() == 0u);
}

TEST_CASE("Draw record allocation stops at the capacity", "[renderer][material]")
{
    mir::DrawRecordAllocator allocator;
    for (uint32_t index = 0; index < mir::kMaxDrawRecords; ++index)
        allocator.Acquire();

    CHECK(allocator.Used() == mir::kMaxDrawRecords);
    CHECK_FALSE(allocator.Acquire().has_value());

    allocator.Reset();
    REQUIRE(allocator.Acquire() == 0u);
}

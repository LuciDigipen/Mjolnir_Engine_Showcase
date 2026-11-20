/**
 * @file DrawRecordLayout.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/DrawRecord.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/DrawRecord.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

TEST_CASE("The draw record is one aligned row of the record buffer", "[renderer][material]")
{
    CHECK(sizeof(mir::DrawRecord) == 32);
    CHECK(offsetof(mir::DrawRecord, materialOffset) == 0);
    CHECK(offsetof(mir::DrawRecord, materialSize) == 4);
    CHECK(offsetof(mir::DrawRecord, flags) == 8);
    CHECK(offsetof(mir::DrawRecord, reserved) == 12);
    CHECK(offsetof(mir::DrawRecord, terrain) == 16);
    CHECK(sizeof(mir::DrawRecord::terrain) == 4 * sizeof(uint32_t));
    CHECK(mir::kMaxDrawRecords == 65536);
}

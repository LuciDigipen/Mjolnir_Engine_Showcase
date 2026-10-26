/**
 * @file MaterialRecordTable.tests.cpp
 * @author Sumin Park
 * @brief Tests for renderer/MaterialRecordTable.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MaterialRecordTable.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

using namespace mir;

namespace
{
    std::span<const std::byte> Bytes(const std::array<std::byte, 32> &bytes, std::size_t count)
    {
        return std::span<const std::byte>(bytes).first(count);
    }
}

TEST_CASE("Material records take aligned, non-overlapping rows", "[renderer][material]")
{
    MaterialRecordTable table({ .capacityBytes = 4096, .alignment = 16, .framesInFlight = 2 });
    const std::array<std::byte, 32> storage{};

    const MaterialRecordId first = table.Create(Bytes(storage, 32), 64);
    const MaterialRecordId second = table.Create(Bytes(storage, 16), 0);

    REQUIRE(table.Valid(first));
    REQUIRE(table.Valid(second));
    CHECK(table.Offset(first) % 64 == 0);
    CHECK(table.Offset(second) % 16 == 0);
    CHECK(table.Offset(first) != table.Offset(second));
    CHECK(table.Size(first) == 32);
    CHECK(table.Size(second) == 16);
    CHECK(table.UsedBytes() == 3 * 64 + 3 * 16);
}

TEST_CASE("A published material record keeps its old copy until its frames complete", "[renderer][material]")
{
    MaterialRecordTable table({ .capacityBytes = 4096, .alignment = 16, .framesInFlight = 2 });

    std::array<std::byte, 32> first{};
    std::array<std::byte, 32> second{};
    std::array<std::byte, 32> third{};
    std::array<std::byte, 32> fourth{};
    first.fill(std::byte{ 1 });
    second.fill(std::byte{ 2 });
    third.fill(std::byte{ 3 });
    fourth.fill(std::byte{ 4 });

    const MaterialRecordId id = table.Create(first, 32);
    REQUIRE(table.Valid(id));

    REQUIRE(table.Update(id, second));
    REQUIRE(table.Publish(0, 10));
    const uint32_t secondOffset = table.Offset(id);
    CHECK(table.PublishedBytes(id).front() == std::byte{ 2 });

    REQUIRE(table.Update(id, third));
    REQUIRE(table.Publish(0, 11));
    const uint32_t thirdOffset = table.Offset(id);
    CHECK(thirdOffset != secondOffset);
    CHECK(table.PublishedBytes(id).front() == std::byte{ 3 });

    REQUIRE(table.Update(id, fourth));
    CHECK_FALSE(table.Publish(0, 12));
    CHECK(table.Offset(id) == thirdOffset);
    CHECK(table.PublishedBytes(id).front() == std::byte{ 3 });

    REQUIRE(table.Publish(10, 13));
    CHECK(table.Offset(id) != thirdOffset);
    CHECK(table.PublishedBytes(id).front() == std::byte{ 4 });
}

TEST_CASE("A material record rejects a different byte count and a stale id", "[renderer][material]")
{
    MaterialRecordTable table({ .capacityBytes = 256, .alignment = 16, .framesInFlight = 2 });
    const std::array<std::byte, 32> storage{};

    const MaterialRecordId id = table.Create(Bytes(storage, 32));
    CHECK_FALSE(table.Update(id, Bytes(storage, 16)));
    REQUIRE(table.Release(id));
    CHECK_FALSE(table.Valid(id));
    CHECK(table.Offset(id) == kInvalidMaterialRecordOffset);
    CHECK_FALSE(table.Update(id, Bytes(storage, 32)));
    CHECK_FALSE(table.Release(id));
}

TEST_CASE("Released material record memory comes back after its frames complete", "[renderer][material]")
{
    constexpr uint32_t kBytes = 32;
    constexpr uint32_t kAlignment = 32;
    constexpr uint32_t kBlock = kBytes * MaterialRecordTable::kCopies;
    MaterialRecordTable table({ .capacityBytes = kBlock, .alignment = kAlignment, .framesInFlight = 2 });
    const std::array<std::byte, kBytes> storage{};

    const MaterialRecordId first = table.Create(storage, kAlignment);
    REQUIRE(table.Valid(first));
    REQUIRE(table.Release(first));
    CHECK(table.Create(storage, kAlignment).IsNull());

    REQUIRE_FALSE(table.Publish(2, 2));
    const MaterialRecordId second = table.Create(storage, kAlignment);
    REQUIRE(table.Valid(second));
    CHECK(second.mIndex == first.mIndex);
    CHECK(second.mGeneration != first.mGeneration);
    CHECK(table.Offset(second) == 0);
}

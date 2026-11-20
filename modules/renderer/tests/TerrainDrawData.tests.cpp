/**
 * @file TerrainDrawData.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/TerrainDrawData and the terrain words of a draw record.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/DrawRecord.h>
#include <renderer/TerrainDrawData.h>
#include <renderer/ViewSubmission.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <vector>

using namespace mir;

namespace
{
    WorldDrawItem ItemAtOrigin()
    {
        WorldDrawItem item;
        item.pose = WorldPose{};
        return item;
    }

    std::array<uint32_t, 4> RecordWords(const WorldDrawItem &item)
    {
        std::vector<DrawItem> view;
        BuildViewDrawItems(std::span(&item, 1), WorldPosition{}, glm::mat4(1.0f), view);
        REQUIRE(view.size() == 1);
        CHECK(view[0].terrain == item.terrain);
        const MaterialRecordRef ref{.mOffset = 1024, .mSize = 48, .mFlags = 3, .mReserved = 0};
        const DrawRecord record = MakeDrawRecord(ref, view[0].terrain);
        CHECK(record.materialOffset == 1024);
        CHECK(record.materialSize == 48);
        CHECK(record.flags == 3);
        return {record.terrain[0], record.terrain[1], record.terrain[2], record.terrain[3]};
    }
}

TEST_CASE("Terrain draw words round-trip the image, sampler, slots and reciprocal tile size",
          "[renderer][terrain][drawdata]")
{
    TerrainDrawData data;
    data.weightImageRow = 41;
    data.weightSamplerRow = 7;
    data.slots = {2, 0, kTerrainSlotUnused, 3};
    data.invFootprint = 1.0f / 256.0f;

    const std::array<uint32_t, 4> words = data.Pack();

    CHECK(words[0] == 42);
    CHECK(words[1] == 7);
    CHECK(words[2] == 0x03FF0002u);
    CHECK(std::bit_cast<float>(words[3]) == data.invFootprint);

    const TerrainDrawData back = TerrainDrawData::Unpack(words);
    CHECK(back.weightImageRow == data.weightImageRow);
    CHECK(back.weightSamplerRow == data.weightSamplerRow);
    CHECK(back.slots == data.slots);
    CHECK(back.invFootprint == data.invFootprint);
}

TEST_CASE("A draw without weights packs four zero words and unpacks as none", "[renderer][terrain][drawdata]")
{
    const TerrainDrawData data;
    CHECK(data.Pack() == std::array<uint32_t, 4>{0, 0, 0, 0});

    const TerrainDrawData back = TerrainDrawData::Unpack(kNoTerrainDrawWords);
    CHECK(back.weightImageRow == kTerrainWeightImageNone);
    CHECK(back.weightSamplerRow == 0);
    CHECK(back.invFootprint == 0.0f);
}

TEST_CASE("A WorldDrawItem's terrain words reach its draw record and an item without them writes zeros",
          "[renderer][terrain][drawdata]")
{
    WorldDrawItem weighted = ItemAtOrigin();
    const std::array<uint32_t, 4> words{5, 6, 0xFFFF0203u, 0x3B800000u};
    weighted.terrain = words;
    CHECK(RecordWords(weighted) == words);

    CHECK(RecordWords(ItemAtOrigin()) == std::array<uint32_t, 4>{0, 0, 0, 0});
}

TEST_CASE("The instance table maps a record's weight layers to material layers", "[renderer][terrain][drawdata]")
{
    const std::array<uint8_t, 4> record{0, 1, kTerrainSlotUnused, 2};

    CHECK(MapTerrainSlots(record, kDefaultTerrainSlotMap) ==
          std::array<uint8_t, 4>{0, 0, kTerrainSlotUnused, 0});

    std::array<uint8_t, 8> table{};
    table[0] = 1;
    table[1] = 3;
    table[2] = 2;
    CHECK(MapTerrainSlots(record, table) == std::array<uint8_t, 4>{1, 3, kTerrainSlotUnused, 2});
}

TEST_CASE("The weight UV maps tile edges to the first and last texel centres", "[renderer][terrain][drawdata]")
{
    const uint32_t side = 65;
    const float tileSize = 256.0f;
    const float invFootprint = 1.0f / tileSize;

    const std::array<float, 2> first = TerrainWeightUv(0.0f, 0.0f, invFootprint, side);
    CHECK(first[0] == 0.5f / float(side));
    CHECK(first[1] == 0.5f / float(side));

    const std::array<float, 2> last = TerrainWeightUv(tileSize, tileSize, invFootprint, side);
    CHECK(last[0] == (float(side) - 0.5f) / float(side));
    CHECK(last[1] == (float(side) - 0.5f) / float(side));

    const std::array<float, 2> swapped = TerrainWeightUv(0.0f, tileSize, invFootprint, side);
    CHECK(swapped[0] == first[0]);
    CHECK(swapped[1] == last[1]);
}

TEST_CASE("Channel slots accumulate into material layers and normalise by their sum", "[renderer][terrain][drawdata]")
{
    const std::array<float, 4> sameLayer =
        AccumulateTerrainLayerWeights({0, 0, kTerrainSlotUnused, kTerrainSlotUnused}, {0.25f, 0.5f, 0.0f, 0.0f});
    CHECK(sameLayer == std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f});

    const std::array<float, 4> split =
        AccumulateTerrainLayerWeights({0, 1, kTerrainSlotUnused, kTerrainSlotUnused}, {0.2f, 0.3f, 0.5f, 0.0f});
    CHECK(split[0] == Catch::Approx(0.4f));
    CHECK(split[1] == Catch::Approx(0.6f));
    CHECK(split[2] == 0.0f);
    CHECK(split[3] == 0.0f);

    const std::array<float, 4> zero =
        AccumulateTerrainLayerWeights({0, 1, kTerrainSlotUnused, kTerrainSlotUnused}, {0.0f, 0.0f, 0.0f, 0.0f});
    CHECK(zero == std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f});
}

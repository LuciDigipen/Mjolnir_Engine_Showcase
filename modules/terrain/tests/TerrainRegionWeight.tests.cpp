/**
 * @file TerrainRegionWeight.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainRegionWeights and Region weight payloads.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"

#include <terrain/TerrainRegionWeights.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using Code = TerrainPackErrorCode;
    using Names = std::vector<std::string>;

    TerrainWeightTile ConstantTile(TileCoord tile, int32_t intervals, const Names &layers,
                                   const std::vector<float> &perLayer)
    {
        TerrainWeightTile weights;
        weights.tile = tile;
        weights.intervals = intervals;
        weights.layers = layers;
        const size_t count = weights.SampleCount();
        weights.weights.resize(count * perLayer.size());
        for (size_t l = 0; l < perLayer.size(); ++l)
            for (size_t sample = 0; sample < count; ++sample)
                weights.weights[l * count + sample] = perLayer[l];
        return weights;
    }

    struct TileWeights
    {
        std::vector<TerrainWeightTile> tiles;

        const TerrainWeightTile *operator()(TileCoord at) const
        {
            const auto found = std::find_if(tiles.begin(), tiles.end(),
                                            [&](const TerrainWeightTile &tile)
                                            {
                                                return tile.tile == at;
                                            });
            return found == tiles.end() ? nullptr : &*found;
        }
    };

    std::array<uint8_t, kTerrainWeightSlots> Channels(const TerrainRegionWeightImage &image, size_t texel)
    {
        std::array<uint8_t, kTerrainWeightSlots> channels{};
        for (size_t s = 0; s < kTerrainWeightSlots; ++s)
            channels[s] = std::to_integer<uint8_t>(image.bytes[texel * kTerrainWeightSlots + s]);
        return channels;
    }

    bool SameImage(const TerrainRegionWeightImage &a, const TerrainRegionWeightImage &b)
    {
        return a.region == b.region && a.side == b.side && a.slots == b.slots && a.bytes == b.bytes;
    }

    struct WeightedRelease
    {
        TerrainLayout layout = EdgeLayout();
        std::vector<TileHeights> tiles = BuildTiles(layout);
        Names table{"dirt", "sand", "rock", "snow"};
        std::vector<TerrainWeightTile> weights;
        TerrainRegionWrites regions = BuildTestRegions(layout, tiles, 2, 2);
        std::vector<TerrainRegionWeightImage> images;
        TerrainEncodedRelease encoded;
        std::optional<TerrainIndex> index;

        WeightedRelease()
        {
            const TileRange range = layout.Tiles();
            for (int32_t z = range.min.z; z <= range.max.z; ++z)
                for (int32_t x = range.min.x; x <= range.max.x; ++x)
                {
                    const size_t pick = size_t((x - range.min.x) + (z - range.min.z)) % table.size();
                    std::vector<float> perLayer(table.size(), 0.0f);
                    for (size_t l = 0; l < table.size(); ++l)
                        perLayer[l] = l == pick ? 1.0f : 0.0f;
                    weights.push_back(ConstantTile({x, z}, layout.IntervalsPerTile(), table, perLayer));
                }
            REQUIRE(regions.grid);
            const TileWeights lookup{weights};
            TerrainResult<std::vector<TerrainRegionWeightImage>> built =
                BuildTerrainRegionWeights(*regions.grid, table, [&](TileCoord tile) { return lookup(tile); },
                                          {.side = 3});
            INFO(built.error.message);
            REQUIRE(built);
            images = std::move(*built.value);

            TerrainWeightsWrites writes;
            writes.layers = table;
            writes.fingerprint = 0xBEEF;
            writes.tiles = weights;
            writes.regionImages = images;
            encoded = EncodeWithRegionsAndWeights(Info(TerrainAsset("terrain/edge.terrain.json"), 0xE1), layout,
                                                  tiles, regions, writes, {.chunkTiles = 2});
            const TerrainResult<TerrainIndex> parsed = ParseTerrainIndex(encoded.index);
            INFO(parsed.error.message);
            REQUIRE(parsed);
            index = *parsed.value;
        }
    };
}

TEST_CASE("Region weight images follow the tile weights", "[terrain][region][weights]")
{
    const Names table{"a", "b"};

    SECTION("a texel inside a single-layer area is that layer at 255")
    {
        const TerrainLayout layout = *TerrainLayout::CreateFromTileSize(2.0, 1.0, {0.0, 0.0, 4.0, 4.0});
        TileWeights tiles;
        tiles.tiles.push_back(ConstantTile({0, 0}, 2, table, {1.0f, 0.0f}));
        tiles.tiles.push_back(ConstantTile({1, 0}, 2, table, {0.0f, 1.0f}));
        tiles.tiles.push_back(ConstantTile({0, 1}, 2, table, {0.0f, 1.0f}));
        tiles.tiles.push_back(ConstantTile({1, 1}, 2, table, {0.0f, 1.0f}));
        const std::optional<TerrainRegionGrid> grid = TerrainRegionGrid::Create(layout, 1, 2, 1);
        REQUIRE(grid);

        const TerrainResult<std::vector<TerrainRegionWeightImage>> images =
            BuildTerrainRegionWeights(*grid, table, [&](TileCoord tile) { return tiles(tile); }, {.side = 3});
        REQUIRE(images);
        const TerrainRegionWeightImage &image = images.value->front();
        CHECK(image.slots == std::array<uint8_t, kTerrainWeightSlots>{0, 1, 0xFF, 0xFF});
        CHECK(Channels(image, 0) == std::array<uint8_t, kTerrainWeightSlots>{255, 0, 0, 0});
    }

    SECTION("a texel over a tile boundary averages both layers")
    {
        const TerrainLayout layout = *TerrainLayout::CreateFromTileSize(2.0, 1.0, {0.0, 0.0, 4.0, 4.0});
        TileWeights tiles;
        tiles.tiles.push_back(ConstantTile({0, 0}, 2, table, {1.0f, 0.0f}));
        tiles.tiles.push_back(ConstantTile({1, 0}, 2, table, {0.0f, 1.0f}));
        tiles.tiles.push_back(ConstantTile({0, 1}, 2, table, {1.0f, 0.0f}));
        tiles.tiles.push_back(ConstantTile({1, 1}, 2, table, {0.0f, 1.0f}));
        const std::optional<TerrainRegionGrid> grid = TerrainRegionGrid::Create(layout, 1, 2, 1);
        REQUIRE(grid);

        const TerrainResult<std::vector<TerrainRegionWeightImage>> images =
            BuildTerrainRegionWeights(*grid, table, [&](TileCoord tile) { return tiles(tile); }, {.side = 2});
        REQUIRE(images);
        const TerrainRegionWeightImage &image = images.value->front();
        CHECK(Channels(image, 1) == std::array<uint8_t, kTerrainWeightSlots>{128, 127, 0, 0});
    }

    SECTION("a texel outside the map takes the nearest sample")
    {
        const TerrainLayout layout = *TerrainLayout::CreateFromTileSize(2.0, 1.0, {0.0, 0.0, 3.0, 3.0});
        TileWeights tiles;
        tiles.tiles.push_back(ConstantTile({0, 0}, 2, table, {0.0f, 1.0f}));
        tiles.tiles.push_back(ConstantTile({1, 0}, 2, table, {0.0f, 1.0f}));
        tiles.tiles.push_back(ConstantTile({0, 1}, 2, table, {0.0f, 1.0f}));
        tiles.tiles.push_back(ConstantTile({1, 1}, 2, table, {1.0f, 0.0f}));
        const std::optional<TerrainRegionGrid> grid = TerrainRegionGrid::Create(layout, 2, 2, 1);
        REQUIRE(grid);
        REQUIRE(grid->Levels() == 1);

        const TerrainResult<std::vector<TerrainRegionWeightImage>> images =
            BuildTerrainRegionWeights(*grid, table, [&](TileCoord tile) { return tiles(tile); }, {.side = 5});
        REQUIRE(images);
        const TerrainRegionWeightImage &image = images.value->front();
        CHECK(Channels(image, 0) == std::array<uint8_t, kTerrainWeightSlots>{0, 255, 0, 0});
        CHECK(Channels(image, 24) == std::array<uint8_t, kTerrainWeightSlots>{255, 0, 0, 0});
    }

    SECTION("the thread count does not change the images")
    {
        const TerrainLayout layout = *TerrainLayout::CreateFromTileSize(2.0, 1.0, {0.0, 0.0, 4.0, 4.0});
        TileWeights tiles;
        std::vector<std::vector<float>> split{{1.0f, 0.0f}, {0.4f, 0.6f}, {0.0f, 1.0f}, {0.7f, 0.3f}};
        const TileRange range = layout.Tiles();
        size_t index = 0;
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
                tiles.tiles.push_back(ConstantTile({x, z}, 2, table, split[index++ % split.size()]));
        const std::optional<TerrainRegionGrid> grid = TerrainRegionGrid::Create(layout, 1, 2, 1);
        REQUIRE(grid);

        const auto lookup = [&](TileCoord tile) { return tiles(tile); };
        const TerrainResult<std::vector<TerrainRegionWeightImage>> single =
            BuildTerrainRegionWeights(*grid, table, lookup, {.side = 3, .workers = 1});
        const TerrainResult<std::vector<TerrainRegionWeightImage>> many =
            BuildTerrainRegionWeights(*grid, table, lookup, {.side = 3, .workers = 4});
        REQUIRE(single);
        REQUIRE(many);
        REQUIRE(single.value->size() == many.value->size());
        for (size_t i = 0; i < single.value->size(); ++i)
            CHECK(SameImage((*single.value)[i], (*many.value)[i]));
    }

    SECTION("more than four layers keep the four largest")
    {
        const Names many{"a", "b", "c", "d", "e", "f"};
        const TerrainLayout layout = *TerrainLayout::CreateFromTileSize(2.0, 1.0, {0.0, 0.0, 4.0, 4.0});
        TileWeights tiles;
        const TileRange range = layout.Tiles();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
                tiles.tiles.push_back(
                    ConstantTile({x, z}, 2, many, {0.4f, 0.3f, 0.2f, 0.06f, 0.03f, 0.01f}));
        const std::optional<TerrainRegionGrid> grid = TerrainRegionGrid::Create(layout, 2, 2, 1);
        REQUIRE(grid);

        const TerrainResult<std::vector<TerrainRegionWeightImage>> images =
            BuildTerrainRegionWeights(*grid, many, [&](TileCoord tile) { return tiles(tile); }, {.side = 2});
        REQUIRE(images);
        const TerrainRegionWeightImage &image = images.value->front();
        CHECK(image.slots == std::array<uint8_t, kTerrainWeightSlots>{0, 1, 2, 3});
        for (size_t texel = 0; texel < 4; ++texel)
        {
            const std::array<uint8_t, kTerrainWeightSlots> channels = Channels(image, texel);
            CHECK(channels == std::array<uint8_t, kTerrainWeightSlots>{106, 80, 53, 16});
        }
    }

    SECTION("a tile without weights is refused")
    {
        const TerrainLayout layout = *TerrainLayout::CreateFromTileSize(2.0, 1.0, {0.0, 0.0, 4.0, 4.0});
        TileWeights tiles;
        tiles.tiles.push_back(ConstantTile({0, 0}, 2, table, {1.0f, 0.0f}));
        const std::optional<TerrainRegionGrid> grid = TerrainRegionGrid::Create(layout, 1, 2, 1);
        REQUIRE(grid);

        const TerrainResult<std::vector<TerrainRegionWeightImage>> images =
            BuildTerrainRegionWeights(*grid, table, [&](TileCoord tile) { return tiles(tile); }, {.side = 2});
        REQUIRE_FALSE(images);
        CHECK(images.error.code == Code::InvalidInput);
        CHECK(Mentions(images.error, "has no weights"));
    }
}

TEST_CASE("Region weight payloads round-trip through a weighted release", "[terrain][pack][region][weights]")
{
    const WeightedRelease fixture;
    REQUIRE(fixture.index);
    const TerrainIndex &index = *fixture.index;
    REQUIRE(index.regionGrid);
    CHECK(index.formatMinor == kTerrainWeightsFormatMinor);
    CHECK(index.regionWeights.size() == index.regionGrid->RegionCount());
    CHECK(index.regionWeights.size() == fixture.images.size());

    const TerrainEncodedRelease plain = EncodeWithRegions(
        Info(TerrainAsset("terrain/edge.terrain.json"), 0xE1), fixture.layout, fixture.tiles, fixture.regions,
        {.chunkTiles = 2});
    REQUIRE(plain.packs.size() == fixture.encoded.packs.size());
    for (size_t p = 0; p < plain.packs.size(); ++p)
        CHECK(plain.packs[p].bytes == fixture.encoded.packs[p].bytes);
    CHECK(plain.heightsHash == fixture.encoded.heightsHash);

    auto [source, handle] = OpenInMemory(fixture.encoded);
    CHECK(handle.VerifyAllPacks().code == Code::None);
    for (size_t i = 0; i < fixture.images.size(); ++i)
    {
        const TerrainRegionWeightImage &image = fixture.images[i];
        const TerrainRegionWeightRecord *record = index.FindRegionWeight(image.region);
        REQUIRE(record);
        CHECK(record->side == image.side);
        CHECK(record->slots == image.slots);
        const TerrainResult<TerrainDecodedWeights> decoded = handle.ReadRegionWeights(image.region);
        INFO(decoded.error.message);
        REQUIRE(decoded);
        CHECK(decoded.value->intervals == int32_t(image.side) - 1);
        CHECK(decoded.value->slots == image.slots);
        CHECK(decoded.value->bytes == image.bytes);
    }
}

TEST_CASE("A weighted release without a Region grid has no Region weight records",
          "[terrain][pack][region][weights]")
{
    const TerrainLayout layout = EdgeLayout();
    const std::vector<TileHeights> tiles = BuildTiles(layout);
    const Names table{"dirt", "sand"};
    std::vector<TerrainWeightTile> weights;
    const TileRange range = layout.Tiles();
    for (int32_t z = range.min.z; z <= range.max.z; ++z)
        for (int32_t x = range.min.x; x <= range.max.x; ++x)
            weights.push_back(ConstantTile({x, z}, layout.IntervalsPerTile(), table, {0.5f, 0.5f}));
    TerrainWeightsWrites writes;
    writes.layers = table;
    writes.fingerprint = 1;
    writes.tiles = weights;
    const TerrainEncodedRelease encoded = EncodeWithWeights(
        Info(TerrainAsset("terrain/edge.terrain.json"), 0xE1), layout, tiles, writes, {.chunkTiles = 2});
    auto [source, handle] = OpenInMemory(encoded);
    CHECK(handle.Index().formatMinor == kTerrainWeightsFormatMinor);
    CHECK(handle.Index().regionWeights.empty());
    CHECK(handle.VerifyAllPacks().code == Code::None);
}

TEST_CASE("A Region weight index rejects records that disagree with the grid", "[terrain][pack][region][weights]")
{
    const WeightedRelease fixture;
    const TerrainIndex &index = *fixture.index;
    REQUIRE(index.regionWeights.size() >= 3);

    auto refused = [&](auto patch, std::string_view text)
    {
        std::vector<std::byte> bytes = fixture.encoded.index;
        patch(bytes);
        ResealIndex(bytes);
        const TerrainResult<TerrainIndex> parsed = ParseTerrainIndex(bytes);
        INFO(parsed.error.message);
        CHECK_FALSE(parsed);
        CHECK(parsed.error.code == Code::Malformed);
        CHECK(Mentions(parsed.error, text));
    };

    SECTION("a missing Region weight record")
    {
        refused(
            [&](auto &b)
            {
                StoreU32(b, terrainformat::index::kRegionWeightCount,
                         static_cast<uint32_t>(index.regionWeights.size() - 1));
            },
            "missing or extra");
    }
    SECTION("a duplicate Region weight record")
    {
        refused(
            [&](auto &b)
            {
                const size_t from = RegionWeightRecordAt(index, 0);
                const size_t to = RegionWeightRecordAt(index, 1);
                for (size_t byte = 0; byte < 12; ++byte)
                    b[to + byte] = b[from + byte];
            },
            "repeat Region");
    }
    SECTION("Region weight records out of order")
    {
        refused(
            [&](auto &b)
            {
                const size_t first = RegionWeightRecordAt(index, 0);
                const size_t second = RegionWeightRecordAt(index, 1);
                std::array<std::byte, 12> swap{};
                for (size_t byte = 0; byte < 12; ++byte)
                    swap[byte] = b[second + byte];
                for (size_t byte = 0; byte < 12; ++byte)
                    b[second + byte] = b[first + byte];
                for (size_t byte = 0; byte < 12; ++byte)
                    b[first + byte] = swap[byte];
            },
            "order");
    }
    SECTION("a bad Region image side")
    {
        refused(
            [&](auto &b)
            {
                StoreU32(b, RegionWeightRecordAt(index, 0) + terrainformat::regionWeightRecord::kSide, 1);
            },
            "side 1");
    }
    SECTION("a slot outside the name table")
    {
        refused(
            [&](auto &b)
            {
                b[RegionWeightRecordAt(index, 0) + terrainformat::regionWeightRecord::kSlots] = std::byte{4};
            },
            "the table has 4");
    }
    SECTION("a payload length that is not an image")
    {
        refused(
            [&](auto &b)
            {
                StoreU64(b, RegionWeightRecordAt(index, 0) + terrainformat::regionWeightRecord::kLength,
                         LoadU64(b, RegionWeightRecordAt(index, 0) + terrainformat::regionWeightRecord::kLength) - 4);
            },
            "payload length");
    }
}

TEST_CASE("A Region weight payload is checked when it is decoded", "[terrain][pack][region][weights]")
{
    const WeightedRelease fixture;
    auto [source, handle] = OpenInMemory(fixture.encoded);
    const TerrainIndex &index = handle.Index();
    const TerrainRegionWeightRecord &record = index.regionWeights.front();
    const std::string pack = TerrainWeightPackFileName(index.weightPacks[record.pack].chunk);
    const std::optional<std::vector<std::byte>> original = source->Get(pack);
    REQUIRE(original);
    std::vector<std::byte> bytes = *original;
    std::vector<std::byte> payload(bytes.begin() + record.offset, bytes.begin() + record.offset + record.length);
    payload[0] ^= std::byte{0x01};
    const TerrainResult<TerrainDecodedWeights> decoded =
        DecodeTerrainRegionWeightPayload(index, record, payload);
    CHECK_FALSE(decoded);
    CHECK(decoded.error.code == Code::Corrupt);
    CHECK(Mentions(decoded.error, "checksum"));
}

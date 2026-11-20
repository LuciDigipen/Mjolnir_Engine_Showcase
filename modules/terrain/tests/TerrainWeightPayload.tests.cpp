/**
 * @file TerrainWeightPayload.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainPackFormat and terrain/TerrainPackWriter weight payloads.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using Code = TerrainPackErrorCode;

    TerrainWeightTile ConstantWeights(TileCoord tile, int32_t intervals, std::vector<std::string> layers,
                                      std::vector<float> layerWeights)
    {
        TerrainWeightTile weights;
        weights.tile = tile;
        weights.intervals = intervals;
        weights.layers = std::move(layers);
        const size_t count = weights.SampleCount();
        weights.weights.resize(count * layerWeights.size());
        for (size_t l = 0; l < layerWeights.size(); ++l)
            for (size_t sample = 0; sample < count; ++sample)
                weights.weights[l * count + sample] = layerWeights[l];
        return weights;
    }

    std::array<uint8_t, kTerrainWeightSlots> Channels(const TerrainEncodedWeights &encoded, size_t texel)
    {
        std::array<uint8_t, kTerrainWeightSlots> channels{};
        for (size_t s = 0; s < kTerrainWeightSlots; ++s)
            channels[s] = std::to_integer<uint8_t>(encoded.bytes[texel * kTerrainWeightSlots + s]);
        return channels;
    }

    std::vector<TerrainWeightTile> BuildWeightTiles(const TerrainLayout &layout,
                                                    const std::vector<std::string> &layers)
    {
        std::vector<TerrainWeightTile> tiles;
        const TileRange range = layout.Tiles();
        const int32_t n = layout.IntervalsPerTile();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                TerrainWeightTile weights;
                weights.tile = {x, z};
                weights.intervals = n;
                weights.layers = layers;
                const size_t count = weights.SampleCount();
                weights.weights.resize(count * layers.size());
                for (int32_t k = 0; k <= n; ++k)
                    for (int32_t i = 0; i <= n; ++i)
                    {
                        const size_t sample = size_t(k) * size_t(n + 1) + size_t(i);
                        const float u = float((i * 13 + k * 7) % 11) / 10.0f;
                        const float v = float((i * 5 + k * 17) % 11) / 10.0f;
                        const float w0 = u * 0.3f;
                        const float w1 = v * 0.2f;
                        const float w2 = (1.0f - u) * 0.1f;
                        const float w3 = 1.0f - (w0 + w1 + w2);
                        const std::array<float, 4> perLayer{w0, w1, w2, w3};
                        for (size_t l = 0; l < perLayer.size(); ++l)
                            weights.weights[l * count + sample] = perLayer[l];
                    }
                tiles.push_back(std::move(weights));
            }
        return tiles;
    }

    bool SameHeights(const TerrainTileRecord &a, const TerrainTileRecord &b)
    {
        return a.tile == b.tile && a.pack == b.pack && a.samplesPerSide == b.samplesPerSide &&
               a.offset == b.offset && a.length == b.length && a.checksum == b.checksum &&
               a.sampleSpacing == b.sampleSpacing && a.minHeight == b.minHeight && a.maxHeight == b.maxHeight &&
               a.validExtent == b.validExtent && a.layer == b.layer && a.kind == b.kind && a.level == b.level &&
               a.source == b.source && a.maxDeviation == b.maxDeviation && a.sourceChecksum == b.sourceChecksum &&
               a.topologyHash == b.topologyHash && a.vertexCount == b.vertexCount && a.indexCount == b.indexCount;
    }

    struct WeightedRelease
    {
        TerrainLayout layout = EdgeLayout();
        std::vector<TileHeights> tiles = BuildTiles(layout);
        std::vector<std::string> table{"dirt", "sand", "rock", "snow"};
        std::vector<TerrainWeightTile> weights = BuildWeightTiles(layout, table);
        TerrainReleaseInfo info = Info(TerrainAsset("terrain/edge.terrain.json"), 0xE1);
        TerrainEncodedRelease encoded;
        std::optional<TerrainIndex> index;

        WeightedRelease()
        {
            TerrainWeightsWrites writes;
            writes.layers = table;
            writes.fingerprint = 0xF00D;
            writes.tiles = weights;
            encoded = EncodeWithWeights(info, layout, tiles, writes, {.chunkTiles = 2});
            const TerrainResult<TerrainIndex> parsed = ParseTerrainIndex(encoded.index);
            INFO(parsed.error.message);
            REQUIRE(parsed);
            index = *parsed.value;
        }
    };
}

TEST_CASE("Weight encoding keeps the four heaviest layers and sums every texel to 255", "[terrain][pack][weights]")
{
    const std::vector<std::string> table{"dirt", "sand", "rock", "snow", "moss"};

    SECTION("two equal layers split 128 and 127")
    {
        const TerrainWeightTile tile = ConstantWeights({0, 0}, 2, {"dirt", "sand"}, {0.5f, 0.5f});
        const TerrainResult<TerrainEncodedWeights> encoded = EncodeTerrainWeightPayload(tile, table);
        REQUIRE(encoded);
        CHECK(encoded.value->slots == std::array<uint8_t, kTerrainWeightSlots>{0, 1, 0xFF, 0xFF});
        for (size_t texel = 0; texel < tile.SampleCount(); ++texel)
            CHECK(Channels(*encoded.value, texel) == std::array<uint8_t, kTerrainWeightSlots>{128, 127, 0, 0});
    }

    SECTION("three equal layers give 85 each")
    {
        const TerrainWeightTile tile = ConstantWeights({0, 0}, 2, {"dirt", "sand", "rock"}, {1.0f / 3.0f,
                                                                                           1.0f / 3.0f,
                                                                                           1.0f / 3.0f});
        const TerrainResult<TerrainEncodedWeights> encoded = EncodeTerrainWeightPayload(tile, table);
        REQUIRE(encoded);
        CHECK(encoded.value->slots == std::array<uint8_t, kTerrainWeightSlots>{0, 1, 2, 0xFF});
        for (size_t texel = 0; texel < tile.SampleCount(); ++texel)
            CHECK(Channels(*encoded.value, texel) == std::array<uint8_t, kTerrainWeightSlots>{85, 85, 85, 0});
    }

    SECTION("largest remainder breaks the half units")
    {
        const TerrainWeightTile tile = ConstantWeights({0, 0}, 2, {"dirt", "sand", "rock", "snow"},
                                                       {0.7f, 0.2f, 0.1f, 0.0f});
        const TerrainResult<TerrainEncodedWeights> encoded = EncodeTerrainWeightPayload(tile, table);
        REQUIRE(encoded);
        CHECK(encoded.value->slots == std::array<uint8_t, kTerrainWeightSlots>{0, 1, 2, 3});
        for (size_t texel = 0; texel < tile.SampleCount(); ++texel)
            CHECK(Channels(*encoded.value, texel) == std::array<uint8_t, kTerrainWeightSlots>{178, 51, 26, 0});
    }

    SECTION("a fifth layer is dropped and the rest renormalised")
    {
        const TerrainWeightTile tile = ConstantWeights({0, 0}, 2, {"dirt", "sand", "rock", "snow", "moss"},
                                                       {0.4f, 0.3f, 0.2f, 0.05f, 0.05f});
        const TerrainResult<TerrainEncodedWeights> encoded = EncodeTerrainWeightPayload(tile, table);
        REQUIRE(encoded);
        CHECK(encoded.value->slots == std::array<uint8_t, kTerrainWeightSlots>{0, 1, 2, 3});
        for (size_t texel = 0; texel < tile.SampleCount(); ++texel)
            CHECK(Channels(*encoded.value, texel) == std::array<uint8_t, kTerrainWeightSlots>{107, 81, 54, 13});
    }

    SECTION("a texel whose kept weights are all zero falls to its first slot")
    {
        TerrainWeightTile tile;
        tile.tile = {0, 0};
        tile.intervals = 2;
        tile.layers = {"dirt", "sand", "rock", "snow", "moss"};
        const size_t count = tile.SampleCount();
        tile.weights.resize(count * tile.layers.size());
        for (size_t sample = 0; sample < count; ++sample)
            for (size_t l = 0; l < tile.layers.size(); ++l)
                tile.weights[l * count + sample] = l < 4 ? (sample == 0 ? 0.0f : 0.25f) : (sample == 0 ? 1.0f : 0.0f);
        const TerrainResult<TerrainEncodedWeights> encoded = EncodeTerrainWeightPayload(tile, table);
        REQUIRE(encoded);
        CHECK(encoded.value->slots == std::array<uint8_t, kTerrainWeightSlots>{0, 1, 2, 3});
        CHECK(Channels(*encoded.value, 0) == std::array<uint8_t, kTerrainWeightSlots>{255, 0, 0, 0});
        CHECK(Channels(*encoded.value, 1) == std::array<uint8_t, kTerrainWeightSlots>{64, 64, 64, 63});
    }

    SECTION("tied tile sums keep the lower table indices and slots follow the table")
    {
        const TerrainWeightTile tile =
            ConstantWeights({0, 0}, 2, {"moss", "snow", "rock", "sand", "dirt"}, {0.2f, 0.2f, 0.2f, 0.2f, 0.2f});
        const TerrainResult<TerrainEncodedWeights> encoded = EncodeTerrainWeightPayload(tile, table);
        REQUIRE(encoded);
        CHECK(encoded.value->slots == std::array<uint8_t, kTerrainWeightSlots>{0, 1, 2, 3});
        for (size_t texel = 0; texel < tile.SampleCount(); ++texel)
            CHECK(Channels(*encoded.value, texel) == std::array<uint8_t, kTerrainWeightSlots>{64, 64, 64, 63});
    }

    SECTION("slots name their layer table entry")
    {
        const TerrainWeightTile tile = ConstantWeights({0, 0}, 2, {"rock", "sand"}, {0.5f, 0.5f});
        const TerrainResult<TerrainEncodedWeights> encoded = EncodeTerrainWeightPayload(tile, table);
        REQUIRE(encoded);
        CHECK(encoded.value->slots == std::array<uint8_t, kTerrainWeightSlots>{1, 2, 0xFF, 0xFF});
        CHECK(Channels(*encoded.value, 0) == std::array<uint8_t, kTerrainWeightSlots>{128, 127, 0, 0});
    }

    SECTION("a single layer fills the red channel")
    {
        const TerrainWeightTile tile = ConstantWeights({0, 0}, 2, {"sand"}, {1.0f});
        const TerrainResult<TerrainEncodedWeights> encoded = EncodeTerrainWeightPayload(tile, table);
        REQUIRE(encoded);
        CHECK(encoded.value->slots == std::array<uint8_t, kTerrainWeightSlots>{1, 0xFF, 0xFF, 0xFF});
        for (size_t texel = 0; texel < tile.SampleCount(); ++texel)
            CHECK(Channels(*encoded.value, texel) == std::array<uint8_t, kTerrainWeightSlots>{255, 0, 0, 0});
    }

    SECTION("encoding twice is byte-identical")
    {
        const TerrainWeightTile tile = ConstantWeights({0, 0}, 2, {"dirt", "sand", "rock", "snow", "moss"},
                                                       {0.4f, 0.3f, 0.2f, 0.05f, 0.05f});
        const TerrainResult<TerrainEncodedWeights> first = EncodeTerrainWeightPayload(tile, table);
        const TerrainResult<TerrainEncodedWeights> second = EncodeTerrainWeightPayload(tile, table);
        REQUIRE(first);
        REQUIRE(second);
        CHECK(first.value->slots == second.value->slots);
        CHECK(first.value->bytes == second.value->bytes);
    }
}

TEST_CASE("Weight payloads add their own packs and leave the height packs byte-identical", "[terrain][pack][weights]")
{
    const TerrainLayout layout = EdgeLayout();
    const std::vector<TileHeights> tiles = BuildTiles(layout);
    const std::vector<std::string> table{"dirt", "sand", "rock", "snow"};
    const std::vector<TerrainWeightTile> weights = BuildWeightTiles(layout, table);
    const AssetId asset = TerrainAsset("terrain/edge.terrain.json");
    const TerrainReleaseInfo info = Info(asset, 0xE1);

    const TerrainEncodedRelease plain = Encode(info, layout, tiles, {.chunkTiles = 2});
    TerrainWeightsWrites writes;
    writes.layers = table;
    writes.fingerprint = 0x0123456789ABCDEFull;
    writes.tiles = weights;
    const TerrainEncodedRelease weighted = EncodeWithWeights(info, layout, tiles, writes, {.chunkTiles = 2});

    REQUIRE(plain.packs.size() == weighted.packs.size());
    for (size_t p = 0; p < plain.packs.size(); ++p)
    {
        CHECK(plain.packs[p].fileName == weighted.packs[p].fileName);
        CHECK(plain.packs[p].bytes == weighted.packs[p].bytes);
    }
    CHECK(weighted.heightsHash == plain.heightsHash);
    CHECK(weighted.heightsHash == TerrainHeightsHash(layout, tiles));

    REQUIRE(weighted.weightPacks.size() == 4);
    for (const TerrainEncodedPack &pack : weighted.weightPacks)
    {
        CHECK(pack.fileName == TerrainWeightPackFileName(pack.chunk));
        CHECK(pack.fileName.ends_with(".wpack"));
    }

    auto [plainSource, plainHandle] = OpenInMemory(plain);
    auto [weightedSource, weightedHandle] = OpenInMemory(weighted);
    const TerrainIndex &index = weightedHandle.Index();
    CHECK(plainHandle.Index().formatMinor == kTerrainFormatMinor);
    CHECK(index.formatMinor == kTerrainWeightsFormatMinor);
    CHECK(plainHandle.Index().weights.empty());
    CHECK(plainHandle.Index().weightPacks.empty());
    CHECK(plainHandle.Index().weightLayers.empty());
    CHECK(plainHandle.Index().weightsFingerprint == 0);
    CHECK(index.weightLayers == table);
    CHECK(index.weightsFingerprint == 0x0123456789ABCDEFull);
    CHECK(index.weights.size() == tiles.size());
    CHECK(index.weightPacks.size() == 4);

    REQUIRE(index.tiles.size() == plainHandle.Index().tiles.size());
    for (size_t t = 0; t < index.tiles.size(); ++t)
        CHECK(SameHeights(index.tiles[t], plainHandle.Index().tiles[t]));
    CHECK(weightedHandle.VerifyAllPacks().code == Code::None);

    for (const TerrainWeightTile &tile : weights)
    {
        const TerrainWeightRecord *record = index.FindWeight(tile.tile);
        REQUIRE(record);
        const std::vector<std::byte> bytes = ReadWeightBytes(*weightedSource, index, *record);
        const TerrainResult<TerrainDecodedWeights> decoded = DecodeTerrainWeightPayload(index, *record, bytes);
        INFO(decoded.error.message);
        REQUIRE(decoded);
        CHECK(decoded.value->tile == tile.tile);
        CHECK(decoded.value->intervals == layout.IntervalsPerTile());

        const TerrainResult<TerrainEncodedWeights> expected = EncodeTerrainWeightPayload(tile, table);
        REQUIRE(expected);
        CHECK(decoded.value->slots == expected.value->slots);
        CHECK(decoded.value->bytes == expected.value->bytes);

        const TerrainWeightTile expanded = ExpandTerrainWeights(*decoded.value);
        CHECK(expanded.tile == tile.tile);
        CHECK(expanded.intervals == layout.IntervalsPerTile());
        REQUIRE(expanded.layers == tile.layers);
        REQUIRE(expanded.weights.size() == tile.weights.size());
        for (size_t sample = 0; sample < tile.SampleCount(); ++sample)
        {
            double sum = 0.0;
            for (size_t l = 0; l < tile.layers.size(); ++l)
                sum += tile.weights[l * tile.SampleCount() + sample];
            REQUIRE(sum > 0.0);
            for (size_t l = 0; l < tile.layers.size(); ++l)
                CHECK(std::abs(double(expanded.weights[l * tile.SampleCount() + sample]) -
                               double(tile.weights[l * tile.SampleCount() + sample]) / sum) <= 1.0 / 255.0 + 1e-6);
        }
    }
}

TEST_CASE("A weight index rejects records that disagree with the layout and table", "[terrain][pack][weights]")
{
    const WeightedRelease fixture;
    const TerrainIndex &index = *fixture.index;
    REQUIRE(index.weights.size() == 9);

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

    SECTION("a record for a tile outside the layout")
    {
        refused(
            [&](auto &b)
            {
                StoreU32(b, WeightRecordAt(index, uint32_t(index.weights.size() - 1)), 1);
            },
            "outside the layout");
    }
    SECTION("a record for a tile without a source height record")
    {
        refused(
            [&](auto &b)
            {
                StoreU32(b, WeightedTileRecordAt(index, 0) + terrainformat::payload::kSource, 0);
            },
            "no source height record");
    }
    SECTION("two records for one tile")
    {
        refused(
            [&](auto &b)
            {
                StoreU32(b, WeightRecordAt(index, 1), LoadU64(b, WeightRecordAt(index, 0)) & 0xFFFFFFFFu);
                StoreU32(b, WeightRecordAt(index, 1) + 4,
                         uint32_t(LoadU64(b, WeightRecordAt(index, 0)) >> 32));
            },
            "both name tile");
    }
    SECTION("a slot outside the name table")
    {
        refused(
            [&](auto &b)
            {
                b[WeightRecordAt(index, 0) + terrainformat::weightRecord::kSlots] = std::byte{4};
            },
            "the table has 4");
    }
    SECTION("a used slot after an unused one")
    {
        refused(
            [&](auto &b)
            {
                const size_t at = WeightRecordAt(index, 0) + terrainformat::weightRecord::kSlots;
                b[at] = std::byte{0};
                b[at + 1] = std::byte{0xFF};
                b[at + 2] = std::byte{1};
                b[at + 3] = std::byte{0xFF};
            },
            "unused one");
    }
    SECTION("a repeated used slot")
    {
        refused(
            [&](auto &b)
            {
                const size_t at = WeightRecordAt(index, 0) + terrainformat::weightRecord::kSlots;
                b[at] = std::byte{0};
                b[at + 1] = std::byte{0};
                b[at + 2] = std::byte{0xFF};
                b[at + 3] = std::byte{0xFF};
            },
            "slots repeat");
    }
    SECTION("a payload length that is not a tile of texels")
    {
        refused(
            [&](auto &b)
            {
                StoreU64(b, WeightRecordAt(index, 0) + terrainformat::weightRecord::kLength,
                         LoadU64(b, WeightRecordAt(index, 0) + terrainformat::weightRecord::kLength) - 4);
            },
            "payload length");
    }
    SECTION("an invalid layer name")
    {
        refused(
            [&](auto &b)
            {
                b[WeightLayerTableAt(index) + sizeof(uint32_t)] = std::byte{'D'};
            },
            "not a valid material layer name");
    }
    SECTION("a duplicate layer name")
    {
        refused(
            [&](auto &b)
            {
                const size_t at = WeightLayerTableAt(index) + 2 * sizeof(uint32_t) + 4;
                for (size_t i = 0; i < 4; ++i)
                    b[at + i] = std::byte(uint8_t("dirt"[i]));
            },
            "duplicate name");
    }
    SECTION("too many layer names")
    {
        refused(
            [&](auto &b)
            {
                StoreU32(b, terrainformat::index::kWeightLayerCount, uint32_t(kMaxTerrainMaterialLayers) + 1);
            },
            "exceeds this reader's limit");
    }
    SECTION("two weight payloads that overlap")
    {
        refused(
            [&](auto &b)
            {
                StoreU64(b, WeightRecordAt(index, 1) + terrainformat::weightRecord::kOffset,
                         LoadU64(b, WeightRecordAt(index, 0) + terrainformat::weightRecord::kOffset));
            },
            "overlap");
    }
}

TEST_CASE("A weight pack is checked whole and a damaged one is corrupt", "[terrain][pack][weights]")
{
    const WeightedRelease fixture;
    auto [source, handle] = OpenInMemory(fixture.encoded);
    CHECK(handle.VerifyAllPacks().code == Code::None);

    const TerrainWeightRecord *record = handle.Index().FindWeight({-2, -1});
    REQUIRE(record);
    const std::string name = TerrainWeightPackFileName(handle.Index().weightPacks[record->pack].chunk);
    {
        const std::optional<std::vector<std::byte>> original = source->Get(name);
        REQUIRE(original);
        std::vector<std::byte> bytes = *original;
        bytes[size_t(record->offset)] ^= std::byte{0x01};
        source->Put(name, bytes);
    }

    const TerrainPackError verify = handle.VerifyAllPacks();
    CHECK(verify.code == Code::Corrupt);
    CHECK(Mentions(verify, "wpack"));
    CHECK(Mentions(verify, "checksum"));

    const std::vector<std::byte> payload = ReadWeightBytes(*source, handle.Index(), *record);
    const TerrainResult<TerrainDecodedWeights> decoded =
        DecodeTerrainWeightPayload(handle.Index(), *record, payload);
    CHECK_FALSE(decoded);
    CHECK(decoded.error.code == Code::Corrupt);
    CHECK(Mentions(decoded.error, "checksum"));

    source->Remove(name);
    CHECK(handle.VerifyAllPacks().code == Code::Io);
    CHECK(handle.VerifyWeightPack(handle.Index().weightPacks.size()).code == Code::InvalidInput);
}

TEST_CASE("An index without weight tiles keeps the previous minor and an empty weight section",
          "[terrain][pack][weights]")
{
    const TerrainLayout layout = EdgeLayout();
    const std::vector<TileHeights> tiles = BuildTiles(layout);
    const AssetId asset = TerrainAsset("terrain/edge.terrain.json");
    const TerrainEncodedRelease plain = Encode(Info(asset, 0xE1), layout, tiles, {.chunkTiles = 2});

    auto [source, handle] = OpenInMemory(plain);
    const TerrainIndex &index = handle.Index();
    CHECK(index.formatMinor == kTerrainFormatMinor);
    CHECK(index.formatMinor < kTerrainWeightsFormatMinor);
    CHECK(index.weights.empty());
    CHECK(index.weightPacks.empty());
    CHECK(index.weightLayers.empty());
    CHECK(index.weightsFingerprint == 0);
    CHECK(handle.VerifyAllPacks().code == Code::None);
}

TEST_CASE("The writer refuses weight tiles it cannot encode", "[terrain][pack][weights]")
{
    const TerrainLayout layout = EdgeLayout();
    const std::vector<TileHeights> tiles = BuildTiles(layout);
    const TerrainReleaseInfo info = Info(TerrainAsset("terrain/edge.terrain.json"), 0xE1);
    const std::vector<std::string> table{"dirt", "sand", "rock", "snow"};
    const std::vector<TerrainWeightTile> weights = BuildWeightTiles(layout, table);

    auto refused = [&](TerrainWeightsWrites writes, std::span<const TileHeights> heights, std::string_view text)
    {
        std::vector<TerrainTileWrite> inputs;
        for (const TileHeights &tile : heights)
            inputs.push_back({&tile});
        const TerrainResult<TerrainEncodedRelease> result =
            EncodeTerrainRelease(info, std::span(&layout, 1), inputs, {}, {}, writes, {.chunkTiles = 2});
        INFO(result.error.message);
        CHECK_FALSE(result);
        CHECK(result.error.code == Code::InvalidInput);
        CHECK(Mentions(result.error, text));
    };

    TerrainWeightsWrites writes;
    writes.tiles = weights;
    refused(writes, tiles, "weight layer table");

    writes.layers = table;
    std::vector<TerrainWeightTile> badName = weights;
    badName[0].layers[0] = "Dirt";
    writes.tiles = badName;
    refused(writes, tiles, "not in the table");

    std::vector<TerrainWeightTile> oddIntervals = weights;
    oddIntervals[0] = ConstantWeights(weights[0].tile, 32, table, {0.25f, 0.25f, 0.25f, 0.25f});
    writes.tiles = oddIntervals;
    refused(writes, tiles, "intervals");

    std::vector<TerrainWeightTile> duplicated = weights;
    duplicated.push_back(weights[0]);
    writes.tiles = duplicated;
    refused(writes, tiles, "given twice");

    std::vector<TerrainWeightTile> outside = weights;
    outside.push_back(ConstantWeights({5, 5}, layout.IntervalsPerTile(), table, {0.25f, 0.25f, 0.25f, 0.25f}));
    writes.tiles = outside;
    refused(writes, tiles, "outside the layout");

    writes.tiles = weights;
    writes.layers = {"dirt", "sand"};
    refused(writes, tiles, "not in the table");
}

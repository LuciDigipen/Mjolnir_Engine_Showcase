/**
 * @file TerrainBaseWeights.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainBaseWeights.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainPackTestSupport.h"

#include <terrain/TerrainBaseWeights.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using Code = TerrainBaseWeightsErrorCode;

    constexpr uint64_t kWeightsKey = 0x0123456789abcdeFull;

    std::vector<std::string> Layers()
    {
        return {"base", "sand"};
    }

    std::vector<TerrainWeightTile> BuildWeightTiles(const TerrainLayout &layout)
    {
        const TileRange range = layout.Tiles();
        const int32_t n = layout.IntervalsPerTile();
        const size_t count = static_cast<size_t>(n + 1) * static_cast<size_t>(n + 1);
        std::vector<TerrainWeightTile> tiles;
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                TerrainWeightTile tile;
                tile.tile = {x, z};
                tile.intervals = n;
                tile.layers = Layers();
                tile.weights.assign(2 * count, 0.0f);
                for (int32_t k = 0; k <= n; ++k)
                    for (int32_t i = 0; i <= n; ++i)
                    {
                        const size_t sample = static_cast<size_t>(k) * static_cast<size_t>(n + 1) +
                                              static_cast<size_t>(i);
                        const float sand = static_cast<float>((i * 3 + k * 5) % 8) / 8.0f;
                        tile.weights[sample] = 1.0f - sand;
                        tile.weights[count + sample] = sand;
                    }
                tiles.push_back(std::move(tile));
            }
        return tiles;
    }

    TerrainEncodedBaseWeights EncodeCache(AssetId asset, const TerrainLayout &layout,
                                          std::span<const TerrainWeightTile> tiles)
    {
        const std::vector<std::string> layers = Layers();
        TerrainResult<TerrainEncodedBaseWeights> encoded =
            EncodeTerrainBaseWeights(asset, TerrainSourceFingerprint(kWeightsKey), layout, layers, tiles);
        INFO(encoded.error.message);
        REQUIRE(encoded);
        return std::move(*encoded.value);
    }

    void WriteCache(const std::filesystem::path &root, AssetId asset, const TerrainEncodedBaseWeights &encoded)
    {
        const std::filesystem::path dir =
            TerrainBaseWeightsDir(root, asset, TerrainSourceFingerprint(kWeightsKey));
        REQUIRE(std::filesystem::create_directories(dir));
        REQUIRE(WriteBytes(dir / kTerrainBaseWeightsIndexFileName, encoded.index));
        REQUIRE(WriteBytes(dir / kTerrainBaseWeightsPackFileName, encoded.pack));
        REQUIRE(WriteTextAtomic(TerrainBaseWeightsPointerPath(root, asset), TerrainIdHex(kWeightsKey)));
    }
}

TEST_CASE("Base weights follow the pointer and read the cached rule weights", "[terrain][weights][base]")
{
    const std::filesystem::path root = MakeScratchDir("base_weights_read");
    const AssetId asset = TerrainAsset("terrain/fixture.terrain.json");
    const TerrainLayout layout = ReliefLayout();
    const std::vector<TerrainWeightTile> tiles = BuildWeightTiles(layout);
    WriteCache(root, asset, EncodeCache(asset, layout, tiles));

    const TerrainBaseWeightsResult<TerrainBaseWeights> opened = TerrainBaseWeights::Open(root, asset);
    INFO(opened.error.message);
    REQUIRE(opened);
    CHECK(opened.value->Key() == TerrainSourceFingerprint(kWeightsKey));
    CHECK(std::vector<std::string>(opened.value->Layers().begin(), opened.value->Layers().end()) == Layers());
    CHECK(opened.value->Layout().SampleSpacing() == layout.SampleSpacing());
    CHECK(opened.value->Layout().IntervalsPerTile() == layout.IntervalsPerTile());
    CHECK(opened.value->Layout().Bounds() == layout.Bounds());

    for (const TerrainWeightTile &expected : tiles)
    {
        const TerrainBaseWeightsResult<TerrainWeightTile> read = opened.value->ReadTile(expected.tile);
        INFO(read.error.message);
        REQUIRE(read);
        CHECK(read.value->tile == expected.tile);
        CHECK(read.value->intervals == expected.intervals);
        CHECK(read.value->layers == expected.layers);
        REQUIRE(read.value->weights.size() == expected.weights.size());
        for (size_t s = 0; s < expected.weights.size(); ++s)
            CHECK(std::abs(read.value->weights[s] - expected.weights[s]) <= 1.0 / 255.0 + 1e-6);
    }
}

TEST_CASE("Base weights report a missing pointer, a missing tile and corruption distinctly",
          "[terrain][weights][base]")
{
    const std::filesystem::path root = MakeScratchDir("base_weights_errors");
    const AssetId asset = TerrainAsset("terrain/fixture.terrain.json");
    const TerrainLayout layout = ReliefLayout();
    const std::vector<TerrainWeightTile> tiles = BuildWeightTiles(layout);
    const TerrainEncodedBaseWeights encoded = EncodeCache(asset, layout, tiles);

    SECTION("nothing has written a cache")
    {
        const TerrainBaseWeightsResult<TerrainBaseWeights> opened = TerrainBaseWeights::Open(root, asset);
        CHECK_FALSE(opened);
        CHECK(opened.error.code == Code::MissingPointer);
        CHECK(opened.error.message.find("sync") != std::string::npos);
    }

    SECTION("the pointer names something that is not a key")
    {
        REQUIRE(std::filesystem::create_directories(TerrainBaseWeightsRoot(root, asset)));
        REQUIRE(WriteTextAtomic(TerrainBaseWeightsPointerPath(root, asset), "not a key"));
        const TerrainBaseWeightsResult<TerrainBaseWeights> opened = TerrainBaseWeights::Open(root, asset);
        CHECK_FALSE(opened);
        CHECK(opened.error.code == Code::Corrupt);
    }

    WriteCache(root, asset, encoded);
    const TerrainBaseWeightsResult<TerrainBaseWeights> opened = TerrainBaseWeights::Open(root, asset);
    INFO(opened.error.message);
    REQUIRE(opened);

    SECTION("a tile outside the cached map")
    {
        const TerrainBaseWeightsResult<TerrainWeightTile> read = opened.value->ReadTile({9, 9});
        CHECK_FALSE(read);
        CHECK(read.error.code == Code::MissingTile);
    }

    SECTION("a corrupted pack")
    {
        const std::filesystem::path pack = TerrainBaseWeightsDir(root, asset, TerrainSourceFingerprint(kWeightsKey)) /
                                           kTerrainBaseWeightsPackFileName;
        std::optional<std::vector<std::byte>> bytes = ReadBytes(pack);
        REQUIRE(bytes);
        (*bytes)[kTerrainPackHeaderSize + 1] ^= std::byte{0x40};
        REQUIRE(WriteBytes(pack, *bytes));

        const TerrainBaseWeightsResult<TerrainWeightTile> read = opened.value->ReadTile(tiles.front().tile);
        CHECK_FALSE(read);
        CHECK(read.error.code == Code::Corrupt);
    }

    SECTION("a corrupted index")
    {
        const std::filesystem::path path = TerrainBaseWeightsDir(root, asset, TerrainSourceFingerprint(kWeightsKey)) /
                                           kTerrainBaseWeightsIndexFileName;
        std::optional<std::vector<std::byte>> bytes = ReadBytes(path);
        REQUIRE(bytes);
        (*bytes)[bytes->size() - 1] ^= std::byte{0x01};
        REQUIRE(WriteBytes(path, *bytes));

        const TerrainBaseWeightsResult<TerrainBaseWeights> reopened = TerrainBaseWeights::Open(root, asset);
        CHECK_FALSE(reopened);
        CHECK(reopened.error.code == Code::Corrupt);
    }

    SECTION("a newer index version")
    {
        const std::filesystem::path path = TerrainBaseWeightsDir(root, asset, TerrainSourceFingerprint(kWeightsKey)) /
                                           kTerrainBaseWeightsIndexFileName;
        std::optional<std::vector<std::byte>> bytes = ReadBytes(path);
        REQUIRE(bytes);
        StoreU32(*bytes, 4, kTerrainBaseWeightsVersion + 1);
        REQUIRE(WriteBytes(path, *bytes));

        const TerrainBaseWeightsResult<TerrainBaseWeights> reopened = TerrainBaseWeights::Open(root, asset);
        CHECK_FALSE(reopened);
        CHECK(reopened.error.code == Code::Unsupported);
    }

    SECTION("a missing pack")
    {
        const std::filesystem::path pack = TerrainBaseWeightsDir(root, asset, TerrainSourceFingerprint(kWeightsKey)) /
                                           kTerrainBaseWeightsPackFileName;
        REQUIRE(std::filesystem::remove(pack));
        const TerrainBaseWeightsResult<TerrainBaseWeights> reopened = TerrainBaseWeights::Open(root, asset);
        CHECK_FALSE(reopened);
        CHECK(reopened.error.code == Code::Io);
    }
}

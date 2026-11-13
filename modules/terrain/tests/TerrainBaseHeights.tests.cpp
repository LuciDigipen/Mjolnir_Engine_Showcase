/**
 * @file TerrainBaseHeights.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainBaseHeights.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainPackTestSupport.h"

#include <terrain/TerrainBaseHeights.h>

#include <catch2/catch_test_macros.hpp>

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
    using Code = TerrainBaseHeightsErrorCode;

    constexpr uint64_t kBaseKey = 0x0123456789abcdefull;

    void WriteBaseCache(const std::filesystem::path &root, const TerrainReleaseInfo &info,
                        const TerrainEncodedRelease &encoded)
    {
        const std::filesystem::path dir = TerrainBaseHeightsDir(root, info.asset, info.source);
        REQUIRE(std::filesystem::create_directories(dir));
        REQUIRE(WriteBytes(dir / kTerrainIndexFileName, encoded.index));
        for (const TerrainEncodedPack &pack : encoded.packs)
            REQUIRE(WriteBytes(dir / pack.fileName, pack.bytes));
        REQUIRE(WriteTextAtomic(TerrainBaseHeightsPointerPath(root, info.asset), TerrainIdHex(info.source.value)));
    }
}

TEST_CASE("Base heights follow the pointer and read the cached source tiles", "[terrain][base]")
{
    const std::filesystem::path root = MakeScratchDir("base_read");
    const AssetId asset = TerrainAsset("terrain/fixture.terrain.json");
    const TerrainLayout layout = ReliefLayout();
    const std::vector<TileHeights> tiles = BuildTiles(layout);
    const TerrainReleaseInfo info = Info(asset, kBaseKey, kBaseKey);
    const TerrainEncodedRelease encoded = Encode(info, layout, tiles, {.chunkTiles = 2});
    WriteBaseCache(root, info, encoded);

    const TerrainBaseHeightsResult<TerrainBaseHeights> opened = TerrainBaseHeights::Open(root, asset);
    INFO(opened.error.message);
    REQUIRE(opened);
    CHECK(opened.value->Key() == info.source);
    CHECK(opened.value->Release().Info() == info);
    for (const TileHeights &expected : tiles)
    {
        const TerrainBaseHeightsResult<TileHeights> read = opened.value->ReadTile(expected.Tile());
        INFO(read.error.message);
        REQUIRE(read);
        CHECK(*read.value == expected);
    }
}

TEST_CASE("Base heights report a missing pointer, a missing tile and corruption distinctly", "[terrain][base]")
{
    const std::filesystem::path root = MakeScratchDir("base_errors");
    const AssetId asset = TerrainAsset("terrain/fixture.terrain.json");

    SECTION("nothing has written a cache")
    {
        const TerrainBaseHeightsResult<TerrainBaseHeights> opened = TerrainBaseHeights::Open(root, asset);
        CHECK_FALSE(opened);
        CHECK(opened.error.code == Code::MissingPointer);
        CHECK(opened.error.message.find("sync") != std::string::npos);
    }

    const TerrainLayout layout = ReliefLayout();
    const std::vector<TileHeights> tiles = BuildTiles(layout);
    const TerrainReleaseInfo info = Info(asset, kBaseKey, kBaseKey);
    const TerrainEncodedRelease encoded = Encode(info, layout, tiles, {.chunkTiles = 2});

    SECTION("the pointer names a key that is not a base")
    {
        REQUIRE(std::filesystem::create_directories(TerrainBaseHeightsRoot(root, asset)));
        REQUIRE(WriteTextAtomic(TerrainBaseHeightsPointerPath(root, asset), "not a key"));
        const TerrainBaseHeightsResult<TerrainBaseHeights> opened = TerrainBaseHeights::Open(root, asset);
        CHECK_FALSE(opened);
        CHECK(opened.error.code == Code::Corrupt);
    }

    WriteBaseCache(root, info, encoded);
    const TerrainBaseHeightsResult<TerrainBaseHeights> opened = TerrainBaseHeights::Open(root, asset);
    INFO(opened.error.message);
    REQUIRE(opened);

    SECTION("a tile outside the cached map")
    {
        const TerrainBaseHeightsResult<TileHeights> read = opened.value->ReadTile({9, 9});
        CHECK_FALSE(read);
        CHECK(read.error.code == Code::MissingTile);
    }

    SECTION("a corrupted pack")
    {
        const std::filesystem::path pack = TerrainBaseHeightsDir(root, asset, info.source) /
                                           encoded.packs.front().fileName;
        std::optional<std::vector<std::byte>> bytes = ReadBytes(pack);
        REQUIRE(bytes);
        (*bytes)[bytes->size() / 2] ^= std::byte{0x40};
        REQUIRE(WriteBytes(pack, *bytes));

        const TerrainBaseHeightsResult<TileHeights> read = opened.value->ReadTile(tiles.front().Tile());
        CHECK_FALSE(read);
        CHECK(read.error.code == Code::Corrupt);
    }
}

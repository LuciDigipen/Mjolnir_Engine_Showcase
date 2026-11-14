/**
 * @file TerrainPackWriter.h
 * @author Jackey Cen
 * @brief Encodes a terrain release's index and packs in memory, and mints release ids.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainByteSource.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRegionGrid.h>
#include <terrain/TerrainWeights.h>
#include <terrain/TileHeights.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mir
{
    struct TerrainPackWriteOptions
    {
        // Addresses never depend on it.
        int32_t chunkTiles = kDefaultTerrainChunkTiles;
        // Power of two up to kMaxTerrainPayloadAlignment.
        uint32_t payloadAlignment = kDefaultTerrainPayloadAlignment;
    };

    struct TerrainEncodedPack
    {
        TerrainChunkCoord chunk;
        std::string fileName;
        std::vector<std::byte> bytes;
    };

    struct TerrainEncodedRelease
    {
        std::vector<std::byte> index;
        std::vector<TerrainEncodedPack> packs;
        std::vector<TerrainEncodedPack> weightPacks;
        uint64_t heightsHash = 0;
    };

    struct TerrainTileWrite
    {
        const TileHeights *heights = nullptr;
        uint32_t layer = 0;
        bool source = true;
        std::optional<double> maxDeviation;
        uint64_t sourceChecksum = 0;
    };

    TerrainResult<TerrainEncodedRelease> EncodeTerrainRelease(const TerrainReleaseInfo &info,
                                                              std::span<const TerrainLayout> layers,
                                                              std::span<const TerrainTileWrite> tiles,
                                                              const TerrainPackWriteOptions &options = {});

    struct TerrainPayloadWrite
    {
        TerrainTileRecord record;
        std::vector<std::byte> bytes;
    };

    TerrainResult<TerrainEncodedRelease> EncodeTerrainRelease(const TerrainReleaseInfo &info,
                                                              std::span<const TerrainLayout> layers,
                                                              std::span<const TerrainTileWrite> tiles,
                                                              std::span<const TerrainPayloadWrite> derived,
                                                              const TerrainPackWriteOptions &options = {});
    uint64_t TerrainPayloadsHash(std::span<const TerrainLayout> layers, std::span<const TerrainTileWrite> tiles,
                                 std::span<const TerrainPayloadWrite> derived);

    // One payload per Region of grid, in any order; a record names its Region by level and x, z as tile.
    struct TerrainRegionWrites
    {
        std::optional<TerrainRegionGrid> grid;
        std::vector<TerrainPayloadWrite> payloads;
    };

    TerrainResult<TerrainEncodedRelease> EncodeTerrainRelease(const TerrainReleaseInfo &info,
                                                              std::span<const TerrainLayout> layers,
                                                              std::span<const TerrainTileWrite> tiles,
                                                              std::span<const TerrainPayloadWrite> derived,
                                                              const TerrainRegionWrites &regions,
                                                              const TerrainPackWriteOptions &options = {});
    uint64_t TerrainPayloadsHash(std::span<const TerrainLayout> layers, std::span<const TerrainTileWrite> tiles,
                                 std::span<const TerrainPayloadWrite> derived, const TerrainRegionWrites &regions);

    uint64_t TerrainHeightsHash(std::span<const TerrainLayout> layers, std::span<const TerrainTileWrite> tiles);

    struct TerrainEncodedWeights
    {
        std::array<uint8_t, kTerrainWeightSlots> slots{};
        // (intervals + 1)^2 RGBA texels, row-major x fastest.
        std::vector<std::byte> bytes;
    };

    // Keeps the four heaviest layers of the tile, renormalises each texel and quantises so its bytes sum to 255.
    TerrainResult<TerrainEncodedWeights> EncodeTerrainWeightPayload(const TerrainWeightTile &tile,
                                                                    std::span<const std::string> table);

    // A Region's encoded image: one payload per Region of the release's grid.
    struct TerrainRegionWeightImage
    {
        TerrainRegionCoord region;
        uint32_t side = kTerrainRegionWeightSide;
        std::array<uint8_t, kTerrainWeightSlots> slots{};
        // side * side RGBA texels, row-major x fastest.
        std::vector<std::byte> bytes;
    };

    struct TerrainWeightsWrites
    {
        // Layer names the records' slots index; every weighted tile's layers must be here.
        std::vector<std::string> layers;
        uint64_t fingerprint = 0;
        std::span<const TerrainWeightTile> tiles;
        // Every Region of regions.grid when the release has one; empty otherwise.
        std::span<const TerrainRegionWeightImage> regionImages;
    };

    TerrainResult<TerrainEncodedRelease> EncodeTerrainRelease(const TerrainReleaseInfo &info,
                                                              std::span<const TerrainLayout> layers,
                                                              std::span<const TerrainTileWrite> tiles,
                                                              std::span<const TerrainPayloadWrite> derived,
                                                              const TerrainRegionWrites &regions,
                                                              const TerrainWeightsWrites &weights,
                                                              const TerrainPackWriteOptions &options = {});

    TerrainResult<TerrainEncodedRelease> EncodeTerrainRelease(const TerrainReleaseInfo &info,
                                                              const TerrainLayout &layout,
                                                              std::span<const TileHeights> tiles,
                                                              const TerrainPackWriteOptions &options = {});

    // Independent of chunk size and alignment.
    uint64_t TerrainHeightsHash(const TerrainLayout &layout, std::span<const TileHeights> tiles);

    // The nonce makes two bakes of the same source distinct releases. Never zero.
    TerrainReleaseId MintTerrainReleaseId(AssetId asset, TerrainSourceFingerprint source, uint64_t heightsHash,
                                          uint64_t nonce);

    uint64_t MakeTerrainReleaseNonce();

    std::shared_ptr<TerrainMemorySource> MakeTerrainMemorySource(const TerrainEncodedRelease &release);
}

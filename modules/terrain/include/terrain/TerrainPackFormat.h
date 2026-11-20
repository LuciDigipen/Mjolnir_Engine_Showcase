/**
 * @file TerrainPackFormat.h
 * @author Jackey Cen
 * @brief Terrain release index and tile pack format: records, parsing and decoding.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainConstants.h>

#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainRegionGrid.h>
#include <terrain/TerrainWeights.h>
#include <terrain/TileHeights.h>
#include <terrain/TileKey.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    inline constexpr std::string_view kTerrainIndexFileName = "release.tindex";

    // floorDiv(tile, chunkTiles) on each axis.
    struct TerrainChunkCoord
    {
        int32_t x = 0;
        int32_t z = 0;

        constexpr bool operator==(const TerrainChunkCoord &) const = default;
    };

    TerrainChunkCoord TerrainChunkOf(TileCoord tile, int32_t chunkTiles);

    // "chunk_<x>_<z>.tpack"
    std::string TerrainPackFileName(TerrainChunkCoord chunk);

    // "chunk_<x>_<z>.wpack"
    std::string TerrainWeightPackFileName(TerrainChunkCoord chunk);

    struct TerrainPackRecord
    {
        TerrainChunkCoord chunk;
        uint32_t tileCount = 0;
        uint64_t fileSize = 0;
        // Fnv1a64 of the whole pack file.
        uint64_t checksum = 0;
    };

    enum class TerrainPayloadKind : uint32_t
    {
        Height = 0,
        LodHeight = 1,
        Normal = 2,
        Region = 3,
    };

    bool TerrainLayersNest(const TerrainLayout &fine, const TerrainLayout &coarse);

    // One tile's RGBA8 weight image on the reference grid; slots index weightLayers, 0xFF is unused.
    struct TerrainWeightRecord
    {
        TileCoord tile;
        uint32_t pack = 0;
        uint64_t offset = 0;
        uint64_t length = 0;
        uint64_t checksum = 0;
        std::array<uint8_t, kTerrainWeightSlots> slots{};
    };

    // One Region's RGBA8 weight image over its whole rect; side texels a side.
    struct TerrainRegionWeightRecord
    {
        TerrainRegionCoord region;
        uint32_t pack = 0;
        uint64_t offset = 0;
        uint64_t length = 0;
        uint64_t checksum = 0;
        std::array<uint8_t, kTerrainWeightSlots> slots{};
        uint32_t side = 0;
    };

    struct TerrainTileRecord
    {
        TileCoord tile;
        uint32_t pack = 0;
        int32_t samplesPerSide = 0;
        uint64_t offset = 0;
        uint64_t length = 0;
        uint64_t checksum = 0;
        double sampleSpacing = 0.0;
        // Padding samples included.
        float minHeight = 0.0f;
        float maxHeight = 0.0f;
        TerrainLocalBounds validExtent;
        uint32_t layer = 0;
        TerrainPayloadKind kind = TerrainPayloadKind::Height;
        uint32_t level = 0;
        bool source = true;
        std::optional<double> maxDeviation;
        uint64_t sourceChecksum = 0;
        uint64_t topologyHash = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
    };

    enum class TerrainPackErrorCode : uint8_t
    {
        None,
        Io,            // a file is missing, unreadable or shorter than a record says
        Malformed,     // magic, sizes, bounds, ordering, duplicates, overlaps, overflow
        Unsupported,   // a major format, content version or limit this reader does not know
        Corrupt,       // a checksum or a payload disagrees with its record
        WrongIdentity, // asset or release differs from the one asked for
        NotFound,      // a tile outside the release's layout
        InvalidInput,  // the writer was given tiles or options it cannot encode
        Internal,      // a reader or decoder threw; a bug, so never retried
    };

    struct TerrainPackError
    {
        TerrainPackErrorCode code = TerrainPackErrorCode::None;
        std::string message;
        // First non-finite sample in a checksum-valid payload.
        std::optional<GlobalSampleIndex> nonFiniteSample;
    };

    template <typename T> struct TerrainResult
    {
        std::optional<T> value;
        TerrainPackError error;

        bool Ok() const
        {
            return value.has_value();
        }
        explicit operator bool() const
        {
            return Ok();
        }
    };

    // layout is the finest reference grid, not each tile's source grid.
    struct TerrainIndex
    {
        TerrainReleaseInfo release;
        uint32_t formatMinor = 0;
        uint32_t contentVersion = 0;
        uint32_t payloadAlignment = 0;
        int32_t chunkTiles = 0;
        TerrainLayout layout;
        std::vector<TerrainLayout> layers;
        // Fnv1a64 of the ordered payload keys, spacing, source flags and checksums.
        uint64_t heightsHash = 0;
        std::vector<TerrainPackRecord> packs;
        std::vector<TerrainTileRecord> tiles;
        std::optional<TerrainRegionGrid> regionGrid;
        // Every Region of regionGrid, in its ordinal order.
        std::vector<TerrainTileRecord> regions;
        // Layer names the weight records' slots index, in table order.
        std::vector<std::string> weightLayers;
        uint64_t weightsFingerprint = 0;
        std::vector<TerrainPackRecord> weightPacks;
        // One per weighted tile, in (z, x) order.
        std::vector<TerrainWeightRecord> weights;
        // One per Region of regionGrid, in ordinal order; empty without a Region grid or weights.
        std::vector<TerrainRegionWeightRecord> regionWeights;

        const TerrainTileRecord *Find(TileCoord tile) const;
        // Kind Region names a Region by its x, z in tile and its level, with layer 0.
        const TerrainTileRecord *Find(TileCoord tile, uint32_t layer,
                                      TerrainPayloadKind kind = TerrainPayloadKind::Height, uint32_t level = 0) const;
        const TerrainTileRecord *FindRegion(TerrainRegionCoord region) const;
        const TerrainWeightRecord *FindWeight(TileCoord tile) const;
        const TerrainRegionWeightRecord *FindRegionWeight(TerrainRegionCoord region) const;
    };

    uint64_t TerrainRecordsHeightsHash(std::span<const TerrainTileRecord> records);
    uint64_t TerrainRecordsHeightsHash(std::span<const TerrainTileRecord> records,
                                       std::span<const TerrainTileRecord> regions);

    // Never returns a partial index.
    TerrainResult<TerrainIndex> ParseTerrainIndex(std::span<const std::byte> bytes);

    // Checks length, checksum, finite samples and min/max against the record.
    TerrainResult<TileHeights> DecodeTerrainTile(const TerrainIndex &index, const TerrainTileRecord &record,
                                                 std::span<const std::byte> payload);

    struct TerrainDecodedWeights
    {
        TileCoord tile;
        int32_t intervals = 0;
        std::array<uint8_t, kTerrainWeightSlots> slots{};
        // The name of each used slot; empty for an unused one.
        std::array<std::string, kTerrainWeightSlots> layers;
        // (intervals + 1)^2 RGBA texels, row-major x fastest.
        std::vector<std::byte> bytes;
    };

    // Checks the record's layer slots, the payload length, the checksum and every texel's byte rules.
    TerrainResult<TerrainDecodedWeights> DecodeTerrainWeightPayload(const TerrainIndex &index,
                                                                    const TerrainWeightRecord &record,
                                                                    std::span<const std::byte> payload);

    // The Region image's slot and texel rules, reported with the same shape as a tile's.
    TerrainResult<TerrainDecodedWeights> DecodeTerrainRegionWeightPayload(const TerrainIndex &index,
                                                                          const TerrainRegionWeightRecord &record,
                                                                          std::span<const std::byte> payload);

    // Converts the channels of each used slot to [0, 1] weights.
    TerrainWeightTile ExpandTerrainWeights(const TerrainDecodedWeights &decoded);
}

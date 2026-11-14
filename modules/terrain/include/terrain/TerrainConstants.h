/**
 * @file TerrainConstants.h
 * @author Jackey Cen
 * @brief Shared terrain format, Region defaults and numerical limits.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainIdentity.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace mir
{
    // Decisions: docs/modules/terrain.md#size-baseline-issue-24 and
    // docs/modules/terrain.md#region-hierarchy-issue-124. Sizes remain revisable.
    inline constexpr int32_t kDefaultTerrainChunkTiles = 16;
    inline constexpr uint32_t kDefaultTerrainPayloadAlignment = 16;
    inline constexpr uint32_t kMaxTerrainPayloadAlignment = 65536;
    inline constexpr uint32_t kMaxTerrainRegionCells = 4096;
    inline constexpr uint32_t kMaxTerrainRegionTiles = 1u << 20;
    inline constexpr uint32_t kMaxTerrainRegionLevel = 40;
    inline constexpr int64_t kTerrainExactSampleLimit = int64_t(1) << 53;
    // Side of one Region's RGBA8 weight image at every level.
    inline constexpr uint32_t kTerrainRegionWeightSide = 257;

    // Wire layout: docs/modules/terrain.md#rules, binary-format table.
    inline constexpr TerrainFormatVersion kTerrainFormatVersion{2};
    inline constexpr uint32_t kTerrainFormatMinor = 2;
    inline constexpr uint32_t kTerrainRegionsFormatMinor = 2;
    // A release without weight tiles keeps kTerrainFormatMinor and the shorter header.
    inline constexpr uint32_t kTerrainWeightsFormatMinor = kTerrainFormatMinor + 1;
    inline constexpr uint32_t kTerrainHeightsContentVersion = 1;
    inline constexpr uint32_t kTerrainLodContentVersion = 1;
    inline constexpr uint32_t kTerrainRegionContentVersion = 1;
    inline constexpr uint32_t kTerrainWeightsContentVersion = 1;
    inline constexpr uint32_t kTerrainIndexHeaderSize = 152;
    inline constexpr uint32_t kTerrainWeightsIndexHeaderSize = 192;
    inline constexpr uint32_t kTerrainLegacyIndexHeaderSize = 128;
    inline constexpr uint32_t kTerrainPackRecordSize = 32;
    inline constexpr uint32_t kTerrainTileRecordSize = 144;
    inline constexpr uint32_t kTerrainLegacyTileRecordSize = 120;
    inline constexpr uint32_t kTerrainWeightRecordSize = 48;
    inline constexpr uint32_t kTerrainRegionWeightRecordSize = 48;
    inline constexpr uint32_t kTerrainLayerRecordSize = 16;
    inline constexpr uint32_t kTerrainPackHeaderSize = 48;
    inline constexpr uint32_t kTerrainIndexTrailerSize = 8;
    inline constexpr uint32_t kTerrainDerivedHeaderSize = 80;
    inline constexpr uint32_t kTerrainRegionHeaderSize = 112;

    // Numerical policy: docs/modules/terrain.md#numerical-limits-and-tolerances.
    inline constexpr double kTerrainNormalLengthSquaredTolerance = 2e-6;
    inline constexpr double kTerrainCoverageRelativeTolerance = 1e-9;
    inline constexpr double kTerrainDeviationRoundoffFactor = 4096;
    inline constexpr double kTerrainOverlaySignRoundoffFactor = 64;
    inline constexpr double kTerrainSharedNodeRoundoffFactor = 64;
    inline constexpr double kTerrainSamplerRoundoffFactor = 64;
    inline constexpr size_t kTerrainOverlayPolygonCapacity = 32;
} // namespace mir

namespace mir::terrainbake
{
    inline constexpr uint32_t kTerrainRegionTiles = 16;
    inline constexpr uint32_t kTerrainRegionCells = 128;
} // namespace mir::terrainbake

namespace mir::terrainformat
{
    inline constexpr std::array<std::byte, 4> kIndexMagic{std::byte{'M'}, std::byte{'J'}, std::byte{'T'},
                                                          std::byte{'I'}};
    inline constexpr std::array<std::byte, 4> kPackMagic{std::byte{'M'}, std::byte{'J'}, std::byte{'T'},
                                                         std::byte{'P'}};
    inline constexpr uint32_t kLodMagic = 0x31444f4c;
    inline constexpr uint32_t kNormalMagic = 0x314d524e;
    inline constexpr uint32_t kRegionMagic = 0x314e4752;
    inline constexpr size_t kPrefixSize = 16;
    inline constexpr size_t kRecordAlignment = 8;
    inline constexpr size_t kFloatBytes = 4;
    inline constexpr size_t kIndexBytes = 4;
    inline constexpr size_t kVec3Bytes = 3 * kFloatBytes;
    inline constexpr size_t kRegionVertexBytes = 2 * kVec3Bytes;

    namespace index
    {
        inline constexpr size_t kMajor = 4;
        inline constexpr size_t kMinor = 8;
        inline constexpr size_t kHeaderSize = 12;
        inline constexpr size_t kContentVersion = 16;
        inline constexpr size_t kAlignment = 20;
        inline constexpr size_t kAsset = 24;
        inline constexpr size_t kRelease = 32;
        inline constexpr size_t kSource = 40;
        inline constexpr size_t kSpacing = 48;
        inline constexpr size_t kIntervals = 56;
        inline constexpr size_t kChunkTiles = 60;
        inline constexpr size_t kMinX = 64;
        inline constexpr size_t kMinZ = 72;
        inline constexpr size_t kMaxX = 80;
        inline constexpr size_t kMaxZ = 88;
        inline constexpr size_t kPackCount = 96;
        inline constexpr size_t kPackRecordSize = 100;
        inline constexpr size_t kTileCount = 104;
        inline constexpr size_t kTileRecordSize = 108;
        inline constexpr size_t kHeightsHash = 112;
        inline constexpr size_t kLayerCount = 120;
        inline constexpr size_t kLayerRecordSize = 124;
        inline constexpr size_t kRegionCount = 128;
        inline constexpr size_t kRegionRecordSize = 132;
        inline constexpr size_t kRegionTiles = 136;
        inline constexpr size_t kRegionCells = 140;
        inline constexpr size_t kRegionUnit = 144;
        inline constexpr size_t kRegionLevels = 148;
        inline constexpr size_t kWeightPackCount = 152;
        inline constexpr size_t kWeightPackRecordSize = 156;
        inline constexpr size_t kWeightTileCount = 160;
        inline constexpr size_t kWeightTileRecordSize = 164;
        inline constexpr size_t kWeightLayerCount = 168;
        inline constexpr size_t kWeightLayerBytes = 172;
        inline constexpr size_t kWeightsFingerprint = 176;
        inline constexpr size_t kRegionWeightCount = 184;
        inline constexpr size_t kRegionWeightRecordSize = 188;
    } // namespace index
    namespace packRecord
    {
        inline constexpr size_t kX = 0;
        inline constexpr size_t kZ = 4;
        inline constexpr size_t kTileCount = 8;
        inline constexpr size_t kFileSize = 16;
        inline constexpr size_t kChecksum = 24;
    } // namespace packRecord
    namespace payload
    {
        inline constexpr size_t kX = 0;
        inline constexpr size_t kZ = 4;
        inline constexpr size_t kPack = 8;
        inline constexpr size_t kSamplesPerSide = 12;
        inline constexpr size_t kOffset = 16;
        inline constexpr size_t kLength = 24;
        inline constexpr size_t kChecksum = 32;
        inline constexpr size_t kSpacing = 40;
        inline constexpr size_t kMinHeight = 48;
        inline constexpr size_t kMaxHeight = 52;
        inline constexpr size_t kMinX = 56;
        inline constexpr size_t kMinZ = 64;
        inline constexpr size_t kMaxX = 72;
        inline constexpr size_t kMaxZ = 80;
        inline constexpr size_t kLayer = 88;
        inline constexpr size_t kKind = 92;
        inline constexpr size_t kLevel = 96;
        inline constexpr size_t kSource = 100;
        inline constexpr size_t kHasDeviation = 104;
        inline constexpr size_t kDeviation = 112;
        inline constexpr size_t kSourceChecksum = 120;
        inline constexpr size_t kTopologyHash = 128;
        inline constexpr size_t kVertexCount = 136;
        inline constexpr size_t kIndexCount = 140;
    } // namespace payload
    namespace layer
    {
        inline constexpr size_t kSpacing = 0;
        inline constexpr size_t kIntervals = 8;
    } // namespace layer
    namespace weightRecord
    {
        inline constexpr size_t kX = 0;
        inline constexpr size_t kZ = 4;
        inline constexpr size_t kPack = 8;
        inline constexpr size_t kOffset = 16;
        inline constexpr size_t kLength = 24;
        inline constexpr size_t kChecksum = 32;
        inline constexpr size_t kSlots = 40;
    } // namespace weightRecord
    namespace regionWeightRecord
    {
        inline constexpr size_t kLevel = 0;
        inline constexpr size_t kX = 4;
        inline constexpr size_t kZ = 8;
        inline constexpr size_t kPack = 12;
        inline constexpr size_t kOffset = 16;
        inline constexpr size_t kLength = 24;
        inline constexpr size_t kChecksum = 32;
        inline constexpr size_t kSlots = 40;
        inline constexpr size_t kSide = 44;
    } // namespace regionWeightRecord
    namespace pack
    {
        inline constexpr size_t kMajor = 4;
        inline constexpr size_t kHeaderSize = 12;
        inline constexpr size_t kAsset = 16;
        inline constexpr size_t kRelease = 24;
        inline constexpr size_t kX = 32;
        inline constexpr size_t kZ = 36;
        inline constexpr size_t kTileCount = 40;
        inline constexpr size_t kContentVersion = 44;
    } // namespace pack
    namespace derived
    {
        inline constexpr size_t kMagic = 0;
        inline constexpr size_t kContentVersion = 4;
        inline constexpr size_t kX = 8;
        inline constexpr size_t kZ = 12;
        inline constexpr size_t kSourceSpacing = 16;
        inline constexpr size_t kSpacing = 24;
        inline constexpr size_t kSourceChecksum = 32;
        inline constexpr size_t kTopologyHash = 40;
        inline constexpr size_t kReserved0 = 48;
        inline constexpr size_t kVertexCount = 56;
        inline constexpr size_t kIndexCount = 60;
        inline constexpr size_t kDeviation = 64;
        inline constexpr size_t kReserved1 = 72;
    } // namespace derived
    namespace region
    {
        inline constexpr size_t kMagic = 0;
        inline constexpr size_t kContentVersion = 4;
        inline constexpr size_t kLevel = 8;
        inline constexpr size_t kX = 12;
        inline constexpr size_t kZ = 16;
        inline constexpr size_t kReserved0 = 20;
        inline constexpr size_t kOriginX = 24;
        inline constexpr size_t kOriginZ = 32;
        inline constexpr size_t kSpacing = 40;
        inline constexpr size_t kMinTileX = 48;
        inline constexpr size_t kMinTileZ = 52;
        inline constexpr size_t kMaxTileX = 56;
        inline constexpr size_t kMaxTileZ = 60;
        inline constexpr size_t kSourceChecksum = 64;
        inline constexpr size_t kTopologyHash = 72;
        inline constexpr size_t kVertexCount = 80;
        inline constexpr size_t kIndexCount = 84;
        inline constexpr size_t kDeviation = 88;
        inline constexpr size_t kReserved1 = 96;
        inline constexpr size_t kReserved2 = 104;
    } // namespace region
} // namespace mir::terrainformat

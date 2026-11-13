/**
 * @file TerrainPackFormat.cpp
 * @author Jackey Cen
 * @brief Terrain release index and tile pack format: records, parsing and decoding.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TerrainPackFormat.h"

#include "LittleEndian.h"
#include "PackInternal.h"

#include <assets/AssetId.h>
#include <terrain/TerrainRegion.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>
#include <tuple>
#include <bit>

namespace mir
{
    namespace
    {
        using Code = TerrainPackErrorCode;
        using detail::Describe;
        using detail::Error;
        using detail::Fail;
        using detail::Hex;
        using detail::LoadF32;
        using detail::LoadF64;
        using detail::LoadI32;
        using detail::LoadU32;
        using detail::LoadU64;

        int32_t FloorDiv(int32_t value, int32_t divisor)
        {
            int32_t quotient = value / divisor;
            if (value % divisor != 0 && (value < 0) != (divisor < 0))
                --quotient;
            return quotient;
        }

        bool IsPowerOfTwo(uint32_t v)
        {
            return v != 0 && (v & (v - 1)) == 0;
        }

        std::string DescribeBounds(const TerrainLocalBounds &b)
        {
            return std::format("[{}, {}]..[{}, {}]", b.minX, b.minZ, b.maxX, b.maxZ);
        }

        struct Header
        {
            uint32_t headerSize = 0;
            uint32_t minor = 0;
            uint32_t contentVersion = 0;
            uint32_t alignment = 0;
            uint64_t asset = 0;
            uint64_t release = 0;
            uint64_t source = 0;
            double spacing = 0.0;
            int32_t intervals = 0;
            int32_t chunkTiles = 0;
            TerrainLocalBounds bounds;
            uint32_t packCount = 0;
            uint32_t packRecordSize = 0;
            uint32_t tileCount = 0;
            uint32_t tileRecordSize = 0;
            uint64_t heightsHash = 0;
            uint32_t layerCount = 0;
            uint32_t layerRecordSize = 0;
            uint32_t regionCount = 0;
            uint32_t regionRecordSize = 0;
            uint32_t regionTiles = 0;
            uint32_t regionCells = 0;
            uint32_t regionUnit = 0;
            uint32_t regionLevels = 0;
            uint32_t weightPackCount = 0;
            uint32_t weightPackRecordSize = 0;
            uint32_t weightTileCount = 0;
            uint32_t weightTileRecordSize = 0;
            uint32_t weightLayerCount = 0;
            uint32_t weightLayerBytes = 0;
            uint64_t weightsFingerprint = 0;
        };

        TerrainResult<Header> ParseHeader(std::span<const std::byte> bytes)
        {
            constexpr size_t kPrefix = terrainformat::kPrefixSize;
            if (bytes.size() < kPrefix)
                return Fail<Header>(Code::Malformed, "{} bytes is too short for an index header (truncated?)",
                                    bytes.size());
            if (!std::equal(terrainformat::kIndexMagic.begin(), terrainformat::kIndexMagic.end(), bytes.begin()))
                return Fail<Header>(Code::Malformed, "bad magic: not a terrain release index");

            const uint32_t format = LoadU32(bytes, terrainformat::index::kMajor);
            if (format != kTerrainFormatVersion.value)
                return Fail<Header>(Code::Unsupported,
                                    "format version {} is not supported (this reader reads {}); rebake it with a "
                                    "matching Terrain Tool",
                                    format, kTerrainFormatVersion.value);

            Header h;
            h.minor = LoadU32(bytes, terrainformat::index::kMinor);
            h.headerSize = LoadU32(bytes, terrainformat::index::kHeaderSize);
            const uint32_t minimumHeader =
                h.minor >= kTerrainRegionsFormatMinor ? kTerrainIndexHeaderSize : kTerrainLegacyIndexHeaderSize;
            if (h.headerSize < minimumHeader || h.headerSize % terrainformat::kRecordAlignment != 0)
                return Fail<Header>(Code::Malformed, "header size {} is invalid (needs >= {} and a multiple of 8)",
                                    h.headerSize, minimumHeader);
            if (bytes.size() < uint64_t(h.headerSize) + kTerrainIndexTrailerSize)
                return Fail<Header>(Code::Malformed,
                                    "{} bytes cannot hold a {}-byte header and the checksum (truncated?)", bytes.size(),
                                    h.headerSize);

            // Checked before any table is read, so an edited index never yields records.
            const size_t body = bytes.size() - kTerrainIndexTrailerSize;
            const uint64_t stored = LoadU64(bytes, body);
            const uint64_t actual = Fnv1a64(bytes.first(body));
            if (stored != actual)
                return Fail<Header>(Code::Corrupt,
                                    "checksum {} does not match the contents {} (truncated or corrupt index)",
                                    Hex(stored), Hex(actual));

            h.contentVersion = LoadU32(bytes, terrainformat::index::kContentVersion);
            if (h.contentVersion != kTerrainHeightsContentVersion)
                return Fail<Header>(Code::Unsupported, "content version {} is not supported (this reader reads {})",
                                    h.contentVersion, kTerrainHeightsContentVersion);
            h.alignment = LoadU32(bytes, terrainformat::index::kAlignment);
            h.asset = LoadU64(bytes, terrainformat::index::kAsset);
            h.release = LoadU64(bytes, terrainformat::index::kRelease);
            h.source = LoadU64(bytes, terrainformat::index::kSource);
            h.spacing = LoadF64(bytes, terrainformat::index::kSpacing);
            h.intervals = LoadI32(bytes, terrainformat::index::kIntervals);
            h.chunkTiles = LoadI32(bytes, terrainformat::index::kChunkTiles);
            h.bounds = {LoadF64(bytes, terrainformat::index::kMinX), LoadF64(bytes, terrainformat::index::kMinZ),
                        LoadF64(bytes, terrainformat::index::kMaxX), LoadF64(bytes, terrainformat::index::kMaxZ)};
            h.packCount = LoadU32(bytes, terrainformat::index::kPackCount);
            h.packRecordSize = LoadU32(bytes, terrainformat::index::kPackRecordSize);
            h.tileCount = LoadU32(bytes, terrainformat::index::kTileCount);
            h.tileRecordSize = LoadU32(bytes, terrainformat::index::kTileRecordSize);
            h.heightsHash = LoadU64(bytes, terrainformat::index::kHeightsHash);
            h.layerCount = LoadU32(bytes, terrainformat::index::kLayerCount);
            h.layerRecordSize = LoadU32(bytes, terrainformat::index::kLayerRecordSize);
            if (h.layerCount == 0 || h.layerRecordSize < kTerrainLayerRecordSize ||
                h.layerRecordSize % terrainformat::kRecordAlignment != 0)
                return Fail<Header>(Code::Malformed, "layer count {} or record size {} is invalid", h.layerCount,
                                    h.layerRecordSize);
            if (h.minor >= kTerrainRegionsFormatMinor)
            {
                h.regionCount = LoadU32(bytes, terrainformat::index::kRegionCount);
                h.regionRecordSize = LoadU32(bytes, terrainformat::index::kRegionRecordSize);
                h.regionTiles = LoadU32(bytes, terrainformat::index::kRegionTiles);
                h.regionCells = LoadU32(bytes, terrainformat::index::kRegionCells);
                h.regionUnit = LoadU32(bytes, terrainformat::index::kRegionUnit);
                h.regionLevels = LoadU32(bytes, terrainformat::index::kRegionLevels);
                if (h.regionRecordSize < kTerrainTileRecordSize ||
                    h.regionRecordSize % terrainformat::kRecordAlignment != 0)
                    return Fail<Header>(Code::Malformed,
                                        "Region record size {} is invalid (needs >= {}, a multiple of 8)",
                                        h.regionRecordSize, kTerrainTileRecordSize);
                if (h.regionTiles == 0 &&
                    (h.regionCount != 0 || h.regionCells != 0 || h.regionUnit != 0 || h.regionLevels != 0))
                    return Fail<Header>(Code::Malformed,
                                        "an index without Regions has Region count {}, cells {}, "
                                        "unit {} or levels {}",
                                        h.regionCount, h.regionCells, h.regionUnit, h.regionLevels);
            }
            // A weight header may be absent from a minor that knows the section, as an older writer left it.
            if (h.minor >= kTerrainWeightsFormatMinor && h.headerSize >= kTerrainWeightsIndexHeaderSize)
            {
                h.weightPackCount = LoadU32(bytes, terrainformat::index::kWeightPackCount);
                h.weightPackRecordSize = LoadU32(bytes, terrainformat::index::kWeightPackRecordSize);
                h.weightTileCount = LoadU32(bytes, terrainformat::index::kWeightTileCount);
                h.weightTileRecordSize = LoadU32(bytes, terrainformat::index::kWeightTileRecordSize);
                h.weightLayerCount = LoadU32(bytes, terrainformat::index::kWeightLayerCount);
                h.weightLayerBytes = LoadU32(bytes, terrainformat::index::kWeightLayerBytes);
                h.weightsFingerprint = LoadU64(bytes, terrainformat::index::kWeightsFingerprint);
                if (h.weightPackRecordSize < kTerrainPackRecordSize ||
                    h.weightPackRecordSize % terrainformat::kRecordAlignment != 0)
                    return Fail<Header>(Code::Malformed,
                                        "weight pack record size {} is invalid (needs >= {}, a multiple of 8)",
                                        h.weightPackRecordSize, kTerrainPackRecordSize);
                if (h.weightTileRecordSize < kTerrainWeightRecordSize ||
                    h.weightTileRecordSize % terrainformat::kRecordAlignment != 0)
                    return Fail<Header>(Code::Malformed,
                                        "weight tile record size {} is invalid (needs >= {}, a multiple of 8)",
                                        h.weightTileRecordSize, kTerrainWeightRecordSize);
                if (h.weightLayerCount > uint32_t(kMaxTerrainMaterialLayers))
                    return Fail<Header>(Code::Malformed, "weight layer count {} exceeds this reader's limit {}",
                                        h.weightLayerCount, kMaxTerrainMaterialLayers);
                if (h.weightTileCount == 0 &&
                    (h.weightPackCount != 0 || h.weightLayerCount != 0 || h.weightLayerBytes != 0))
                    return Fail<Header>(Code::Malformed,
                                        "an index without weight tiles has {} weight packs, {} weight layers "
                                        "and {} table bytes",
                                        h.weightPackCount, h.weightLayerCount, h.weightLayerBytes);
            }

            if (!IsPowerOfTwo(h.alignment) || h.alignment > kMaxTerrainPayloadAlignment)
                return Fail<Header>(Code::Malformed, "payload alignment {} is not a power of two up to {}", h.alignment,
                                    kMaxTerrainPayloadAlignment);
            if (h.asset == 0)
                return Fail<Header>(Code::Malformed, "asset id is zero");
            if (h.release == 0)
                return Fail<Header>(Code::Malformed, "release id is zero");
            if (h.packRecordSize < kTerrainPackRecordSize || h.packRecordSize % terrainformat::kRecordAlignment != 0)
                return Fail<Header>(Code::Malformed, "pack record size {} is invalid (needs >= {}, a multiple of 8)",
                                    h.packRecordSize, kTerrainPackRecordSize);
            if (h.tileRecordSize < (h.minor == 0 ? kTerrainLegacyTileRecordSize : kTerrainTileRecordSize) ||
                h.tileRecordSize % terrainformat::kRecordAlignment != 0)
                return Fail<Header>(Code::Malformed, "tile record size {} is invalid (needs >= {}, a multiple of 8)",
                                    h.tileRecordSize, kTerrainTileRecordSize);

            // Each product is below 2^64; subtracting keeps the sum from overflowing.
            const uint64_t room = body - h.headerSize;
            const uint64_t packBytes = uint64_t(h.packCount) * h.packRecordSize;
            const uint64_t tileBytes = uint64_t(h.tileCount) * h.tileRecordSize;
            const uint64_t layerBytes = uint64_t(h.layerCount) * h.layerRecordSize;
            const uint64_t regionBytes = uint64_t(h.regionCount) * h.regionRecordSize;
            const uint64_t weightPackBytes = uint64_t(h.weightPackCount) * h.weightPackRecordSize;
            const uint64_t weightTileBytes = uint64_t(h.weightTileCount) * h.weightTileRecordSize;
            if (packBytes > room || tileBytes > room - packBytes || layerBytes > room - packBytes - tileBytes ||
                regionBytes > room - packBytes - tileBytes - layerBytes ||
                weightPackBytes > room - packBytes - tileBytes - layerBytes - regionBytes ||
                weightTileBytes >
                    room - packBytes - tileBytes - layerBytes - regionBytes - weightPackBytes ||
                h.weightLayerBytes >
                    room - packBytes - tileBytes - layerBytes - regionBytes - weightPackBytes - weightTileBytes)
                return Fail<Header>(Code::Malformed,
                                    "{} pack, {} tile, {} Region, {} weight pack and {} weight tile records do not "
                                    "fit in {} bytes (truncated?)",
                                    h.packCount, h.tileCount, h.regionCount, h.weightPackCount, h.weightTileCount,
                                    bytes.size());

            if (h.intervals < 1)
                return Fail<Header>(Code::Malformed, "intervals per tile {} is below 1", h.intervals);
            if (h.intervals > kMaxTerrainTileIntervals)
                return Fail<Header>(Code::Unsupported, "intervals per tile {} exceeds this reader's limit {}",
                                    h.intervals, kMaxTerrainTileIntervals);
            if (h.chunkTiles < 1)
                return Fail<Header>(Code::Malformed, "tiles per chunk {} is below 1", h.chunkTiles);

            TerrainResult<Header> result;
            result.value = h;
            return result;
        }

        TerrainPackRecord ReadPackRecord(std::span<const std::byte> b, size_t at)
        {
            TerrainPackRecord r;
            r.chunk = {LoadI32(b, at + terrainformat::packRecord::kX), LoadI32(b, at + terrainformat::packRecord::kZ)};
            r.tileCount = LoadU32(b, at + terrainformat::packRecord::kTileCount);
            r.fileSize = LoadU64(b, at + terrainformat::packRecord::kFileSize);
            r.checksum = LoadU64(b, at + terrainformat::packRecord::kChecksum);
            return r;
        }

        TerrainTileRecord ReadTileRecord(std::span<const std::byte> b, size_t at, uint32_t size)
        {
            TerrainTileRecord r;
            r.tile = {LoadI32(b, at + terrainformat::payload::kX), LoadI32(b, at + terrainformat::payload::kZ)};
            r.pack = LoadU32(b, at + terrainformat::payload::kPack);
            r.samplesPerSide = static_cast<int32_t>(LoadU32(b, at + terrainformat::payload::kSamplesPerSide));
            r.offset = LoadU64(b, at + terrainformat::payload::kOffset);
            r.length = LoadU64(b, at + terrainformat::payload::kLength);
            r.checksum = LoadU64(b, at + terrainformat::payload::kChecksum);
            r.sampleSpacing = LoadF64(b, at + terrainformat::payload::kSpacing);
            r.minHeight = LoadF32(b, at + terrainformat::payload::kMinHeight);
            r.maxHeight = LoadF32(b, at + terrainformat::payload::kMaxHeight);
            r.validExtent = {
                LoadF64(b, at + terrainformat::payload::kMinX), LoadF64(b, at + terrainformat::payload::kMinZ),
                LoadF64(b, at + terrainformat::payload::kMaxX), LoadF64(b, at + terrainformat::payload::kMaxZ)};
            r.layer = LoadU32(b, at + terrainformat::payload::kLayer);
            r.kind = static_cast<TerrainPayloadKind>(LoadU32(b, at + terrainformat::payload::kKind));
            r.level = LoadU32(b, at + terrainformat::payload::kLevel);
            r.source = LoadU32(b, at + terrainformat::payload::kSource) != 0;
            if (LoadU32(b, at + terrainformat::payload::kHasDeviation) != 0)
                r.maxDeviation = LoadF64(b, at + terrainformat::payload::kDeviation);
            if (size >= kTerrainTileRecordSize)
            {
                r.sourceChecksum = LoadU64(b, at + terrainformat::payload::kSourceChecksum);
                r.topologyHash = LoadU64(b, at + terrainformat::payload::kTopologyHash);
                r.vertexCount = LoadU32(b, at + terrainformat::payload::kVertexCount);
                r.indexCount = LoadU32(b, at + terrainformat::payload::kIndexCount);
            }
            return r;
        }

        auto RecordKey(const TerrainTileRecord &r)
        {
            return std::tuple(r.tile.z, r.tile.x, r.kind, r.level, r.layer);
        }

        TerrainWeightRecord ReadWeightRecord(std::span<const std::byte> b, size_t at)
        {
            TerrainWeightRecord r;
            r.tile = {LoadI32(b, at + terrainformat::weightRecord::kX),
                      LoadI32(b, at + terrainformat::weightRecord::kZ)};
            r.pack = LoadU32(b, at + terrainformat::weightRecord::kPack);
            r.offset = LoadU64(b, at + terrainformat::weightRecord::kOffset);
            r.length = LoadU64(b, at + terrainformat::weightRecord::kLength);
            r.checksum = LoadU64(b, at + terrainformat::weightRecord::kChecksum);
            for (size_t s = 0; s < kTerrainWeightSlots; ++s)
                r.slots[s] = std::to_integer<uint8_t>(b[at + terrainformat::weightRecord::kSlots + s]);
            return r;
        }

        TerrainPackError CheckTileRecord(const Header &h, const TerrainLayout &layout,
                                         std::span<const TerrainPackRecord> packs, const TerrainTileRecord &r)
        {
            const std::string tile = Describe(r.tile);
            if (r.pack >= packs.size())
                return Error(Code::Malformed, "tile {}: pack {} is out of range ({} packs)", tile, r.pack,
                             packs.size());
            const TerrainPackRecord &pack = packs[r.pack];
            const std::string name = TerrainPackFileName(pack.chunk);
            const TerrainChunkCoord chunk = TerrainChunkOf(r.tile, h.chunkTiles);
            if (pack.chunk != chunk)
                return Error(Code::Malformed, "tile {}: stored in {} but belongs to chunk {} at {} tiles per chunk",
                             tile, name, Describe(chunk), h.chunkTiles);

            const int32_t side = layout.IntervalsPerTile() + 1;
            if (r.samplesPerSide != side || r.sampleSpacing != layout.SampleSpacing())
                return Error(Code::Malformed, "tile {}: {} samples per side at spacing {}, the layout has {} at {}",
                             tile, r.samplesPerSide, r.sampleSpacing, side, layout.SampleSpacing());
            const uint64_t length = uint64_t(side) * uint64_t(side) * sizeof(float);
            if (r.kind == TerrainPayloadKind::Height && r.length != length)
                return Error(Code::Malformed, "tile {}: payload length {} should be {}", tile, r.length, length);
            if (r.offset % h.alignment != 0)
                return Error(Code::Malformed, "tile {}: offset {} is not a multiple of the payload alignment {}", tile,
                             r.offset, h.alignment);
            if (r.offset < kTerrainPackHeaderSize)
                return Error(Code::Malformed, "tile {}: offset {} is inside the pack header of {}", tile, r.offset,
                             name);
            if (r.offset > std::numeric_limits<uint64_t>::max() - r.length)
                return Error(Code::Malformed, "tile {}: offset {} + length {} overflows", tile, r.offset, r.length);
            if (r.offset + r.length > pack.fileSize)
                return Error(Code::Malformed, "tile {}: payload [{}, {}) ends past {} ({} bytes)", tile, r.offset,
                             r.offset + r.length, name, pack.fileSize);
            if (!std::isfinite(r.minHeight) || !std::isfinite(r.maxHeight) || r.minHeight > r.maxHeight)
                return Error(Code::Malformed, "tile {}: height range [{}, {}] is invalid", tile, r.minHeight,
                             r.maxHeight);
            if (r.kind == TerrainPayloadKind::Region ||
                static_cast<uint32_t>(r.kind) > static_cast<uint32_t>(TerrainPayloadKind::Region))
                return Error(Code::Unsupported, "tile {}: unsupported payload kind", tile);
            if (r.kind == TerrainPayloadKind::Height)
            {
                if (r.level != 0 || (r.source && r.sourceChecksum) || r.topologyHash || r.vertexCount || r.indexCount)
                    return Error(Code::Malformed, "tile {}: regular Height has derived topology fields", tile);
            }
            else
            {
                const uint64_t expected =
                    uint64_t(kTerrainDerivedHeaderSize) + uint64_t(r.vertexCount) * terrainformat::kVec3Bytes +
                    (r.kind == TerrainPayloadKind::LodHeight ? uint64_t(r.indexCount) * terrainformat::kIndexBytes : 0);
                if (h.minor < 1 || r.source || !r.maxDeviation || r.vertexCount < 4 || r.indexCount < 6 ||
                    r.indexCount % 3 || r.length != expected)
                    return Error(Code::Malformed, "tile {}: derived count/source/length/error fields invalid", tile);
            }
            if (r.maxDeviation && (!std::isfinite(*r.maxDeviation) || *r.maxDeviation < 0.0))
                return Error(Code::Malformed, "tile {}: layer {} deviation is invalid", tile, r.layer);
            if (!layout.Contains(r.tile))
                return Error(Code::Malformed, "tile {} is outside the layer {} layout", tile, r.layer);
            const TerrainLocalBounds extent = *layout.ValidExtent(r.tile);
            if (r.validExtent != extent)
                return Error(Code::Malformed, "tile {}: valid extent {} differs from the layout's {}", tile,
                             DescribeBounds(r.validExtent), DescribeBounds(extent));
            return {};
        }

        std::string DescribeRegion(TerrainRegionCoord region)
        {
            return std::format("Region L{} ({}, {})", region.level, region.x, region.z);
        }

        std::string DescribeRecord(const TerrainTileRecord &r)
        {
            return r.kind == TerrainPayloadKind::Region ? DescribeRegion({r.level, r.tile.x, r.tile.z})
                                                        : "tile " + Describe(r.tile);
        }

        TerrainPackError CheckRegionRecord(const Header &h, const TerrainRegionGrid &grid,
                                           std::span<const TerrainPackRecord> packs, const TerrainTileRecord &r,
                                           TerrainRegionCoord want, uint64_t sourceChecksum)
        {
            const std::string name = DescribeRegion(want);
            const TerrainRegionCoord have{r.level, r.tile.x, r.tile.z};
            if (have != want)
                return Error(Code::Malformed,
                             "Region records must list every Region once in (level, z, x) order: "
                             "{} is where {} belongs",
                             DescribeRegion(have), name);
            if (r.kind != TerrainPayloadKind::Region || r.layer != 0 || r.source)
                return Error(Code::Malformed, "{}: kind, layer or source flag is not a Region's", name);
            if (r.samplesPerSide != grid.SamplesPerSide(want.level) || r.sampleSpacing != grid.CellSpacing(want.level))
                return Error(Code::Malformed, "{}: {} nodes per side at {} m, the grid has {} at {} m", name,
                             r.samplesPerSide, r.sampleSpacing, grid.SamplesPerSide(want.level),
                             grid.CellSpacing(want.level));
            const TerrainLocalBounds extent = grid.ValidExtent(want);
            if (r.validExtent != extent)
                return Error(Code::Malformed, "{}: valid extent {} differs from the grid's {}", name,
                             DescribeBounds(r.validExtent), DescribeBounds(extent));
            if (!std::isfinite(r.minHeight) || !std::isfinite(r.maxHeight) || r.minHeight > r.maxHeight)
                return Error(Code::Malformed, "{}: height range [{}, {}] is invalid", name, r.minHeight, r.maxHeight);
            if (!r.maxDeviation || !std::isfinite(*r.maxDeviation) || *r.maxDeviation < 0.0)
                return Error(Code::Malformed, "{}: error bound is missing or invalid", name);
            if (r.vertexCount < 3 || r.indexCount < 3 || r.indexCount % 3 != 0 ||
                r.length != TerrainRegionPayloadLength(r.vertexCount, r.indexCount))
                return Error(Code::Malformed, "{}: payload length {} does not hold {} vertices and {} indices", name,
                             r.length, r.vertexCount, r.indexCount);
            if (r.sourceChecksum != sourceChecksum)
                return Error(Code::Malformed, "{}: source checksum {} differs from its map tiles' {}", name,
                             Hex(r.sourceChecksum), Hex(sourceChecksum));
            if (r.pack >= packs.size())
                return Error(Code::Malformed, "{}: pack {} is out of range ({} packs)", name, r.pack, packs.size());
            const TerrainPackRecord &pack = packs[r.pack];
            const TerrainChunkCoord chunk = TerrainChunkOf(grid.MapTiles(want).min, h.chunkTiles);
            if (pack.chunk != chunk)
                return Error(Code::Malformed, "{}: stored in {} but belongs with its first map tile in chunk {}", name,
                             TerrainPackFileName(pack.chunk), Describe(chunk));
            if (r.offset % h.alignment != 0 || r.offset < kTerrainPackHeaderSize)
                return Error(Code::Malformed, "{}: offset {} is unaligned or inside the pack header", name, r.offset);
            if (r.offset > std::numeric_limits<uint64_t>::max() - r.length || r.offset + r.length > pack.fileSize)
                return Error(Code::Malformed, "{}: payload [{}, {}) ends past {} ({} bytes)", name, r.offset,
                             r.offset + r.length, TerrainPackFileName(pack.chunk), pack.fileSize);
            return {};
        }
    }

    namespace detail
    {
        uint64_t HeightsHashOf(std::span<const TerrainTileRecord> records)
        {
            return HeightsHashOf(records, {});
        }

        uint64_t HeightsHashOf(std::span<const TerrainTileRecord> tiles, std::span<const TerrainTileRecord> regions)
        {
            std::vector<std::byte> bytes;
            bytes.reserve((tiles.size() + regions.size()) * 56);
            LeWriter out(bytes);
            std::vector<const TerrainTileRecord *> records;
            records.reserve(tiles.size() + regions.size());
            for (const TerrainTileRecord &r : tiles)
                records.push_back(&r);
            for (const TerrainTileRecord &r : regions)
                records.push_back(&r);
            for (const TerrainTileRecord *record : records)
            {
                const TerrainTileRecord &r = *record;
                out.I32(r.tile.x);
                out.I32(r.tile.z);
                out.U32(r.layer);
                out.U32(static_cast<uint32_t>(r.kind));
                out.U32(r.level);
                out.U32(r.source ? 1 : 0);
                out.F64(r.sampleSpacing);
                out.I32(r.samplesPerSide);
                out.U32(r.maxDeviation ? 1 : 0);
                out.F64(r.maxDeviation.value_or(0.0));
                out.U64(r.checksum);
                if (r.sourceChecksum)
                    out.U64(r.sourceChecksum);
            }
            return Fnv1a64(bytes);
        }
    }

    uint64_t TerrainRecordsHeightsHash(std::span<const TerrainTileRecord> records)
    {
        return detail::HeightsHashOf(records);
    }

    uint64_t TerrainRecordsHeightsHash(std::span<const TerrainTileRecord> records,
                                       std::span<const TerrainTileRecord> regions)
    {
        return detail::HeightsHashOf(records, regions);
    }

    TerrainChunkCoord TerrainChunkOf(TileCoord tile, int32_t chunkTiles)
    {
        return {FloorDiv(tile.x, chunkTiles), FloorDiv(tile.z, chunkTiles)};
    }

    std::string TerrainPackFileName(TerrainChunkCoord chunk)
    {
        return std::format("chunk_{}_{}.tpack", chunk.x, chunk.z);
    }

    std::string TerrainWeightPackFileName(TerrainChunkCoord chunk)
    {
        return std::format("chunk_{}_{}.wpack", chunk.x, chunk.z);
    }

    bool TerrainLayersNest(const TerrainLayout &fine, const TerrainLayout &coarse)
    {
        if (coarse.SampleSpacing() <= fine.SampleSpacing() ||
            fine.IntervalsPerTile() % coarse.IntervalsPerTile() != 0 || fine.Bounds() != coarse.Bounds())
            return false;
        const int32_t ratio = fine.IntervalsPerTile() / coarse.IntervalsPerTile();
        if (coarse.SampleSpacing() != fine.SampleSpacing() * ratio ||
            (fine.Tiles().min != coarse.Tiles().min || fine.Tiles().max != coarse.Tiles().max))
            return false;
        const auto equalEdge = [&](int64_t t)
        {
            const double a = fine.SamplePosition(t * fine.IntervalsPerTile());
            const double b = coarse.SamplePosition(t * coarse.IntervalsPerTile());
            return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
        };
        const TileRange range = fine.Tiles();
        for (int64_t x = range.min.x; x <= int64_t(range.max.x) + 1; ++x)
            if (!equalEdge(x))
                return false;
        for (int64_t z = range.min.z; z <= int64_t(range.max.z) + 1; ++z)
            if (!equalEdge(z))
                return false;
        return true;
    }

    const TerrainTileRecord *TerrainIndex::Find(TileCoord tile) const
    {
        auto at = std::lower_bound(tiles.begin(), tiles.end(), tile,
                                   [](const TerrainTileRecord &r, TileCoord t)
                                   {
                                       return detail::LessZX(r.tile.x, r.tile.z, t.x, t.z);
                                   });
        for (; at != tiles.end() && at->tile == tile; ++at)
            if (at->source)
                return &*at;
        return nullptr;
    }

    const TerrainTileRecord *TerrainIndex::Find(TileCoord tile, uint32_t layer, TerrainPayloadKind kind,
                                                uint32_t level) const
    {
        if (kind == TerrainPayloadKind::Region)
            return layer == 0 ? FindRegion({level, tile.x, tile.z}) : nullptr;
        TerrainTileRecord key;
        key.tile = tile;
        key.layer = layer;
        key.kind = kind;
        key.level = level;
        auto at = std::lower_bound(tiles.begin(), tiles.end(), key,
                                   [](const auto &a, const auto &b)
                                   {
                                       return RecordKey(a) < RecordKey(b);
                                   });
        return at != tiles.end() && RecordKey(*at) == RecordKey(key) ? &*at : nullptr;
    }

    const TerrainTileRecord *TerrainIndex::FindRegion(TerrainRegionCoord region) const
    {
        if (!regionGrid || !regionGrid->Contains(region))
            return nullptr;
        const uint64_t ordinal = regionGrid->Ordinal(region);
        return ordinal < regions.size() ? &regions[size_t(ordinal)] : nullptr;
    }

    const TerrainWeightRecord *TerrainIndex::FindWeight(TileCoord tile) const
    {
        auto at = std::lower_bound(weights.begin(), weights.end(), tile,
                                   [](const TerrainWeightRecord &r, TileCoord t)
                                   {
                                       return detail::LessZX(r.tile.x, r.tile.z, t.x, t.z);
                                   });
        return at != weights.end() && at->tile == tile ? &*at : nullptr;
    }

    TerrainResult<TerrainIndex> ParseTerrainIndex(std::span<const std::byte> bytes)
    {
        using R = TerrainIndex;
        TerrainResult<Header> parsed = ParseHeader(bytes);
        if (!parsed)
            return detail::Failure<R>(parsed.error, "");
        const Header &h = *parsed.value;

        const std::optional<TerrainLayout> layout = TerrainLayout::Create(h.spacing, h.intervals, h.bounds);
        if (!layout)
            return Fail<R>(Code::Malformed, "layout (spacing {}, {} intervals per tile, bounds {}) is invalid",
                           h.spacing, h.intervals, DescribeBounds(h.bounds));

        const size_t layerTable =
            size_t(h.headerSize) + size_t(h.packCount) * h.packRecordSize + size_t(h.tileCount) * h.tileRecordSize;
        std::vector<TerrainLayout> layers;
        layers.reserve(h.layerCount);
        for (uint32_t l = 0; l < h.layerCount; ++l)
        {
            const size_t at = layerTable + size_t(l) * h.layerRecordSize;
            auto layer = TerrainLayout::Create(LoadF64(bytes, at + terrainformat::layer::kSpacing),
                                               LoadI32(bytes, at + terrainformat::layer::kIntervals), h.bounds);
            if (!layer || layer->IntervalsPerTile() > kMaxTerrainTileIntervals)
                return Fail<R>(Code::Malformed, "layer {}: spacing or intervals is invalid", l);
            if (l == 0 && (layer->SampleSpacing() != h.spacing || layer->IntervalsPerTile() != h.intervals))
                return Fail<R>(Code::Malformed, "layer 0 differs from the reference grid");
            if (l > 0 && !TerrainLayersNest(layers.back(), *layer))
                return Fail<R>(Code::Malformed, "layer {}: spacing {} does not nest or tile edges differ bitwise", l,
                               layer->SampleSpacing());
            layers.push_back(*layer);
        }

        std::vector<TerrainPackRecord> packs(h.packCount);
        for (uint32_t p = 0; p < h.packCount; ++p)
        {
            TerrainPackRecord &pack = packs[p];
            pack = ReadPackRecord(bytes, size_t(h.headerSize) + size_t(p) * h.packRecordSize);
            if (p > 0 && !detail::LessZX(packs[p - 1].chunk.x, packs[p - 1].chunk.z, pack.chunk.x, pack.chunk.z))
                return Fail<R>(Code::Malformed,
                               "pack records {} and {} (chunks {} and {}) are not in increasing (z, x) order or "
                               "name one chunk twice",
                               p - 1, p, Describe(packs[p - 1].chunk), Describe(pack.chunk));
            if (pack.tileCount == 0)
                return Fail<R>(Code::Malformed, "pack record {} ({}) holds no tiles", p,
                               TerrainPackFileName(pack.chunk));
            if (pack.fileSize < kTerrainPackHeaderSize)
                return Fail<R>(Code::Malformed, "pack record {} ({}): file size {} is smaller than a pack header", p,
                               TerrainPackFileName(pack.chunk), pack.fileSize);
        }

        const TileRange range = layout->Tiles();
        const uint64_t width = uint64_t(int64_t(range.max.x) - range.min.x + 1);
        const uint64_t height = uint64_t(int64_t(range.max.z) - range.min.z + 1);
        if (width > std::numeric_limits<uint32_t>::max() / height || width * height > h.tileCount)
            return Fail<R>(Code::Malformed,
                           "{} payload records cannot cover {} x {} map tiles: a source tile is missing", h.tileCount,
                           width, height);
        std::vector<uint32_t> sources(static_cast<size_t>(width * height), 0);
        std::vector<uint32_t> finest(sources.size(), h.layerCount);

        const size_t tileTable = size_t(h.headerSize) + size_t(h.packCount) * h.packRecordSize;
        std::vector<TerrainTileRecord> tiles(h.tileCount);
        std::vector<uint32_t> tilesPerPack(h.packCount, 0);
        for (uint32_t t = 0; t < h.tileCount; ++t)
        {
            TerrainTileRecord &r = tiles[t];
            r = ReadTileRecord(bytes, tileTable + size_t(t) * h.tileRecordSize, h.tileRecordSize);
            if (r.layer >= layers.size())
                return Fail<R>(Code::Malformed, "tile record {} {}: unknown layer {}", t, Describe(r.tile), r.layer);
            if (t > 0 && !(RecordKey(tiles[t - 1]) < RecordKey(r)))
                return Fail<R>(
                    Code::Malformed,
                    "tile records {} and {} duplicate a key or are not in increasing order (both name tile {})", t - 1,
                    t, Describe(r.tile));
            const size_t raw = tileTable + size_t(t) * h.tileRecordSize;
            if (LoadU32(bytes, raw + terrainformat::payload::kSource) > 1 ||
                LoadU32(bytes, raw + terrainformat::payload::kHasDeviation) > 1)
                return Fail<R>(Code::Malformed, "tile record {}: invalid source/deviation flag", t);
            const TerrainPackError checked = CheckTileRecord(h, layers[r.layer], packs, r);
            if (checked.code != Code::None)
                return detail::Failure<R>(checked, "");
            const size_t mapTile =
                size_t(int64_t(r.tile.z) - range.min.z) * width + size_t(int64_t(r.tile.x) - range.min.x);
            if (r.source && (r.kind != TerrainPayloadKind::Height || ++sources[mapTile] > 1))
                return Fail<R>(Code::Malformed, "tile {} has two source records", Describe(r.tile));
            if (r.kind == TerrainPayloadKind::Height)
                finest[mapTile] = std::min(finest[mapTile], r.layer);
            ++tilesPerPack[r.pack];
        }

        std::vector<TerrainPackRecord> weightPacks(h.weightPackCount);
        std::vector<TerrainWeightRecord> weights(h.weightTileCount);
        std::vector<std::string> weightLayers;
        if (h.weightTileCount != 0)
        {
            const size_t weightPackTable = size_t(h.headerSize) + size_t(h.packCount) * h.packRecordSize +
                                           size_t(h.tileCount) * h.tileRecordSize +
                                           size_t(h.layerCount) * h.layerRecordSize +
                                           size_t(h.regionCount) * h.regionRecordSize;
            for (uint32_t p = 0; p < h.weightPackCount; ++p)
            {
                TerrainPackRecord &pack = weightPacks[p];
                pack = ReadPackRecord(bytes, weightPackTable + size_t(p) * h.weightPackRecordSize);
                if (p > 0 && !detail::LessZX(weightPacks[p - 1].chunk.x, weightPacks[p - 1].chunk.z, pack.chunk.x,
                                             pack.chunk.z))
                    return Fail<R>(Code::Malformed,
                                   "weight pack records {} and {} (chunks {} and {}) are not in increasing (z, x) "
                                   "order or name one chunk twice",
                                   p - 1, p, Describe(weightPacks[p - 1].chunk), Describe(pack.chunk));
                if (pack.tileCount == 0)
                    return Fail<R>(Code::Malformed, "weight pack record {} ({}) holds no weight tiles", p,
                                   TerrainWeightPackFileName(pack.chunk));
                if (pack.fileSize < kTerrainPackHeaderSize)
                    return Fail<R>(Code::Malformed,
                                   "weight pack record {} ({}): file size {} is smaller than a pack header", p,
                                   TerrainWeightPackFileName(pack.chunk), pack.fileSize);
            }

            const size_t weightTileTable = weightPackTable + size_t(h.weightPackCount) * h.weightPackRecordSize;
            const size_t weightLayerTable = weightTileTable + size_t(h.weightTileCount) * h.weightTileRecordSize;
            const size_t tableEnd = weightLayerTable + h.weightLayerBytes;
            size_t at = weightLayerTable;
            std::set<std::string> names;
            weightLayers.reserve(h.weightLayerCount);
            for (uint32_t l = 0; l < h.weightLayerCount; ++l)
            {
                if (at > tableEnd || tableEnd - at < sizeof(uint32_t))
                    return Fail<R>(Code::Malformed, "weight layer {}: the name table ends before its length", l);
                const uint32_t length = LoadU32(bytes, at);
                at += sizeof(uint32_t);
                if (length == 0 || length > tableEnd - at)
                    return Fail<R>(Code::Malformed, "weight layer {}: name length {} exceeds the table", l, length);
                const std::string name(reinterpret_cast<const char *>(bytes.data() + at), length);
                if (!IsValidTerrainMaterialLayerName(name))
                    return Fail<R>(Code::Malformed, "weight layer {}: \"{}\" is not a valid material layer name", l,
                                   name);
                if (!names.insert(name).second)
                    return Fail<R>(Code::Malformed, "weight layer {}: duplicate name \"{}\"", l, name);
                weightLayers.push_back(name);
                at += length;
            }
            if (at != tableEnd)
                return Fail<R>(Code::Malformed, "weight layer table: {} bytes are spare after {} names",
                               tableEnd - at, h.weightLayerCount);

            std::vector<uint32_t> weightTilesPerPack(h.weightPackCount, 0);
            const uint64_t expectedLength = uint64_t(h.intervals + 1) * uint64_t(h.intervals + 1) * 4;
            for (uint32_t t = 0; t < h.weightTileCount; ++t)
            {
                TerrainWeightRecord &r = weights[t];
                r = ReadWeightRecord(bytes, weightTileTable + size_t(t) * h.weightTileRecordSize);
                const std::string tile = Describe(r.tile);
                if (t > 0 && !detail::LessZX(weights[t - 1].tile.x, weights[t - 1].tile.z, r.tile.x, r.tile.z))
                {
                    if (weights[t - 1].tile == r.tile)
                        return Fail<R>(Code::Malformed, "weight records {} and {} both name tile {}", t - 1, t, tile);
                    return Fail<R>(Code::Malformed,
                                   "weight records {} and {} are not in increasing (z, x) order", t - 1, t);
                }
                if (r.pack >= weightPacks.size())
                    return Fail<R>(Code::Malformed, "weight record {} tile {}: pack {} is out of range ({} weight packs)",
                                   t, tile, r.pack, weightPacks.size());
                const TerrainPackRecord &pack = weightPacks[r.pack];
                const TerrainChunkCoord chunk = TerrainChunkOf(r.tile, h.chunkTiles);
                if (pack.chunk != chunk)
                    return Fail<R>(Code::Malformed,
                                   "weight record {} tile {}: stored in {} but belongs to chunk {} at {} tiles per "
                                   "chunk",
                                   t, tile, TerrainWeightPackFileName(pack.chunk), Describe(chunk), h.chunkTiles);
                if (r.offset % h.alignment != 0 || r.offset < kTerrainPackHeaderSize)
                    return Fail<R>(Code::Malformed,
                                   "weight record {} tile {}: offset {} is unaligned or inside the pack header", t,
                                   tile, r.offset);
                if (r.offset > std::numeric_limits<uint64_t>::max() - r.length ||
                    r.offset + r.length > pack.fileSize)
                    return Fail<R>(Code::Malformed, "weight record {} tile {}: payload [{}, {}) ends past {} ({} bytes)",
                                   t, tile, r.offset, r.offset + r.length,
                                   TerrainWeightPackFileName(pack.chunk), pack.fileSize);
                if (r.length != expectedLength)
                    return Fail<R>(Code::Malformed, "weight record {} tile {}: payload length {} should be {}", t,
                                   tile, r.length, expectedLength);

                std::array<bool, size_t(kMaxTerrainMaterialLayers)> seen{};
                size_t used = 0;
                bool unused = false;
                for (size_t s = 0; s < kTerrainWeightSlots; ++s)
                {
                    const uint8_t slot = r.slots[s];
                    if (slot == 0xFF)
                    {
                        unused = true;
                        continue;
                    }
                    if (slot >= weightLayers.size())
                        return Fail<R>(Code::Malformed,
                                       "weight record {} tile {}: slot {} names layer {}, the table has {}", t, tile, s,
                                       slot, weightLayers.size());
                    if (unused || seen[slot])
                        return Fail<R>(Code::Malformed,
                                       "weight record {} tile {}: slots repeat a layer or a used slot follows an "
                                       "unused one",
                                       t, tile);
                    seen[slot] = true;
                    ++used;
                }
                if (used == 0)
                    return Fail<R>(Code::Malformed, "weight record {} tile {}: no used slot", t, tile);
                if (!layout->Contains(r.tile))
                    return Fail<R>(Code::Malformed, "weight record {} tile {} is outside the layout", t, tile);
                const size_t mapTile =
                    size_t(int64_t(r.tile.z) - range.min.z) * width + size_t(int64_t(r.tile.x) - range.min.x);
                if (sources[mapTile] != 1)
                    return Fail<R>(Code::Malformed, "weight record {} tile {}: no source height record", t, tile);
                ++weightTilesPerPack[r.pack];
            }

            for (uint32_t p = 0; p < h.weightPackCount; ++p)
                if (weightTilesPerPack[p] != weightPacks[p].tileCount)
                    return Fail<R>(Code::Malformed,
                                   "weight pack record {} ({}) says {} tiles, {} weight records point at it", p,
                                   TerrainWeightPackFileName(weightPacks[p].chunk), weightPacks[p].tileCount,
                                   weightTilesPerPack[p]);

            std::vector<const TerrainWeightRecord *> byOffset;
            byOffset.reserve(weights.size());
            for (const TerrainWeightRecord &r : weights)
                byOffset.push_back(&r);
            std::sort(byOffset.begin(), byOffset.end(),
                      [](const TerrainWeightRecord *a, const TerrainWeightRecord *b)
                      {
                          return a->pack != b->pack ? a->pack < b->pack : a->offset < b->offset;
                      });
            for (size_t i = 1; i < byOffset.size(); ++i)
            {
                const TerrainWeightRecord &a = *byOffset[i - 1];
                const TerrainWeightRecord &b = *byOffset[i];
                if (a.pack == b.pack && a.offset + a.length > b.offset)
                    return Fail<R>(Code::Malformed, "weight records for tiles {} and {} overlap in {}: [{}, {}) and "
                                                    "[{}, {})",
                                   Describe(a.tile), Describe(b.tile),
                                   TerrainWeightPackFileName(weightPacks[a.pack].chunk), a.offset, a.offset + a.length,
                                   b.offset, b.offset + b.length);
            }
        }

        for (size_t t = 0; t < sources.size(); ++t)
        {
            const TileCoord tile{static_cast<int32_t>(range.min.x + int64_t(t % width)),
                                 static_cast<int32_t>(range.min.z + int64_t(t / width))};
            if (sources[t] != 1)
                return Fail<R>(Code::Malformed, "tile {} has no source record", Describe(tile));
        }
        for (const auto &r : tiles)
        {
            const size_t t = size_t(int64_t(r.tile.z) - range.min.z) * width + size_t(int64_t(r.tile.x) - range.min.x);
            if (r.kind != TerrainPayloadKind::Height || r.sourceChecksum)
            {
                auto begin = std::lower_bound(tiles.begin(), tiles.end(), r.tile,
                                              [](const auto &v, TileCoord tile)
                                              {
                                                  return detail::LessZX(v.tile.x, v.tile.z, tile.x, tile.z);
                                              });
                auto end = begin;
                while (end != tiles.end() && end->tile == r.tile)
                    ++end;
                const auto source = std::find_if(begin, end,
                                                 [](const auto &v)
                                                 {
                                                     return v.source;
                                                 });
                if (source == end || r.sourceChecksum != source->checksum || r.layer < source->layer ||
                    (r.kind != TerrainPayloadKind::Height && r.level != r.layer - source->layer))
                    return Fail<R>(Code::Malformed, "derived source checksum/spacing/level differs");
                if (r.kind == TerrainPayloadKind::Normal)
                {
                    auto mesh = std::find_if(begin, end,
                                             [&](const auto &v)
                                             {
                                                 return v.tile == r.tile && v.layer == r.layer && v.level == r.level &&
                                                        v.kind == TerrainPayloadKind::LodHeight;
                                             });
                    if (mesh == end || mesh->topologyHash != r.topologyHash || mesh->vertexCount != r.vertexCount ||
                        mesh->indexCount != r.indexCount || mesh->maxDeviation != r.maxDeviation ||
                        mesh->minHeight != r.minHeight || mesh->maxHeight != r.maxHeight)
                        return Fail<R>(Code::Malformed, "normal topology/count/range differs from LOD record");
                }
            }
            if (r.source && r.layer != finest[t])
                return Fail<R>(Code::Malformed, "tile {} source layer {} is not its finest baked layer {}",
                               Describe(r.tile), r.layer, finest[t]);
        }

        std::optional<TerrainRegionGrid> regionGrid;
        std::vector<TerrainTileRecord> regions;
        if (h.regionTiles != 0)
        {
            regionGrid = TerrainRegionGrid::Create(*layout, h.regionTiles, h.regionCells, h.regionUnit);
            if (!regionGrid)
                return Fail<R>(Code::Malformed,
                               "Region grid of {} tiles, {} cells and unit {} does not fit the "
                               "reference grid",
                               h.regionTiles, h.regionCells, h.regionUnit);
            if (regionGrid->Levels() != h.regionLevels || regionGrid->RegionCount() != h.regionCount)
                return Fail<R>(Code::Malformed,
                               "the header lists {} Region levels and {} Regions, the grid has {} and {}",
                               h.regionLevels, h.regionCount, regionGrid->Levels(), regionGrid->RegionCount());
            std::vector<TerrainSourceChecksum> sourceChecksums;
            for (const TerrainTileRecord &r : tiles)
            {
                if (!r.source)
                    continue;
                // Region grid lines must be nodes of every tile's source grid.
                const int32_t step = layout->IntervalsPerTile() / layers[r.layer].IntervalsPerTile();
                if (h.regionUnit % uint32_t(step) != 0)
                    return Fail<R>(Code::Malformed, "Region unit {} is not a multiple of tile {}'s source step {}",
                                   h.regionUnit, Describe(r.tile), step);
                sourceChecksums.push_back({r.tile, r.checksum});
            }

            const size_t regionTable = layerTable + size_t(h.layerCount) * h.layerRecordSize;
            regions.resize(h.regionCount);
            std::vector<uint64_t> levelChecksums;
            uint32_t checksumLevel = std::numeric_limits<uint32_t>::max();
            for (uint32_t o = 0; o < h.regionCount; ++o)
            {
                const size_t raw = regionTable + size_t(o) * h.regionRecordSize;
                TerrainTileRecord &r = regions[o];
                r = ReadTileRecord(bytes, raw, h.regionRecordSize);
                if (LoadU32(bytes, raw + terrainformat::payload::kSource) > 1 ||
                    LoadU32(bytes, raw + terrainformat::payload::kHasDeviation) > 1)
                    return Fail<R>(Code::Malformed, "Region record {}: invalid source/deviation flag", o);
                const TerrainRegionCoord want = regionGrid->AtOrdinal(o);
                if (want.level != checksumLevel)
                {
                    levelChecksums = TerrainRegionSourceChecksums(*regionGrid, want.level, sourceChecksums);
                    checksumLevel = want.level;
                }
                const TerrainPackError checked = CheckRegionRecord(
                    h, *regionGrid, packs, r, want,
                    levelChecksums[size_t(want.z) * size_t(regionGrid->Count(want.level).x) + size_t(want.x)]);
                if (checked.code != Code::None)
                    return detail::Failure<R>(checked, "");
                ++tilesPerPack[r.pack];
            }
        }

        for (uint32_t p = 0; p < h.packCount; ++p)
        {
            if (tilesPerPack[p] != packs[p].tileCount)
                return Fail<R>(Code::Malformed, "pack record {} ({}) says {} tiles, {} tile records point at it", p,
                               TerrainPackFileName(packs[p].chunk), packs[p].tileCount, tilesPerPack[p]);
        }

        // Payloads never share bytes: no two ranges in one pack may overlap.
        std::vector<const TerrainTileRecord *> byOffset;
        byOffset.reserve(tiles.size() + regions.size());
        for (const TerrainTileRecord &r : tiles)
            byOffset.push_back(&r);
        for (const TerrainTileRecord &r : regions)
            byOffset.push_back(&r);
        std::sort(byOffset.begin(), byOffset.end(), [](const TerrainTileRecord *a, const TerrainTileRecord *b) {
            return a->pack != b->pack ? a->pack < b->pack : a->offset < b->offset;
        });
        for (size_t i = 1; i < byOffset.size(); ++i)
        {
            const TerrainTileRecord &a = *byOffset[i - 1];
            const TerrainTileRecord &b = *byOffset[i];
            if (a.pack == b.pack && a.offset + a.length > b.offset)
                return Fail<R>(Code::Malformed, "{} and {} overlap in {}: [{}, {}) and [{}, {})", DescribeRecord(a),
                               DescribeRecord(b), TerrainPackFileName(packs[a.pack].chunk), a.offset,
                               a.offset + a.length, b.offset, b.offset + b.length);
        }

        const uint64_t heightsHash = detail::HeightsHashOf(tiles, regions);
        if (heightsHash != h.heightsHash)
            return Fail<R>(Code::Corrupt, "heights hash {} does not match the tile records {}", Hex(h.heightsHash),
                           Hex(heightsHash));

        TerrainResult<TerrainIndex> result;
        result.value.emplace(TerrainIndex{
            .release = {AssetId{h.asset}, TerrainSourceFingerprint(h.source), kTerrainFormatVersion,
                        TerrainReleaseId(h.release)},
            .formatMinor = h.minor,
            .contentVersion = h.contentVersion,
            .payloadAlignment = h.alignment,
            .chunkTiles = h.chunkTiles,
            .layout = *layout,
            .layers = std::move(layers),
            .heightsHash = heightsHash,
            .packs = std::move(packs),
            .tiles = std::move(tiles),
            .regionGrid = std::move(regionGrid),
            .regions = std::move(regions),
            .weightLayers = std::move(weightLayers),
            .weightsFingerprint = h.weightsFingerprint,
            .weightPacks = std::move(weightPacks),
            .weights = std::move(weights),
        });
        return result;
    }

    TerrainResult<TileHeights> DecodeTerrainTile(const TerrainIndex &index, const TerrainTileRecord &record,
                                                 std::span<const std::byte> payload)
    {
        using R = TileHeights;
        const std::string tile = Describe(record.tile);
        if (record.layer >= index.layers.size())
            return Fail<R>(Code::Malformed, "tile {}: unknown layer {}", tile, record.layer);
        const int32_t intervals = index.layers[record.layer].IntervalsPerTile();
        if (record.samplesPerSide != intervals + 1)
            return Fail<R>(Code::Malformed, "tile {}: {} samples per side, the layout has {}", tile,
                           record.samplesPerSide, intervals + 1);
        if (record.sampleSpacing != index.layers[record.layer].SampleSpacing() ||
            record.length != uint64_t(intervals + 1) * (intervals + 1) * sizeof(float))
            return Fail<R>(Code::Malformed, "tile {}: spacing or payload length differs from layer {}", tile,
                           record.layer);
        if (record.kind != TerrainPayloadKind::Height || record.level != 0)
            return Fail<R>(Code::Unsupported, "tile {}: payload kind/level is not a height layer", tile);
        if (payload.size() != record.length)
            return Fail<R>(Code::Io, "tile {}: got {} payload bytes, the index says {}", tile, payload.size(),
                           record.length);
        const uint64_t checksum = Fnv1a64(payload);
        if (checksum != record.checksum)
            return Fail<R>(Code::Corrupt,
                           "tile {}: payload checksum {} does not match the index {}; the pack is corrupt or from "
                           "another release",
                           tile, Hex(checksum), Hex(record.checksum));

        const size_t side = static_cast<size_t>(record.samplesPerSide);
        std::vector<float> samples(side * side);
        float lo = std::numeric_limits<float>::infinity();
        float hi = -std::numeric_limits<float>::infinity();
        for (size_t s = 0; s < samples.size(); ++s)
        {
            const float h = LoadF32(payload, s * sizeof(float));
            if (!std::isfinite(h))
            {
                TerrainResult<R> bad =
                    Fail<R>(Code::Corrupt, "tile {}: sample ({}, {}) is not finite", tile, s % side, s / side);
                bad.error.nonFiniteSample =
                    GlobalSampleIndex{index.layers[record.layer].TileFirstSample(record.tile.x) + int64_t(s % side),
                                      index.layers[record.layer].TileFirstSample(record.tile.z) + int64_t(s / side)};
                return bad;
            }
            lo = std::min(lo, h);
            hi = std::max(hi, h);
            samples[s] = h;
        }
        if (lo != record.minHeight || hi != record.maxHeight)
            return Fail<R>(Code::Corrupt, "tile {}: heights span [{}, {}], the index says [{}, {}]", tile, lo, hi,
                           record.minHeight, record.maxHeight);

        std::optional<TileHeights> heights = TileHeights::Create(record.tile, intervals, std::move(samples));
        if (!heights)
            return Fail<R>(Code::Unsupported, "tile {}: {} intervals per tile is beyond this reader", tile, intervals);
        TerrainResult<TileHeights> result;
        result.value = std::move(heights);
        return result;
    }

    TerrainResult<TerrainDecodedWeights> DecodeTerrainWeightPayload(const TerrainIndex &index,
                                                                    const TerrainWeightRecord &record,
                                                                    std::span<const std::byte> payload)
    {
        using R = TerrainDecodedWeights;
        const std::string tile = Describe(record.tile);
        const size_t side = size_t(index.layout.IntervalsPerTile()) + 1;
        const uint64_t expectedLength = uint64_t(side) * side * kTerrainWeightSlots;
        if (record.length != expectedLength)
            return Fail<R>(Code::Malformed, "weight tile {}: payload length {} should be {}", tile, record.length,
                           expectedLength);
        if (payload.size() != record.length)
            return Fail<R>(Code::Io, "weight tile {}: got {} payload bytes, the index says {}", tile, payload.size(),
                           record.length);
        const uint64_t checksum = Fnv1a64(payload);
        if (checksum != record.checksum)
            return Fail<R>(Code::Corrupt,
                           "weight tile {}: payload checksum {} does not match the index {}; the weight pack is "
                           "corrupt or from another release",
                           tile, Hex(checksum), Hex(record.checksum));

        TerrainDecodedWeights decoded;
        decoded.tile = record.tile;
        decoded.intervals = index.layout.IntervalsPerTile();
        decoded.slots = record.slots;
        size_t used = 0;
        while (used < kTerrainWeightSlots && record.slots[used] != 0xFF)
        {
            if (record.slots[used] >= index.weightLayers.size())
                return Fail<R>(Code::Malformed, "weight tile {}: slot {} names layer {}, the table has {}", tile,
                               used, record.slots[used], index.weightLayers.size());
            decoded.layers[used] = index.weightLayers[record.slots[used]];
            ++used;
        }
        if (used == 0)
            return Fail<R>(Code::Malformed, "weight tile {}: no used slot", tile);

        decoded.bytes.assign(payload.begin(), payload.end());
        for (size_t texel = 0; texel < side * side; ++texel)
        {
            uint32_t sum = 0;
            for (size_t s = 0; s < kTerrainWeightSlots; ++s)
            {
                const uint8_t channel = std::to_integer<uint8_t>(decoded.bytes[texel * kTerrainWeightSlots + s]);
                if (s >= used && channel != 0)
                    return Fail<R>(Code::Corrupt, "weight tile {}: texel ({}, {}) writes an unused slot", tile,
                                   texel % side, texel / side);
                sum += channel;
            }
            if (sum != 255)
                return Fail<R>(Code::Corrupt, "weight tile {}: texel ({}, {}) weights sum to {}, not 255", tile,
                               texel % side, texel / side, sum);
        }

        TerrainResult<R> result;
        result.value = std::move(decoded);
        return result;
    }

    TerrainWeightTile ExpandTerrainWeights(const TerrainDecodedWeights &decoded)
    {
        TerrainWeightTile tile;
        tile.tile = decoded.tile;
        tile.intervals = decoded.intervals;
        size_t used = 0;
        while (used < kTerrainWeightSlots && decoded.slots[used] != 0xFF)
        {
            tile.layers.push_back(decoded.layers[used]);
            ++used;
        }
        const size_t count = tile.SampleCount();
        tile.weights.resize(count * used);
        for (size_t texel = 0; texel < count; ++texel)
            for (size_t s = 0; s < used; ++s)
                tile.weights[s * count + texel] =
                    float(std::to_integer<uint8_t>(decoded.bytes[texel * kTerrainWeightSlots + s])) / 255.0f;
        return tile;
    }
}

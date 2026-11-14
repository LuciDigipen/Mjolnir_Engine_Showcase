/**
 * @file TerrainBaseWeights.cpp
 * @author Jackey Cen
 * @brief Opens the rule weights cached beside an asset's releases.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainBaseWeights.h"

#include "LittleEndian.h"
#include "PackInternal.h"

#include <assets/AssetId.h>
#include <core/fs/FileIO.h>
#include <core/log/Assert.h>

#include <algorithm>
#include <format>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        using Code = TerrainBaseWeightsErrorCode;
        using detail::LoadF64;
        using detail::LoadI32;
        using detail::LoadU32;
        using detail::LoadU64;
        using detail::LeWriter;

        constexpr size_t kTrailerSize = 8;

        namespace index
        {
            constexpr size_t kVersion = 4;
            constexpr size_t kHeaderSize = 8;
            constexpr size_t kContentVersion = 12;
            constexpr size_t kAlignment = 16;
            constexpr size_t kAsset = 24;
            constexpr size_t kKey = 32;
            constexpr size_t kSpacing = 40;
            constexpr size_t kIntervals = 48;
            constexpr size_t kMinX = 56;
            constexpr size_t kMinZ = 64;
            constexpr size_t kMaxX = 72;
            constexpr size_t kMaxZ = 80;
            constexpr size_t kTileCount = 88;
            constexpr size_t kTileRecordSize = 92;
            constexpr size_t kLayerCount = 96;
            constexpr size_t kLayerBytes = 100;
            constexpr size_t kPackSize = 104;
        } // namespace index

        Code MapPackCode(TerrainPackErrorCode code)
        {
            switch (code)
            {
            case TerrainPackErrorCode::None:
                return Code::None;
            case TerrainPackErrorCode::Io:
                return Code::Io;
            case TerrainPackErrorCode::Unsupported:
                return Code::Unsupported;
            case TerrainPackErrorCode::InvalidInput:
                return Code::InvalidInput;
            default:
                return Code::Corrupt;
            }
        }

        std::string Describe(TileCoord tile)
        {
            return std::format("({}, {})", tile.x, tile.z);
        }

        bool IsPowerOfTwo(uint32_t value)
        {
            return value != 0 && (value & (value - 1)) == 0;
        }
    }

    std::filesystem::path TerrainBaseWeightsRoot(const std::filesystem::path &releasesRoot, AssetId asset)
    {
        return TerrainAssetReleasesDir(releasesRoot, asset) / kTerrainBaseWeightsDirName;
    }

    std::filesystem::path TerrainBaseWeightsDir(const std::filesystem::path &releasesRoot, AssetId asset,
                                                TerrainSourceFingerprint key)
    {
        return TerrainBaseWeightsRoot(releasesRoot, asset) / TerrainIdHex(key.value);
    }

    std::filesystem::path TerrainBaseWeightsPointerPath(const std::filesystem::path &releasesRoot, AssetId asset)
    {
        return TerrainBaseWeightsRoot(releasesRoot, asset) / kTerrainCurrentPointerFileName;
    }

    TerrainBaseWeights::TerrainBaseWeights(TerrainSourceFingerprint key, TerrainLayout layout,
                                           std::vector<std::string> layers, std::filesystem::path pack,
                                           std::vector<TerrainWeightRecord> tiles)
        : mKey(key), mLayout(std::move(layout)), mLayers(std::move(layers)), mPack(std::move(pack)),
          mTiles(std::move(tiles))
    {
    }

    const TerrainWeightRecord *TerrainBaseWeights::FindTile(TileCoord tile) const
    {
        auto at = std::lower_bound(mTiles.begin(), mTiles.end(), tile,
                                   [](const TerrainWeightRecord &record, TileCoord key)
                                   {
                                       return detail::LessZX(record.tile.x, record.tile.z, key.x, key.z);
                                   });
        return at != mTiles.end() && at->tile == tile ? &*at : nullptr;
    }

    TerrainBaseWeightsResult<TerrainBaseWeights> TerrainBaseWeights::Open(const std::filesystem::path &releasesRoot,
                                                                          AssetId asset)
    {
        TerrainBaseWeightsResult<TerrainBaseWeights> result;
        if (asset.IsNull())
        {
            result.error = {Code::InvalidInput, "no asset id"};
            return result;
        }

        const std::filesystem::path pointer = TerrainBaseWeightsPointerPath(releasesRoot, asset);
        const std::optional<std::string> text = ReadText(pointer);
        if (!text)
        {
            result.error = {Code::MissingPointer, std::format("{}: no base weights cache (run TerrainTool sync)",
                                                             pointer.generic_string())};
            return result;
        }
        const TerrainResult<TerrainReleaseId> parsed = ParseTerrainCurrentPointer(*text);
        if (!parsed)
        {
            result.error = {Code::Corrupt, std::format("{}: {}", pointer.generic_string(), parsed.error.message)};
            return result;
        }
        const TerrainSourceFingerprint key(parsed.value->value);
        const std::filesystem::path dir = TerrainBaseWeightsDir(releasesRoot, asset, key);
        result = OpenDir(dir, asset);
        if (result && result.value->Key() != key)
        {
            const uint64_t stored = result.value->Key().value;
            result.value.reset();
            result.error = {Code::Corrupt, std::format("{}: the cache keeps key {}, not the pointer's {}",
                                                      dir.generic_string(), TerrainIdHex(stored),
                                                      TerrainIdHex(key.value))};
        }
        return result;
    }

    TerrainBaseWeightsResult<TerrainBaseWeights> TerrainBaseWeights::OpenDir(const std::filesystem::path &dir,
                                                                             AssetId asset)
    {
        TerrainBaseWeightsResult<TerrainBaseWeights> result;
        if (asset.IsNull())
        {
            result.error = {Code::InvalidInput, "no asset id"};
            return result;
        }
        const std::string where = dir.generic_string();
        auto corrupt = [&result](std::string why)
        {
            result.error = {Code::Corrupt, std::move(why)};
            return result;
        };

        const std::filesystem::path indexPath = dir / kTerrainBaseWeightsIndexFileName;
        const std::optional<std::vector<std::byte>> bytes = ReadBytes(indexPath);
        if (!bytes)
        {
            result.error = {Code::Io, std::format("{}: cannot read {}", where, indexPath.filename().generic_string())};
            return result;
        }
        if (bytes->size() < size_t(kTerrainBaseWeightsHeaderSize) + kTrailerSize)
            return corrupt(std::format("{}: {} bytes is too short for a base weights index", where, bytes->size()));
        const std::span<const std::byte> index(*bytes);
        if (!std::equal(kTerrainBaseWeightsMagic.begin(), kTerrainBaseWeightsMagic.end(), index.begin()))
            return corrupt(std::format("{}: bad magic: not a base weights index", where));
        if (LoadU32(index, index::kVersion) != kTerrainBaseWeightsVersion)
            result.error = {Code::Unsupported, std::format("{}: base weights version {} is not supported (this reader "
                                                           "reads {}); run TerrainTool sync again",
                                                           where, LoadU32(index, index::kVersion),
                                                           kTerrainBaseWeightsVersion)};
        else if (LoadU32(index, index::kHeaderSize) != kTerrainBaseWeightsHeaderSize)
            result.error = {Code::Corrupt, std::format("{}: header size {} should be {}", where,
                                                       LoadU32(index, index::kHeaderSize),
                                                       kTerrainBaseWeightsHeaderSize)};
        else if (LoadU32(index, index::kContentVersion) != kTerrainWeightsContentVersion)
            result.error = {Code::Unsupported, std::format("{}: weight content version {} is not supported (this "
                                                           "reader reads {})",
                                                           where, LoadU32(index, index::kContentVersion),
                                                           kTerrainWeightsContentVersion)};
        else if (!IsPowerOfTwo(LoadU32(index, index::kAlignment)) ||
                 LoadU32(index, index::kAlignment) > kMaxTerrainPayloadAlignment)
            result.error = {Code::Unsupported, std::format("{}: payload alignment {} is not supported", where,
                                                           LoadU32(index, index::kAlignment))};
        if (result.error.code != Code::None)
            return result;

        const size_t body = bytes->size() - kTrailerSize;
        const uint64_t checksum = LoadU64(index, body);
        if (checksum != Fnv1a64(index.first(body)))
            return corrupt(std::format("{}: index checksum {:016x} does not match {:016x}", where, checksum,
                                       Fnv1a64(index.first(body))));
        const uint64_t assetId = LoadU64(index, index::kAsset);
        if (assetId != asset.value)
            return corrupt(std::format("{}: the cache is for asset {}, not this asset's {}", where,
                                       TerrainIdHex(assetId), TerrainIdHex(asset.value)));
        const TerrainSourceFingerprint key(LoadU64(index, index::kKey));

        const std::optional<TerrainLayout> layout =
            TerrainLayout::Create(LoadF64(index, index::kSpacing), LoadI32(index, index::kIntervals),
                                  {LoadF64(index, index::kMinX), LoadF64(index, index::kMinZ),
                                   LoadF64(index, index::kMaxX), LoadF64(index, index::kMaxZ)});
        if (!layout)
            return corrupt(std::format("{}: the reference layout does not hold its samples", where));

        const uint32_t tileCount = LoadU32(index, index::kTileCount);
        const uint32_t recordSize = LoadU32(index, index::kTileRecordSize);
        const uint32_t layerCount = LoadU32(index, index::kLayerCount);
        const uint32_t layerBytes = LoadU32(index, index::kLayerBytes);
        const uint64_t packSize = LoadU64(index, index::kPackSize);
        if (tileCount == 0)
            return corrupt(std::format("{}: the base weights cache holds no tiles", where));
        if (recordSize != kTerrainWeightRecordSize)
            return corrupt(std::format("{}: tile record size {} should be {}", where, recordSize,
                                       kTerrainWeightRecordSize));
        if (layerCount == 0 || layerCount > uint32_t(kMaxTerrainMaterialLayers))
            return corrupt(std::format("{}: weight layer count {} is outside 1 to {}", where, layerCount,
                                       kMaxTerrainMaterialLayers));
        const uint64_t expected = uint64_t(kTerrainBaseWeightsHeaderSize) + uint64_t(tileCount) * recordSize +
                                  layerBytes + kTrailerSize;
        if (expected != bytes->size())
            return corrupt(std::format("{}: {} tiles and {} layer bytes need {} bytes, not {}", where, tileCount,
                                       layerBytes, expected, bytes->size()));

        std::vector<std::string> layers;
        layers.reserve(layerCount);
        size_t at = size_t(kTerrainBaseWeightsHeaderSize) + size_t(tileCount) * recordSize;
        const size_t layerEnd = at + layerBytes;
        for (uint32_t l = 0; l < layerCount; ++l)
        {
            if (at + 4 > layerEnd)
                return corrupt(std::format("{}: the layer name table ends before layer {}", where, l));
            const uint32_t length = LoadU32(index, at);
            at += 4;
            if (at + length > layerEnd)
                return corrupt(std::format("{}: layer {} name length {} exceeds the table", where, l, length));
            std::string name(reinterpret_cast<const char *>(index.data() + at), length);
            at += length;
            if (!IsValidTerrainMaterialLayerName(name))
                return corrupt(std::format("{}: layer {}: \"{}\" is not a valid material layer name", where, l, name));
            if (std::find(layers.begin(), layers.end(), name) != layers.end())
                return corrupt(std::format("{}: layer {} repeats \"{}\"", where, l, name));
            layers.push_back(std::move(name));
        }
        if (at != layerEnd)
            return corrupt(std::format("{}: {} layer table bytes are spare after {} names", where, layerEnd - at,
                                       layerCount));

        const size_t side = size_t(layout->IntervalsPerTile()) + 1;
        const uint64_t payloadLength = uint64_t(side) * side * kTerrainWeightSlots;
        std::vector<TerrainWeightRecord> records(tileCount);
        for (uint32_t t = 0; t < tileCount; ++t)
        {
            const size_t recordAt = size_t(kTerrainBaseWeightsHeaderSize) + size_t(t) * recordSize;
            TerrainWeightRecord &record = records[t];
            record.tile = {LoadI32(index, recordAt + terrainformat::weightRecord::kX),
                           LoadI32(index, recordAt + terrainformat::weightRecord::kZ)};
            record.pack = LoadU32(index, recordAt + terrainformat::weightRecord::kPack);
            record.offset = LoadU64(index, recordAt + terrainformat::weightRecord::kOffset);
            record.length = LoadU64(index, recordAt + terrainformat::weightRecord::kLength);
            record.checksum = LoadU64(index, recordAt + terrainformat::weightRecord::kChecksum);
            for (size_t s = 0; s < kTerrainWeightSlots; ++s)
                record.slots[s] =
                    std::to_integer<uint8_t>(index[recordAt + terrainformat::weightRecord::kSlots + s]);

            const std::string tile = Describe(record.tile);
            if (t > 0 && !detail::LessZX(records[t - 1].tile.x, records[t - 1].tile.z, record.tile.x, record.tile.z))
                return corrupt(std::format("{}: tile records {} and {} are not in increasing (z, x) order", where,
                                           t - 1, t));
            if (!layout->Contains(record.tile))
                return corrupt(std::format("{}: tile {} is outside the reference layout", where, tile));
            if (record.length != payloadLength)
                return corrupt(std::format("{}: tile {} payload length {} should be {}", where, tile, record.length,
                                           payloadLength));
            if (record.pack != 0)
                return corrupt(std::format("{}: tile {} names pack {}", where, tile, record.pack));
            size_t used = 0;
            const TerrainPackError slots =
                detail::CheckWeightSlots(record.slots, layers.size(), "weight tile " + tile, used);
            if (slots.code != TerrainPackErrorCode::None)
                return corrupt(std::format("{}: {}", where, slots.message));
        }

        const std::filesystem::path pack = dir / kTerrainBaseWeightsPackFileName;
        const std::optional<std::vector<std::byte>> header = ReadFirstBytes(pack, kTerrainPackHeaderSize);
        if (!header)
        {
            result.error = {Code::Io, std::format("{}: cannot read {}", where, pack.filename().generic_string())};
            return result;
        }
        const std::span<const std::byte> frame(*header);
        if (!std::equal(terrainformat::kPackMagic.begin(), terrainformat::kPackMagic.end(), frame.begin()))
            return corrupt(std::format("{}: bad magic: not a base weights pack", where));
        if (LoadU32(frame, terrainformat::pack::kMajor) != kTerrainFormatVersion.value)
            result.error = {Code::Unsupported, std::format("{}: pack format {} is not supported", where,
                                                           LoadU32(frame, terrainformat::pack::kMajor))};
        else if (LoadU32(frame, terrainformat::pack::kHeaderSize) != kTerrainPackHeaderSize)
            result.error = {Code::Corrupt, std::format("{}: pack header size {} should be {}", where,
                                                       LoadU32(frame, terrainformat::pack::kHeaderSize),
                                                       kTerrainPackHeaderSize)};
        else if (LoadU32(frame, terrainformat::pack::kContentVersion) != kTerrainWeightsContentVersion)
            result.error = {Code::Unsupported, std::format("{}: pack content version {} is not supported", where,
                                                           LoadU32(frame, terrainformat::pack::kContentVersion))};
        else if (LoadU32(frame, terrainformat::pack::kTileCount) != tileCount)
            result.error = {Code::Corrupt, std::format("{}: the pack holds {} tiles, the index {}",
                                                       where, LoadU32(frame, terrainformat::pack::kTileCount),
                                                       tileCount)};
        else if (LoadU64(frame, terrainformat::pack::kAsset) != asset.value ||
                 LoadU64(frame, terrainformat::pack::kRelease) != key.value)
            result.error = {Code::Corrupt, std::format("{}: the pack is another asset's or key's", where)};
        if (result.error.code != Code::None)
            return result;
        const std::optional<uint64_t> actualSize = FileSize(pack);
        if (actualSize != packSize)
            return corrupt(std::format("{}: {} is {}, the index says {} bytes", where, pack.filename().generic_string(),
                                       actualSize ? std::format("{} bytes", *actualSize) : std::string("missing"),
                                       packSize));

        result.value = TerrainBaseWeights(key, *layout, std::move(layers), pack, std::move(records));
        return result;
    }

    TerrainBaseWeightsResult<TerrainWeightTile> TerrainBaseWeights::ReadTile(TileCoord tile) const
    {
        TerrainBaseWeightsResult<TerrainWeightTile> result;
        const TerrainWeightRecord *record = FindTile(tile);
        if (!record)
        {
            result.error = {Code::MissingTile,
                            std::format("tile {} is not in the base weights cache", Describe(tile))};
            return result;
        }
        const std::optional<std::vector<std::byte>> bytes =
            ReadByteRange(mPack, record->offset, static_cast<size_t>(record->length));
        if (!bytes)
        {
            result.error = {Code::Io, std::format("tile {}: cannot read {} bytes from {}", Describe(tile),
                                                  record->length, mPack.generic_string())};
            return result;
        }

        TerrainIndex index{
            .release = {},
            .formatMinor = 0,
            .contentVersion = 0,
            .payloadAlignment = 0,
            .chunkTiles = 0,
            .layout = mLayout,
            .layers = {},
            .heightsHash = 0,
            .packs = {},
            .tiles = {},
            .regionGrid = std::nullopt,
            .regions = {},
            .weightLayers = mLayers,
            .weightsFingerprint = 0,
            .weightPacks = {},
            .weights = {},
            .regionWeights = {},
        };
        const TerrainResult<TerrainDecodedWeights> decoded = DecodeTerrainWeightPayload(index, *record, *bytes);
        if (!decoded)
        {
            result.error = {MapPackCode(decoded.error.code), decoded.error.message};
            return result;
        }

        TerrainWeightTile weights;
        weights.tile = tile;
        weights.intervals = mLayout.IntervalsPerTile();
        weights.layers = mLayers;
        const size_t count = weights.SampleCount();
        weights.weights.assign(count * mLayers.size(), 0.0f);
        size_t used = 0;
        while (used < kTerrainWeightSlots && record->slots[used] != 0xFF)
        {
            const size_t layer = record->slots[used];
            for (size_t texel = 0; texel < count; ++texel)
                weights.weights[layer * count + texel] =
                    float(std::to_integer<uint8_t>((*bytes)[texel * kTerrainWeightSlots + used])) / 255.0f;
            ++used;
        }
        result.value = std::move(weights);
        return result;
    }

    TerrainResult<TerrainEncodedBaseWeights> EncodeTerrainBaseWeights(AssetId asset, TerrainSourceFingerprint key,
                                                                      const TerrainLayout &layout,
                                                                      std::span<const std::string> layers,
                                                                      std::span<const TerrainWeightTile> tiles)
    {
        using Code = TerrainPackErrorCode;
        using R = TerrainEncodedBaseWeights;
        if (asset.IsNull() || key.IsNull())
            return detail::Fail<R>(Code::InvalidInput, "asset and key ids must be set");
        if (layout.IntervalsPerTile() < 1)
            return detail::Fail<R>(Code::InvalidInput, "the reference layout has no intervals per tile");
        if (layers.empty() || layers.size() > size_t(kMaxTerrainMaterialLayers))
            return detail::Fail<R>(Code::InvalidInput, "weight layer table holds {} names, needs 1 to {}",
                                   layers.size(), kMaxTerrainMaterialLayers);
        for (size_t l = 0; l < layers.size(); ++l)
            if (!IsValidTerrainMaterialLayerName(layers[l]))
                return detail::Fail<R>(Code::InvalidInput, "weight layer {}: \"{}\" is not a valid material layer "
                                                           "name",
                                       l, layers[l]);
        for (size_t l = 1; l < layers.size(); ++l)
            if (std::find(layers.begin(), layers.begin() + l, layers[l]) != layers.begin() + l)
                return detail::Fail<R>(Code::InvalidInput, "weight layer {} repeats \"{}\"", l, layers[l]);
        if (tiles.empty())
            return detail::Fail<R>(Code::InvalidInput, "no weight tiles");

        std::vector<const TerrainWeightTile *> ordered;
        ordered.reserve(tiles.size());
        for (const TerrainWeightTile &tile : tiles)
            ordered.push_back(&tile);
        std::sort(ordered.begin(), ordered.end(),
                  [](const TerrainWeightTile *a, const TerrainWeightTile *b)
                  {
                      return detail::LessZX(a->tile.x, a->tile.z, b->tile.x, b->tile.z);
                  });
        for (size_t t = 0; t < ordered.size(); ++t)
        {
            const TerrainWeightTile &tile = *ordered[t];
            if (t > 0 && ordered[t - 1]->tile == tile.tile)
                return detail::Fail<R>(Code::InvalidInput, "weight tile {} is given twice", Describe(tile.tile));
            if (tile.intervals != layout.IntervalsPerTile())
                return detail::Fail<R>(Code::InvalidInput, "weight tile {} has {} intervals, the reference grid {}",
                                       Describe(tile.tile), tile.intervals, layout.IntervalsPerTile());
            if (!layout.Contains(tile.tile))
                return detail::Fail<R>(Code::InvalidInput, "weight tile {} is outside the reference layout",
                                       Describe(tile.tile));
        }

        R encoded;
        LeWriter pack(encoded.pack);
        pack.Bytes(terrainformat::kPackMagic);
        pack.U32(kTerrainFormatVersion.value);
        pack.U32(kTerrainWeightsFormatMinor);
        pack.U32(kTerrainPackHeaderSize);
        pack.U64(asset.value);
        pack.U64(key.value);
        pack.I32(0);
        pack.I32(0);
        pack.U32(static_cast<uint32_t>(ordered.size()));
        pack.U32(kTerrainWeightsContentVersion);

        std::vector<TerrainWeightRecord> records(ordered.size());
        for (size_t t = 0; t < ordered.size(); ++t)
        {
            const TerrainWeightTile &tile = *ordered[t];
            const TerrainResult<TerrainEncodedWeights> payload = EncodeTerrainWeightPayload(tile, layers);
            if (!payload)
                return detail::Failure<R>(payload.error, "");
            pack.PadTo(kDefaultTerrainPayloadAlignment);
            TerrainWeightRecord &record = records[t];
            record.tile = tile.tile;
            record.slots = payload.value->slots;
            record.offset = pack.Size();
            record.length = payload.value->bytes.size();
            record.checksum = Fnv1a64(payload.value->bytes);
            pack.Bytes(payload.value->bytes);
        }

        uint32_t layerBytes = 0;
        for (const std::string &name : layers)
            layerBytes += static_cast<uint32_t>(sizeof(uint32_t) + name.size());

        LeWriter index(encoded.index);
        index.Bytes(kTerrainBaseWeightsMagic);
        index.U32(kTerrainBaseWeightsVersion);
        index.U32(kTerrainBaseWeightsHeaderSize);
        index.U32(kTerrainWeightsContentVersion);
        index.U32(kDefaultTerrainPayloadAlignment);
        index.U32(0);
        index.U64(asset.value);
        index.U64(key.value);
        index.F64(layout.SampleSpacing());
        index.I32(layout.IntervalsPerTile());
        index.U32(0);
        index.F64(layout.Bounds().minX);
        index.F64(layout.Bounds().minZ);
        index.F64(layout.Bounds().maxX);
        index.F64(layout.Bounds().maxZ);
        index.U32(static_cast<uint32_t>(records.size()));
        index.U32(kTerrainWeightRecordSize);
        index.U32(static_cast<uint32_t>(layers.size()));
        index.U32(layerBytes);
        index.U64(encoded.pack.size());
        index.U64(0);
        index.U64(0);
        MIR_ASSERT(index.Size() == kTerrainBaseWeightsHeaderSize, "base weights header layout drifted from the reader");

        for (const TerrainWeightRecord &record : records)
        {
            index.I32(record.tile.x);
            index.I32(record.tile.z);
            index.U32(record.pack);
            index.U32(0);
            index.U64(record.offset);
            index.U64(record.length);
            index.U64(record.checksum);
            uint32_t slots = 0;
            for (size_t s = 0; s < kTerrainWeightSlots; ++s)
                slots |= uint32_t(record.slots[s]) << (8 * s);
            index.U32(slots);
            index.U32(0);
        }
        for (const std::string &name : layers)
        {
            index.U32(static_cast<uint32_t>(name.size()));
            index.Bytes(std::span(reinterpret_cast<const std::byte *>(name.data()), name.size()));
        }
        index.U64(Fnv1a64(std::span<const std::byte>(encoded.index)));
        TerrainResult<R> result;
        result.value = std::move(encoded);
        return result;
    }

    std::string_view DescribeTerrainBaseWeightsError(TerrainBaseWeightsErrorCode code)
    {
        switch (code)
        {
        case TerrainBaseWeightsErrorCode::None:
            return "ok";
        case TerrainBaseWeightsErrorCode::MissingPointer:
            return "no pointer";
        case TerrainBaseWeightsErrorCode::MissingTile:
            return "no such tile";
        case TerrainBaseWeightsErrorCode::Corrupt:
            return "corrupt";
        case TerrainBaseWeightsErrorCode::Unsupported:
            return "unsupported";
        case TerrainBaseWeightsErrorCode::Io:
            return "io";
        case TerrainBaseWeightsErrorCode::InvalidInput:
            return "invalid input";
        }
        return "?";
    }
}

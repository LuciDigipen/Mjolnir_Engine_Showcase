/**
 * @file TerrainEditLayer.cpp
 * @author Jackey Cen
 * @brief Sparse per-tile terrain edits, their compositing order and encodings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TerrainEditLayer.h"

#include "LittleEndian.h"

#include <assets/AssetId.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace mir
{
    using json = nlohmann::json;

    namespace
    {
        using Code = TerrainPackErrorCode;

        constexpr uint64_t kFnvOffset = 14695981039346656037ull;
        constexpr uint64_t kFnvPrime = 1099511628211ull;

        template <typename T>
        TerrainResult<T> Fail(Code code, std::string message)
        {
            TerrainResult<T> result;
            result.error = {code, std::move(message)};
            return result;
        }

        std::string_view BlendName(TerrainEditBlend blend)
        {
            switch (blend)
            {
            case TerrainEditBlend::Additive:
                return "Additive";
            case TerrainEditBlend::AlphaBlend:
                return "AlphaBlend";
            }
            return "Additive";
        }

        std::optional<TerrainEditBlend> ParseBlend(std::string_view text)
        {
            for (const TerrainEditBlend blend : {TerrainEditBlend::Additive, TerrainEditBlend::AlphaBlend})
                if (text == BlendName(blend))
                    return blend;
            return std::nullopt;
        }

        int64_t FloorDiv(int64_t value, int64_t divisor)
        {
            int64_t quotient = value / divisor;
            if (value % divisor != 0 && (value < 0) != (divisor < 0))
                --quotient;
            return quotient;
        }

        bool SameBits(float left, float right)
        {
            return std::bit_cast<uint32_t>(left) == std::bit_cast<uint32_t>(right);
        }

        size_t SampleCount(int32_t intervals)
        {
            const size_t side = static_cast<size_t>(intervals) + 1;
            return side * side;
        }

        bool ValidWeightName(std::string_view name)
        {
            return IsValidTerrainMaterialLayerName(name);
        }

        std::array<TileCoord, 4> HoldingTiles(const TerrainLayout &layout, GlobalSampleIndex sample,
                                              size_t &count)
        {
            const int32_t intervals = layout.IntervalsPerTile();
            std::array<int64_t, 2> tileX{};
            std::array<int64_t, 2> tileZ{};
            size_t countX = 0;
            size_t countZ = 0;
            const int64_t firstX = FloorDiv(sample.x, intervals);
            const int64_t firstZ = FloorDiv(sample.z, intervals);
            tileX[countX++] = firstX;
            tileZ[countZ++] = firstZ;
            if (sample.x - firstX * intervals == 0)
                tileX[countX++] = firstX - 1;
            if (sample.z - firstZ * intervals == 0)
                tileZ[countZ++] = firstZ - 1;

            std::array<TileCoord, 4> tiles{};
            count = 0;
            const TileRange range = layout.Tiles();
            for (size_t ix = 0; ix < countX; ++ix)
                for (size_t iz = 0; iz < countZ; ++iz)
                {
                    if (tileX[ix] < range.min.x || tileX[ix] > range.max.x || tileZ[iz] < range.min.z ||
                        tileZ[iz] > range.max.z)
                        continue;
                    tiles[count++] = {static_cast<int32_t>(tileX[ix]), static_cast<int32_t>(tileZ[iz])};
                }
            return tiles;
        }

        size_t SampleIndex(TileCoord coord, int32_t intervals, GlobalSampleIndex sample)
        {
            const int64_t localX = sample.x - static_cast<int64_t>(coord.x) * intervals;
            const int64_t localZ = sample.z - static_cast<int64_t>(coord.z) * intervals;
            return static_cast<size_t>(localZ) * static_cast<size_t>(intervals + 1) +
                   static_cast<size_t>(localX);
        }

        uint64_t HashBytes(uint64_t hash, std::string_view bytes)
        {
            for (const char c : bytes)
            {
                hash ^= static_cast<unsigned char>(c);
                hash *= kFnvPrime;
            }
            return hash;
        }

        uint64_t HashU32(uint64_t hash, uint32_t value)
        {
            for (int i = 0; i < 4; ++i)
            {
                hash ^= (value >> (8 * i)) & 0xFFu;
                hash *= kFnvPrime;
            }
            return hash;
        }

        uint64_t HashF32(uint64_t hash, float value)
        {
            return HashU32(hash, std::bit_cast<uint32_t>(value));
        }

        TerrainResult<TerrainEditDecodedTile> DecodeHeightOnlyTile(std::span<const std::byte> bytes,
                                                                   uint32_t channelCount, int32_t tileX,
                                                                   int32_t tileZ, uint32_t intervals)
        {
            using namespace terraineditformat;

            if (bytes.size() < kHeaderSize + kChannelSize + kTrailerSize)
                return Fail<TerrainEditDecodedTile>(Code::Malformed, "tile blob is truncated");
            if (channelCount != 1)
                return Fail<TerrainEditDecodedTile>(Code::Unsupported,
                                                    std::format("tile blob channel count {} is not supported",
                                                                channelCount));

            const uint32_t kind = detail::LoadU32(bytes, kHeaderSize + kKindOffset);
            if (kind != kHeightChannel)
                return Fail<TerrainEditDecodedTile>(Code::Unsupported,
                                                    std::format("tile blob channel kind {} is not supported", kind));

            const uint32_t blendValue = detail::LoadU32(bytes, kHeaderSize + kBlendOffset);
            if (blendValue > static_cast<uint32_t>(TerrainEditBlend::AlphaBlend))
                return Fail<TerrainEditDecodedTile>(Code::Malformed,
                                                    std::format("tile blob blend {} is not a blend mode", blendValue));
            const TerrainEditBlend blend = static_cast<TerrainEditBlend>(blendValue);

            const uint32_t alphaFlag = detail::LoadU32(bytes, kHeaderSize + kHasAlphaOffset);
            if (alphaFlag > 1)
                return Fail<TerrainEditDecodedTile>(Code::Malformed, "tile blob has-alpha flag is not zero or one");
            const bool hasAlpha = alphaFlag == 1;
            if (hasAlpha != (blend == TerrainEditBlend::AlphaBlend))
                return Fail<TerrainEditDecodedTile>(Code::Malformed,
                                                    "tile blob alpha flag disagrees with its blend mode");

            const size_t count = SampleCount(static_cast<int32_t>(intervals));
            const size_t payloadBytes = count * sizeof(float) * (hasAlpha ? 2 : 1);
            const size_t expected = kHeaderSize + kChannelSize + payloadBytes + kTrailerSize;
            if (bytes.size() != expected)
                return Fail<TerrainEditDecodedTile>(
                    Code::Malformed,
                    std::format("tile blob size {} does not match {} intervals and alpha {}", bytes.size(), intervals,
                                hasAlpha ? "payloads" : "none"));

            const size_t body = bytes.size() - kTrailerSize;
            if (detail::LoadU64(bytes, body) != Fnv1a64(bytes.first(body)))
                return Fail<TerrainEditDecodedTile>(Code::Corrupt, "tile blob checksum does not match its bytes");

            TerrainEditDecodedTile decoded;
            decoded.blend = blend;
            decoded.tile.tile = {tileX, tileZ};
            decoded.tile.intervals = static_cast<int32_t>(intervals);
            decoded.tile.values.resize(count);
            size_t at = kHeaderSize + kChannelSize;
            for (size_t index = 0; index < count; ++index, at += sizeof(float))
                decoded.tile.values[index] = detail::LoadF32(bytes, at);
            if (hasAlpha)
            {
                decoded.tile.alpha.resize(count);
                for (size_t index = 0; index < count; ++index, at += sizeof(float))
                    decoded.tile.alpha[index] = detail::LoadF32(bytes, at);
            }

            for (size_t index = 0; index < count; ++index)
                if (!std::isfinite(decoded.tile.values[index]))
                    return Fail<TerrainEditDecodedTile>(
                        Code::Corrupt, std::format("tile ({}, {}) sample {} is not finite", tileX, tileZ, index));
            for (size_t index = 0; index < decoded.tile.alpha.size(); ++index)
                if (!(decoded.tile.alpha[index] >= 0.0f && decoded.tile.alpha[index] <= 1.0f))
                    return Fail<TerrainEditDecodedTile>(
                        Code::Corrupt, std::format("tile ({}, {}) alpha {} is outside 0 to 1", tileX, tileZ, index));

            return {std::move(decoded), {}};
        }
    } // namespace

    std::vector<const TerrainEditLayer *> TerrainEditCompositingOrder(std::span<const TerrainEditLayer> layers)
    {
        std::vector<const TerrainEditLayer *> order;
        for (const TerrainEditLayer &layer : layers)
            if (layer.info.enabled)
                order.push_back(&layer);

        std::stable_sort(order.begin(), order.end(), [](const TerrainEditLayer *left, const TerrainEditLayer *right) {
            if (left->info.priority != right->info.priority)
                return left->info.priority < right->info.priority;
            return left->info.sequence < right->info.sequence;
        });
        return order;
    }

    TerrainResult<TileHeights> CompositeTerrainEditLayers(const TileHeights &base,
                                                          std::span<const TerrainEditLayer> layers)
    {
        TileHeights composite = base;
        const int32_t intervals = base.IntervalsPerTile();
        const size_t count = SampleCount(intervals);
        std::span<float> samples = composite.Samples();

        for (const TerrainEditLayer *layer : TerrainEditCompositingOrder(layers))
        {
            const auto found = layer->tiles.find(base.Tile());
            if (found == layer->tiles.end())
                continue;

            const TerrainEditTile &tile = found->second;
            if (tile.values.empty())
                continue;
            if (tile.intervals != intervals)
                return Fail<TileHeights>(Code::InvalidInput,
                                         std::format("layer '{}' tile ({}, {}) has {} intervals, the base has {}",
                                                     layer->info.name, base.Tile().x, base.Tile().z, tile.intervals,
                                                     intervals));
            if (tile.values.size() != count)
                return Fail<TileHeights>(Code::InvalidInput,
                                         std::format("layer '{}' tile ({}, {}) has {} values, expected {}",
                                                     layer->info.name, base.Tile().x, base.Tile().z,
                                                     tile.values.size(), count));

            const bool alphaBlend = layer->info.heightBlend == TerrainEditBlend::AlphaBlend;
            if (alphaBlend && tile.alpha.size() != count)
                return Fail<TileHeights>(Code::InvalidInput,
                                         std::format("layer '{}' tile ({}, {}) has {} alpha values, expected {}",
                                                     layer->info.name, base.Tile().x, base.Tile().z, tile.alpha.size(),
                                                     count));

            for (size_t index = 0; index < count; ++index)
            {
                const float value = tile.values[index];
                // A zero edit leaves the base bits alone, so a negative zero stays negative.
                if (alphaBlend)
                {
                    const float alpha = tile.alpha[index];
                    if (alpha != 0.0f)
                        samples[index] = samples[index] + alpha * (value - samples[index]);
                }
                else if (value != 0.0f)
                {
                    samples[index] = samples[index] + value;
                }

                if (!std::isfinite(samples[index]))
                    return Fail<TileHeights>(Code::InvalidInput,
                                             std::format("layer '{}' tile ({}, {}) sample {} is not finite",
                                                         layer->info.name, base.Tile().x, base.Tile().z, index));
            }
        }
        return {std::move(composite), {}};
    }

    TerrainResult<TerrainEditWeightComposite> CompositeTerrainEditWeights(const TerrainWeightTile &base,
                                                                         std::span<const TerrainEditLayer> layers)
    {
        TerrainEditWeightComposite result;
        result.tile = base;
        const size_t count = base.SampleCount();
        const size_t layerCount = base.layers.size();
        std::set<std::string> skipped;

        for (const TerrainEditLayer *layer : TerrainEditCompositingOrder(layers))
        {
            const auto found = layer->tiles.find(base.tile);
            if (found == layer->tiles.end())
                continue;

            const TerrainEditTile &tile = found->second;
            if (tile.intervals != base.intervals)
                return Fail<TerrainEditWeightComposite>(
                    Code::InvalidInput, std::format("layer '{}' tile ({}, {}) has {} intervals, the base has {}",
                                                    layer->info.name, base.tile.x, base.tile.z, tile.intervals,
                                                    base.intervals));

            for (const TerrainEditWeight &channel : tile.weights)
            {
                const auto target = std::find(base.layers.begin(), base.layers.end(), channel.layer);
                if (target == base.layers.end())
                {
                    skipped.insert(channel.layer);
                    continue;
                }
                if (channel.values.size() != count)
                    return Fail<TerrainEditWeightComposite>(
                        Code::InvalidInput,
                        std::format("layer '{}' tile ({}, {}) channel '{}' has {} values, expected {}",
                                    layer->info.name, base.tile.x, base.tile.z, channel.layer, channel.values.size(),
                                    count));

                const size_t targetLayer = static_cast<size_t>(target - base.layers.begin());
                for (size_t sample = 0; sample < count; ++sample)
                {
                    const float paint = channel.values[sample];
                    if (paint == 0.0f)
                        continue;
                    if (!(paint >= 0.0f && paint <= 1.0f))
                        return Fail<TerrainEditWeightComposite>(
                            Code::InvalidInput,
                            std::format("layer '{}' tile ({}, {}) channel '{}' sample {} is outside 0 to 1",
                                        layer->info.name, base.tile.x, base.tile.z, channel.layer, sample));
                    for (size_t targetIndex = 0; targetIndex < layerCount; ++targetIndex)
                    {
                        float &weight = result.tile.weights[targetIndex * count + sample];
                        const float onehot = targetIndex == targetLayer ? 1.0f : 0.0f;
                        weight = (1.0f - paint) * weight + paint * onehot;
                    }
                }
            }
        }

        result.skippedLayers.assign(skipped.begin(), skipped.end());
        const TerrainWeightStatus status = ValidateTerrainWeightTile(result.tile);
        if (status != TerrainWeightStatus::Ok)
            return Fail<TerrainEditWeightComposite>(
                Code::InvalidInput, std::format("weight composite is invalid: {}", DescribeTerrainWeightStatus(status)));
        return {std::move(result), {}};
    }

    void SetTerrainEditSample(TerrainEditLayer &layer, const TerrainLayout &layout, GlobalSampleIndex sample,
                              float value, float alpha)
    {
        const int32_t intervals = layout.IntervalsPerTile();
        const size_t count = SampleCount(intervals);
        const bool alphaBlend = layer.info.heightBlend == TerrainEditBlend::AlphaBlend;

        size_t heldCount = 0;
        const std::array<TileCoord, 4> held = HoldingTiles(layout, sample, heldCount);
        for (size_t i = 0; i < heldCount; ++i)
        {
            const TileCoord coord = held[i];
            TerrainEditTile &tile = layer.tiles[coord];
            tile.tile = coord;
            tile.intervals = intervals;
            tile.values.resize(count, 0.0f);
            if (alphaBlend)
                tile.alpha.resize(count, 0.0f);

            const size_t index = SampleIndex(coord, intervals, sample);
            tile.values[index] = value;
            if (alphaBlend)
                tile.alpha[index] = alpha;
        }
    }

    void SetTerrainEditWeightSample(TerrainEditLayer &layer, const TerrainLayout &layout, GlobalSampleIndex sample,
                                    std::string_view materialLayer, float value)
    {
        if (!ValidWeightName(materialLayer))
            return;

        const int32_t intervals = layout.IntervalsPerTile();
        const size_t count = SampleCount(intervals);

        size_t heldCount = 0;
        const std::array<TileCoord, 4> held = HoldingTiles(layout, sample, heldCount);
        for (size_t i = 0; i < heldCount; ++i)
        {
            const TileCoord coord = held[i];
            TerrainEditTile &tile = layer.tiles[coord];
            tile.tile = coord;
            tile.intervals = intervals;

            const auto lower = std::lower_bound(tile.weights.begin(), tile.weights.end(), materialLayer,
                                                [](const TerrainEditWeight &channel, std::string_view name)
                                                {
                                                    return channel.layer < name;
                                                });
            const auto channel = lower != tile.weights.end() && lower->layer == materialLayer
                                     ? lower
                                     : tile.weights.insert(lower, TerrainEditWeight{std::string(materialLayer), {}});
            channel->values.resize(count, 0.0f);
            channel->values[SampleIndex(coord, intervals, sample)] = value;
        }
    }

    std::vector<GlobalSampleIndex> ValidateTerrainEditSharedSamples(const TerrainEditLayer &layer,
                                                                    const TerrainLayout &layout)
    {
        const TileRange range = layout.Tiles();
        const int32_t intervals = layout.IntervalsPerTile();
        const size_t side = static_cast<size_t>(intervals) + 1;
        const size_t count = side * side;
        const bool alphaBlend = layer.info.heightBlend == TerrainEditBlend::AlphaBlend;

        const auto copyAt = [&](TileCoord coord, int64_t x, int64_t z, bool alpha, float &out) {
            const auto found = layer.tiles.find(coord);
            if (found == layer.tiles.end())
            {
                out = 0.0f;
                return true;
            }
            const TerrainEditTile &tile = found->second;
            if (tile.intervals != intervals)
                return false;

            out = 0.0f;
            if (tile.values.empty())
                return true;
            if (alpha ? tile.alpha.size() != count : tile.values.size() != count)
                return false;

            const int64_t localX = x - static_cast<int64_t>(coord.x) * intervals;
            const int64_t localZ = z - static_cast<int64_t>(coord.z) * intervals;
            const size_t index = static_cast<size_t>(localZ) * side + static_cast<size_t>(localX);
            out = alpha ? tile.alpha[index] : tile.values[index];
            return true;
        };

        const auto weightAt = [&](TileCoord coord, std::string_view name, int64_t x, int64_t z, float &out) {
            out = 0.0f;
            const auto found = layer.tiles.find(coord);
            if (found == layer.tiles.end())
                return true;
            const TerrainEditTile &tile = found->second;
            if (tile.intervals != intervals)
                return false;
            for (const TerrainEditWeight &channel : tile.weights)
            {
                if (channel.layer != name)
                    continue;
                if (channel.values.size() != count)
                    return false;
                out = channel.values[SampleIndex(coord, intervals, {x, z})];
                return true;
            }
            return true;
        };

        std::set<std::pair<int64_t, int64_t>> mismatches;
        const auto compare = [&](TileCoord left, TileCoord right, int64_t x, int64_t z) {
            bool same = true;
            float leftValue = 0.0f;
            float rightValue = 0.0f;
            if (copyAt(left, x, z, false, leftValue) && copyAt(right, x, z, false, rightValue) &&
                !SameBits(leftValue, rightValue))
                same = false;
            if (same && alphaBlend && copyAt(left, x, z, true, leftValue) && copyAt(right, x, z, true, rightValue) &&
                !SameBits(leftValue, rightValue))
                same = false;

            if (same)
            {
                std::set<std::string> names;
                for (const TileCoord coord : {left, right})
                {
                    const auto found = layer.tiles.find(coord);
                    if (found == layer.tiles.end())
                        continue;
                    for (const TerrainEditWeight &channel : found->second.weights)
                        names.insert(channel.layer);
                }
                for (const std::string &name : names)
                    if (weightAt(left, name, x, z, leftValue) && weightAt(right, name, x, z, rightValue) &&
                        !SameBits(leftValue, rightValue))
                    {
                        same = false;
                        break;
                    }
            }

            if (!same)
                mismatches.emplace(x, z);
        };

        const auto compareVertical = [&](TileCoord here, TileCoord neighbour, int64_t x) {
            for (int32_t k = 0; k <= intervals; ++k)
                compare(here, neighbour, x, static_cast<int64_t>(here.z) * intervals + k);
        };
        const auto compareHorizontal = [&](TileCoord here, TileCoord neighbour, int64_t z) {
            for (int32_t i = 0; i <= intervals; ++i)
                compare(here, neighbour, static_cast<int64_t>(here.x) * intervals + i, z);
        };

        for (const auto &entry : layer.tiles)
        {
            const TileCoord here = entry.first;
            if (!layout.Contains(here))
                continue;
            if (here.x < range.max.x)
                compareVertical(here, {here.x + 1, here.z}, static_cast<int64_t>(here.x + 1) * intervals);
            if (here.x > range.min.x)
                compareVertical(here, {here.x - 1, here.z}, static_cast<int64_t>(here.x) * intervals);
            if (here.z < range.max.z)
                compareHorizontal(here, {here.x, here.z + 1}, static_cast<int64_t>(here.z + 1) * intervals);
            if (here.z > range.min.z)
                compareHorizontal(here, {here.x, here.z - 1}, static_cast<int64_t>(here.z) * intervals);
        }

        std::vector<GlobalSampleIndex> result;
        result.reserve(mismatches.size());
        for (const auto &[x, z] : mismatches)
            result.push_back({x, z});
        return result;
    }

    TerrainResult<std::vector<std::byte>> EncodeTerrainEditTile(const TerrainEditTile &tile, TerrainEditBlend blend)
    {
        using namespace terraineditformat;

        if (tile.intervals < 1 || tile.intervals > kMaxTerrainTileIntervals)
            return Fail<std::vector<std::byte>>(
                Code::InvalidInput, std::format("tile ({}, {}) intervals {} is outside 1 to {}", tile.tile.x, tile.tile.z,
                                                tile.intervals, kMaxTerrainTileIntervals));

        const size_t count = SampleCount(tile.intervals);
        const bool hasHeight = !tile.values.empty();
        const bool hasAlpha = blend == TerrainEditBlend::AlphaBlend;

        if (tile.weights.empty())
        {
            if (tile.values.size() != count)
                return Fail<std::vector<std::byte>>(
                    Code::InvalidInput, std::format("tile ({}, {}) has {} values, expected {}", tile.tile.x, tile.tile.z,
                                                    tile.values.size(), count));
            if (hasAlpha && tile.alpha.size() != count)
                return Fail<std::vector<std::byte>>(
                    Code::InvalidInput,
                    std::format("tile ({}, {}) has {} alpha values, expected {}", tile.tile.x, tile.tile.z,
                                tile.alpha.size(), count));

            for (size_t index = 0; index < count; ++index)
            {
                if (!std::isfinite(tile.values[index]))
                    return Fail<std::vector<std::byte>>(
                        Code::InvalidInput, std::format("tile ({}, {}) sample {} is not finite", tile.tile.x, tile.tile.z,
                                                        index));
                if (hasAlpha && !(tile.alpha[index] >= 0.0f && tile.alpha[index] <= 1.0f))
                    return Fail<std::vector<std::byte>>(
                        Code::InvalidInput,
                        std::format("tile ({}, {}) alpha {} is outside 0 to 1", tile.tile.x, tile.tile.z, index));
            }

            std::vector<std::byte> bytes;
            detail::LeWriter out(bytes);
            out.U32(kMagic);
            out.U32(kTerrainEditFormatVersion);
            out.I32(tile.tile.x);
            out.I32(tile.tile.z);
            out.U32(static_cast<uint32_t>(tile.intervals));
            out.U32(1);
            out.U32(kHeightChannel);
            out.U32(static_cast<uint32_t>(blend));
            out.U32(hasAlpha ? 1u : 0u);
            for (const float value : tile.values)
                out.F32(value);
            if (hasAlpha)
                for (const float alpha : tile.alpha)
                    out.F32(alpha);
            out.U64(Fnv1a64(std::span<const std::byte>(bytes)));
            return {std::move(bytes), {}};
        }

        if (!hasHeight && !tile.alpha.empty())
            return Fail<std::vector<std::byte>>(
                Code::InvalidInput, std::format("tile ({}, {}) has alpha values but no height values", tile.tile.x,
                                                tile.tile.z));

        if (hasHeight)
        {
            if (tile.values.size() != count)
                return Fail<std::vector<std::byte>>(
                    Code::InvalidInput, std::format("tile ({}, {}) has {} values, expected {}", tile.tile.x, tile.tile.z,
                                                    tile.values.size(), count));
            if (hasAlpha && tile.alpha.size() != count)
                return Fail<std::vector<std::byte>>(
                    Code::InvalidInput,
                    std::format("tile ({}, {}) has {} alpha values, expected {}", tile.tile.x, tile.tile.z,
                                tile.alpha.size(), count));

            for (size_t index = 0; index < count; ++index)
            {
                if (!std::isfinite(tile.values[index]))
                    return Fail<std::vector<std::byte>>(
                        Code::InvalidInput, std::format("tile ({}, {}) sample {} is not finite", tile.tile.x, tile.tile.z,
                                                        index));
                if (hasAlpha && !(tile.alpha[index] >= 0.0f && tile.alpha[index] <= 1.0f))
                    return Fail<std::vector<std::byte>>(
                        Code::InvalidInput,
                        std::format("tile ({}, {}) alpha {} is outside 0 to 1", tile.tile.x, tile.tile.z, index));
            }
        }

        std::string_view previous;
        for (const TerrainEditWeight &channel : tile.weights)
        {
            if (!ValidWeightName(channel.layer))
                return Fail<std::vector<std::byte>>(
                    Code::InvalidInput, std::format("tile ({}, {}) weight channel name \"{}\" is not a material layer name",
                                                    tile.tile.x, tile.tile.z, channel.layer));
            if (!previous.empty() && channel.layer <= previous)
                return Fail<std::vector<std::byte>>(
                    Code::InvalidInput, std::format("tile ({}, {}) weight channels are not in name order at \"{}\"",
                                                    tile.tile.x, tile.tile.z, channel.layer));
            previous = channel.layer;
            if (channel.values.size() != count)
                return Fail<std::vector<std::byte>>(
                    Code::InvalidInput,
                    std::format("tile ({}, {}) weight channel \"{}\" has {} values, expected {}", tile.tile.x, tile.tile.z,
                                channel.layer, channel.values.size(), count));
            for (size_t index = 0; index < count; ++index)
                if (!(channel.values[index] >= 0.0f && channel.values[index] <= 1.0f))
                    return Fail<std::vector<std::byte>>(
                        Code::InvalidInput,
                        std::format("tile ({}, {}) weight channel \"{}\" value {} is outside 0 to 1", tile.tile.x,
                                    tile.tile.z, channel.layer, index));
        }

        std::vector<uint32_t> nameOffsets;
        nameOffsets.reserve(tile.weights.size());
        uint32_t nameOffset = 0;
        for (const TerrainEditWeight &channel : tile.weights)
        {
            nameOffsets.push_back(nameOffset);
            nameOffset += static_cast<uint32_t>(kNameLengthSize + channel.layer.size());
        }

        const uint32_t channelCount = (hasHeight ? 1u : 0u) + static_cast<uint32_t>(tile.weights.size());
        std::vector<std::byte> bytes;
        detail::LeWriter out(bytes);
        out.U32(kMagic);
        out.U32(kTerrainEditWeightFormatVersion);
        out.I32(tile.tile.x);
        out.I32(tile.tile.z);
        out.U32(static_cast<uint32_t>(tile.intervals));
        out.U32(channelCount);

        if (hasHeight)
        {
            out.U32(kHeightChannel);
            out.U32(static_cast<uint32_t>(blend));
            out.U32(hasAlpha ? 1u : 0u);
            out.U32(0);
        }
        for (size_t channel = 0; channel < tile.weights.size(); ++channel)
        {
            out.U32(kWeightChannel);
            out.U32(0);
            out.U32(0);
            out.U32(nameOffsets[channel]);
        }

        if (hasHeight)
        {
            for (const float value : tile.values)
                out.F32(value);
            if (hasAlpha)
                for (const float alpha : tile.alpha)
                    out.F32(alpha);
        }
        for (const TerrainEditWeight &channel : tile.weights)
            for (const float value : channel.values)
                out.F32(value);

        for (const TerrainEditWeight &channel : tile.weights)
        {
            out.U32(static_cast<uint32_t>(channel.layer.size()));
            out.Bytes(std::span<const std::byte>(reinterpret_cast<const std::byte *>(channel.layer.data()),
                                                 channel.layer.size()));
        }

        out.U64(Fnv1a64(std::span<const std::byte>(bytes)));
        return {std::move(bytes), {}};
    }

    TerrainResult<TerrainEditDecodedTile> DecodeTerrainEditTile(std::span<const std::byte> bytes)
    {
        using namespace terraineditformat;

        if (bytes.size() < kHeaderSize + kTrailerSize)
            return Fail<TerrainEditDecodedTile>(Code::Malformed, "tile blob is truncated");
        if (detail::LoadU32(bytes, kMagicOffset) != kMagic)
            return Fail<TerrainEditDecodedTile>(Code::Malformed, "tile blob magic is not an edit tile");
        const uint32_t version = detail::LoadU32(bytes, kVersionOffset);
        if (version != kTerrainEditFormatVersion && version != kTerrainEditWeightFormatVersion)
            return Fail<TerrainEditDecodedTile>(Code::Unsupported,
                                                std::format("tile blob version {} is not supported", version));

        const int32_t tileX = detail::LoadI32(bytes, kTileXOffset);
        const int32_t tileZ = detail::LoadI32(bytes, kTileZOffset);
        const uint32_t intervals = detail::LoadU32(bytes, kIntervalsOffset);
        if (intervals < 1 || intervals > static_cast<uint32_t>(kMaxTerrainTileIntervals))
            return Fail<TerrainEditDecodedTile>(
                Code::Malformed, std::format("tile ({}, {}) intervals {} is outside 1 to {}", tileX, tileZ, intervals,
                                             kMaxTerrainTileIntervals));

        const uint32_t channelCount = detail::LoadU32(bytes, kChannelCountOffset);
        if (channelCount == 0)
            return Fail<TerrainEditDecodedTile>(Code::Malformed, "tile blob has no channels");
        if (version == kTerrainEditFormatVersion)
            return DecodeHeightOnlyTile(bytes, channelCount, tileX, tileZ, intervals);

        if (channelCount > 1u + static_cast<uint32_t>(kMaxTerrainMaterialLayers))
            return Fail<TerrainEditDecodedTile>(
                Code::Malformed, std::format("tile blob channel count {} exceeds {}", channelCount,
                                             1u + static_cast<uint32_t>(kMaxTerrainMaterialLayers)));

        const size_t channelTableBytes = static_cast<size_t>(channelCount) * kWeightChannelSize;
        if (bytes.size() < kHeaderSize + channelTableBytes + kTrailerSize)
            return Fail<TerrainEditDecodedTile>(Code::Malformed, "tile blob channel table is truncated");

        struct ChannelEntry
        {
            uint32_t kind = 0;
            uint32_t blend = 0;
            uint32_t hasAlpha = 0;
            uint32_t nameOffset = 0;
        };
        std::vector<ChannelEntry> entries(channelCount);
        TerrainEditBlend blend = TerrainEditBlend::Additive;
        bool hasHeight = false;
        bool hasAlpha = false;
        size_t weightCount = 0;
        for (size_t channel = 0; channel < channelCount; ++channel)
        {
            const size_t at = kHeaderSize + channel * kWeightChannelSize;
            ChannelEntry &entry = entries[channel];
            entry.kind = detail::LoadU32(bytes, at + kKindOffset);
            entry.blend = detail::LoadU32(bytes, at + kBlendOffset);
            entry.hasAlpha = detail::LoadU32(bytes, at + kHasAlphaOffset);
            entry.nameOffset = detail::LoadU32(bytes, at + kNameOffsetOffset);

            if (entry.kind == kHeightChannel)
            {
                if (hasHeight)
                    return Fail<TerrainEditDecodedTile>(Code::Malformed, "tile blob has more than one height channel");
                if (channel != 0)
                    return Fail<TerrainEditDecodedTile>(Code::Malformed, "tile blob height channel is not first");
                if (entry.blend > static_cast<uint32_t>(TerrainEditBlend::AlphaBlend))
                    return Fail<TerrainEditDecodedTile>(
                        Code::Malformed, std::format("tile blob blend {} is not a blend mode", entry.blend));
                if (entry.hasAlpha > 1)
                    return Fail<TerrainEditDecodedTile>(Code::Malformed,
                                                        "tile blob has-alpha flag is not zero or one");
                hasHeight = true;
                blend = static_cast<TerrainEditBlend>(entry.blend);
                hasAlpha = entry.hasAlpha == 1;
                if (hasAlpha != (blend == TerrainEditBlend::AlphaBlend))
                    return Fail<TerrainEditDecodedTile>(Code::Malformed,
                                                        "tile blob alpha flag disagrees with its blend mode");
            }
            else if (entry.kind == kWeightChannel)
            {
                if (entry.blend != 0 || entry.hasAlpha != 0)
                    return Fail<TerrainEditDecodedTile>(
                        Code::Malformed, "tile blob weight channel has a blend mode or alpha flag");
                ++weightCount;
            }
            else
                return Fail<TerrainEditDecodedTile>(
                    Code::Unsupported, std::format("tile blob channel kind {} is not supported", entry.kind));
        }

        const size_t count = SampleCount(static_cast<int32_t>(intervals));
        const size_t payload = kHeaderSize + channelTableBytes;
        const size_t heightBytes = hasHeight ? count * sizeof(float) * (hasAlpha ? 2 : 1) : 0;
        const size_t weightBytes = weightCount * count * sizeof(float);
        const size_t tableStart = payload + heightBytes + weightBytes;
        const size_t body = bytes.size() - kTrailerSize;
        if (tableStart > body)
            return Fail<TerrainEditDecodedTile>(
                Code::Malformed,
                std::format("tile blob is too short for {} intervals and {} channels", intervals, channelCount));
        if (detail::LoadU64(bytes, body) != Fnv1a64(bytes.first(body)))
            return Fail<TerrainEditDecodedTile>(Code::Corrupt, "tile blob checksum does not match its bytes");

        std::vector<std::string> names;
        names.reserve(weightCount);
        size_t cursor = 0;
        for (const ChannelEntry &entry : entries)
        {
            if (entry.kind != kWeightChannel)
                continue;
            if (entry.nameOffset != cursor)
                return Fail<TerrainEditDecodedTile>(Code::Malformed,
                                                    "tile blob weight channel name is not length-prefixed");
            if (tableStart + cursor + kNameLengthSize > body)
                return Fail<TerrainEditDecodedTile>(Code::Malformed, "tile blob name table is truncated");
            const uint32_t length = detail::LoadU32(bytes, tableStart + cursor);
            cursor += kNameLengthSize;
            if (tableStart + cursor + length > body)
                return Fail<TerrainEditDecodedTile>(Code::Malformed, "tile blob name table is truncated");

            std::string name(length, '\0');
            for (size_t index = 0; index < length; ++index)
                name[index] = static_cast<char>(std::to_integer<unsigned char>(bytes[tableStart + cursor + index]));
            cursor += length;

            if (!ValidWeightName(name))
                return Fail<TerrainEditDecodedTile>(
                    Code::Malformed,
                    std::format("tile blob weight channel name \"{}\" is not a material layer name", name));
            if (!names.empty())
            {
                if (name == names.back())
                    return Fail<TerrainEditDecodedTile>(
                        Code::Malformed, std::format("tile blob weight channel name \"{}\" is duplicated", name));
                if (name < names.back())
                    return Fail<TerrainEditDecodedTile>(Code::Malformed,
                                                        "tile blob weight channels are not in name order");
            }
            names.push_back(std::move(name));
        }
        if (tableStart + cursor != body)
            return Fail<TerrainEditDecodedTile>(Code::Malformed, "tile blob name table has trailing bytes");

        TerrainEditDecodedTile decoded;
        decoded.blend = blend;
        decoded.tile.tile = {tileX, tileZ};
        decoded.tile.intervals = static_cast<int32_t>(intervals);

        size_t at = payload;
        if (hasHeight)
        {
            decoded.tile.values.resize(count);
            for (size_t index = 0; index < count; ++index, at += sizeof(float))
                decoded.tile.values[index] = detail::LoadF32(bytes, at);
            if (hasAlpha)
            {
                decoded.tile.alpha.resize(count);
                for (size_t index = 0; index < count; ++index, at += sizeof(float))
                    decoded.tile.alpha[index] = detail::LoadF32(bytes, at);
            }
        }
        decoded.tile.weights.reserve(weightCount);
        for (const std::string &name : names)
        {
            TerrainEditWeight channel;
            channel.layer = name;
            channel.values.resize(count);
            for (size_t index = 0; index < count; ++index, at += sizeof(float))
                channel.values[index] = detail::LoadF32(bytes, at);
            decoded.tile.weights.push_back(std::move(channel));
        }

        for (size_t index = 0; hasHeight && index < count; ++index)
            if (!std::isfinite(decoded.tile.values[index]))
                return Fail<TerrainEditDecodedTile>(
                    Code::Corrupt, std::format("tile ({}, {}) sample {} is not finite", tileX, tileZ, index));
        for (size_t index = 0; index < decoded.tile.alpha.size(); ++index)
            if (!(decoded.tile.alpha[index] >= 0.0f && decoded.tile.alpha[index] <= 1.0f))
                return Fail<TerrainEditDecodedTile>(
                    Code::Corrupt, std::format("tile ({}, {}) alpha {} is outside 0 to 1", tileX, tileZ, index));
        for (const TerrainEditWeight &channel : decoded.tile.weights)
            for (size_t index = 0; index < count; ++index)
                if (!(channel.values[index] >= 0.0f && channel.values[index] <= 1.0f))
                    return Fail<TerrainEditDecodedTile>(
                        Code::Corrupt, std::format("tile ({}, {}) weight channel \"{}\" value {} is outside 0 to 1",
                                                   tileX, tileZ, channel.layer, index));

        return {std::move(decoded), {}};
    }

    std::string EncodeTerrainEditManifest(std::span<const TerrainEditLayerInfo> layers)
    {
        json list = json::array();
        for (const TerrainEditLayerInfo &layer : layers)
        {
            json entry = json::object();
            entry["name"] = layer.name;
            entry["priority"] = layer.priority;
            entry["sequence"] = layer.sequence;
            entry["enabled"] = layer.enabled;
            entry["heightBlend"] = BlendName(layer.heightBlend);
            list.push_back(std::move(entry));
        }

        json root = json::object();
        root["version"] = kTerrainEditManifestVersion;
        root["layers"] = std::move(list);
        return root.dump(2, ' ', false, json::error_handler_t::replace);
    }

    TerrainResult<std::vector<TerrainEditLayerInfo>> DecodeTerrainEditManifest(std::string_view text)
    {
        using Result = std::vector<TerrainEditLayerInfo>;

        std::map<int, std::set<std::string>> keys;
        std::string duplicate;
        const json parsed = json::parse(
            text.begin(), text.end(),
            [&](int depth, json::parse_event_t event, json &value) {
                if (event == json::parse_event_t::object_start)
                    keys.erase(depth + 1);
                else if (event == json::parse_event_t::key && !keys[depth].insert(value.get<std::string>()).second &&
                         duplicate.empty())
                    duplicate = value.get<std::string>();
                return true;
            },
            false);
        if (parsed.is_discarded())
            return Fail<Result>(Code::Malformed, "manifest is not valid JSON");
        if (!duplicate.empty())
            return Fail<Result>(Code::Malformed, std::format("{}: is duplicated", duplicate));
        if (!parsed.is_object())
            return Fail<Result>(Code::Malformed, "manifest is not a JSON object");

        for (auto it = parsed.begin(); it != parsed.end(); ++it)
            if (it.key() != "version" && it.key() != "layers")
                return Fail<Result>(Code::Malformed, std::format("{}: is not a known field", it.key()));

        const auto version = parsed.find("version");
        if (version == parsed.end())
            return Fail<Result>(Code::Malformed, "version: is missing");
        if (!version->is_number_integer() || version->get<int64_t>() < 0)
            return Fail<Result>(Code::Malformed, "version: must be a whole number");
        if (version->get<uint64_t>() != kTerrainEditManifestVersion)
            return Fail<Result>(Code::Unsupported,
                                std::format("version: {} is not supported", version->get<uint64_t>()));

        const auto layers = parsed.find("layers");
        if (layers == parsed.end())
            return Fail<Result>(Code::Malformed, "layers: is missing");
        if (!layers->is_array())
            return Fail<Result>(Code::Malformed, "layers: must be a list");

        Result result;
        std::set<std::string> names;
        for (size_t index = 0; index < layers->size(); ++index)
        {
            const json &entry = (*layers)[index];
            const std::string path = std::format("layers[{}]", index);
            if (!entry.is_object())
                return Fail<Result>(Code::Malformed, std::format("{}: must be an object", path));

            for (auto it = entry.begin(); it != entry.end(); ++it)
                if (it.key() != "name" && it.key() != "priority" && it.key() != "sequence" && it.key() != "enabled" &&
                    it.key() != "heightBlend")
                    return Fail<Result>(Code::Malformed,
                                        std::format("{}.{}: is not a known field", path, it.key()));

            TerrainEditLayerInfo info;

            const auto name = entry.find("name");
            if (name == entry.end())
                return Fail<Result>(Code::Malformed, std::format("{}.name: is missing", path));
            if (!name->is_string())
                return Fail<Result>(Code::Malformed, std::format("{}.name: must be a string", path));
            info.name = name->get<std::string>();
            if (!names.insert(info.name).second)
                return Fail<Result>(Code::Malformed, std::format("{}.name: duplicates '{}'", path, info.name));

            const auto priority = entry.find("priority");
            if (priority == entry.end())
                return Fail<Result>(Code::Malformed, std::format("{}.priority: is missing", path));
            if (!priority->is_number_integer() || priority->get<int64_t>() < std::numeric_limits<int32_t>::min() ||
                priority->get<int64_t>() > std::numeric_limits<int32_t>::max())
                return Fail<Result>(Code::Malformed,
                                    std::format("{}.priority: must be a whole number in range", path));
            info.priority = static_cast<int32_t>(priority->get<int64_t>());

            const auto sequence = entry.find("sequence");
            if (sequence == entry.end())
                return Fail<Result>(Code::Malformed, std::format("{}.sequence: is missing", path));
            if (!sequence->is_number_integer() || sequence->get<int64_t>() < 0 ||
                sequence->get<uint64_t>() > std::numeric_limits<uint32_t>::max())
                return Fail<Result>(Code::Malformed,
                                    std::format("{}.sequence: must be a whole number in range", path));
            info.sequence = static_cast<uint32_t>(sequence->get<uint64_t>());

            const auto enabled = entry.find("enabled");
            if (enabled == entry.end())
                return Fail<Result>(Code::Malformed, std::format("{}.enabled: is missing", path));
            if (!enabled->is_boolean())
                return Fail<Result>(Code::Malformed, std::format("{}.enabled: must be true or false", path));
            info.enabled = enabled->get<bool>();

            const auto blend = entry.find("heightBlend");
            if (blend == entry.end())
                return Fail<Result>(Code::Malformed, std::format("{}.heightBlend: is missing", path));
            if (!blend->is_string())
                return Fail<Result>(Code::Malformed, std::format("{}.heightBlend: must be a string", path));
            const std::optional<TerrainEditBlend> parsedBlend = ParseBlend(blend->get<std::string>());
            if (!parsedBlend)
                return Fail<Result>(Code::Malformed,
                                    std::format("{}.heightBlend: is not a blend mode", path));
            info.heightBlend = *parsedBlend;

            result.push_back(std::move(info));
        }
        return {std::move(result), {}};
    }

    uint64_t TerrainEditStackHash(std::span<const TerrainEditLayer> layers)
    {
        uint64_t hash = kFnvOffset;
        bool anyTile = false;
        for (const TerrainEditLayer *layer : TerrainEditCompositingOrder(layers))
        {
            hash = HashBytes(hash, layer->info.name);
            hash = HashU32(hash, static_cast<uint32_t>(layer->info.priority));
            hash = HashU32(hash, static_cast<uint32_t>(layer->info.heightBlend));

            std::vector<const TerrainEditTile *> tiles;
            tiles.reserve(layer->tiles.size());
            for (const auto &[coord, tile] : layer->tiles)
                tiles.push_back(&tile);
            std::sort(tiles.begin(), tiles.end(), [](const TerrainEditTile *left, const TerrainEditTile *right) {
                if (left->tile.z != right->tile.z)
                    return left->tile.z < right->tile.z;
                return left->tile.x < right->tile.x;
            });

            for (const TerrainEditTile *tile : tiles)
            {
                if (tile->values.empty())
                    continue;
                anyTile = true;
                hash = HashU32(hash, static_cast<uint32_t>(tile->tile.x));
                hash = HashU32(hash, static_cast<uint32_t>(tile->tile.z));
                hash = HashU32(hash, static_cast<uint32_t>(tile->intervals));
                for (const float value : tile->values)
                    hash = HashF32(hash, value);
                for (const float alpha : tile->alpha)
                    hash = HashF32(hash, alpha);
            }
        }
        return anyTile ? hash : 0;
    }

    uint64_t TerrainEditWeightStackHash(std::span<const TerrainEditLayer> layers)
    {
        uint64_t hash = kFnvOffset;
        bool anyWeight = false;
        for (const TerrainEditLayer *layer : TerrainEditCompositingOrder(layers))
        {
            hash = HashBytes(hash, layer->info.name);
            hash = HashU32(hash, static_cast<uint32_t>(layer->info.priority));

            std::vector<const TerrainEditTile *> tiles;
            tiles.reserve(layer->tiles.size());
            for (const auto &[coord, tile] : layer->tiles)
                tiles.push_back(&tile);
            std::sort(tiles.begin(), tiles.end(), [](const TerrainEditTile *left, const TerrainEditTile *right) {
                if (left->tile.z != right->tile.z)
                    return left->tile.z < right->tile.z;
                return left->tile.x < right->tile.x;
            });

            for (const TerrainEditTile *tile : tiles)
            {
                if (tile->weights.empty())
                    continue;
                hash = HashU32(hash, static_cast<uint32_t>(tile->tile.x));
                hash = HashU32(hash, static_cast<uint32_t>(tile->tile.z));
                for (const TerrainEditWeight &channel : tile->weights)
                {
                    anyWeight = true;
                    hash = HashBytes(hash, channel.layer);
                    for (const float value : channel.values)
                        hash = HashF32(hash, value);
                }
            }
        }
        return anyWeight ? hash : 0;
    }
} // namespace mir

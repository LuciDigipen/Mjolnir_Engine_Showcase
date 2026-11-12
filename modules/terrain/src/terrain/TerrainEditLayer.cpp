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

    void SetTerrainEditSample(TerrainEditLayer &layer, const TerrainLayout &layout, GlobalSampleIndex sample,
                              float value, float alpha)
    {
        const int32_t intervals = layout.IntervalsPerTile();
        const size_t count = SampleCount(intervals);
        const bool alphaBlend = layer.info.heightBlend == TerrainEditBlend::AlphaBlend;

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

        const TileRange range = layout.Tiles();
        for (size_t ix = 0; ix < countX; ++ix)
        {
            for (size_t iz = 0; iz < countZ; ++iz)
            {
                if (tileX[ix] < range.min.x || tileX[ix] > range.max.x || tileZ[iz] < range.min.z ||
                    tileZ[iz] > range.max.z)
                    continue;

                const TileCoord coord{static_cast<int32_t>(tileX[ix]), static_cast<int32_t>(tileZ[iz])};
                TerrainEditTile &tile = layer.tiles[coord];
                tile.tile = coord;
                tile.intervals = intervals;
                tile.values.resize(count, 0.0f);
                if (alphaBlend)
                    tile.alpha.resize(count, 0.0f);

                const int64_t localX = sample.x - tileX[ix] * intervals;
                const int64_t localZ = sample.z - tileZ[iz] * intervals;
                const size_t index = static_cast<size_t>(localZ) * static_cast<size_t>(intervals + 1) +
                                     static_cast<size_t>(localX);
                tile.values[index] = value;
                if (alphaBlend)
                    tile.alpha[index] = alpha;
            }
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
            if (tile.intervals != intervals || tile.values.size() != count)
                return false;
            if (alpha && tile.alpha.size() != count)
                return false;

            const int64_t localX = x - static_cast<int64_t>(coord.x) * intervals;
            const int64_t localZ = z - static_cast<int64_t>(coord.z) * intervals;
            const size_t index = static_cast<size_t>(localZ) * side + static_cast<size_t>(localX);
            out = alpha ? tile.alpha[index] : tile.values[index];
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
        if (tile.values.size() != count)
            return Fail<std::vector<std::byte>>(
                Code::InvalidInput, std::format("tile ({}, {}) has {} values, expected {}", tile.tile.x, tile.tile.z,
                                                tile.values.size(), count));

        const bool hasAlpha = blend == TerrainEditBlend::AlphaBlend;
        if (hasAlpha && tile.alpha.size() != count)
            return Fail<std::vector<std::byte>>(
                Code::InvalidInput, std::format("tile ({}, {}) has {} alpha values, expected {}", tile.tile.x, tile.tile.z,
                                                tile.alpha.size(), count));

        for (size_t index = 0; index < count; ++index)
        {
            if (!std::isfinite(tile.values[index]))
                return Fail<std::vector<std::byte>>(
                    Code::InvalidInput, std::format("tile ({}, {}) sample {} is not finite", tile.tile.x, tile.tile.z,
                                                    index));
            if (hasAlpha && !(tile.alpha[index] >= 0.0f && tile.alpha[index] <= 1.0f))
                return Fail<std::vector<std::byte>>(
                    Code::InvalidInput, std::format("tile ({}, {}) alpha {} is outside 0 to 1", tile.tile.x, tile.tile.z,
                                                    index));
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

    TerrainResult<TerrainEditDecodedTile> DecodeTerrainEditTile(std::span<const std::byte> bytes)
    {
        using namespace terraineditformat;

        if (bytes.size() < kHeaderSize + kChannelSize + kTrailerSize)
            return Fail<TerrainEditDecodedTile>(Code::Malformed, "tile blob is truncated");
        if (detail::LoadU32(bytes, kMagicOffset) != kMagic)
            return Fail<TerrainEditDecodedTile>(Code::Malformed, "tile blob magic is not an edit tile");
        if (detail::LoadU32(bytes, kVersionOffset) != kTerrainEditFormatVersion)
            return Fail<TerrainEditDecodedTile>(
                Code::Unsupported, std::format("tile blob version {} is not supported",
                                               detail::LoadU32(bytes, kVersionOffset)));

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
                Code::Malformed, std::format("tile blob size {} does not match {} intervals and alpha {}", bytes.size(),
                                             intervals, hasAlpha ? "payloads" : "none"));

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
} // namespace mir

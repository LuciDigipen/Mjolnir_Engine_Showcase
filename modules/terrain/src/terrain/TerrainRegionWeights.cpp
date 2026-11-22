/**
 * @file TerrainRegionWeights.cpp
 * @author Jackey Cen
 * @brief Derives every Region's RGBA8 material weight image from a release's tile weights.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TerrainRegionWeights.h"

#include "PackInternal.h"

#include <terrain/TileHeights.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

namespace mir
{
    namespace
    {
        using Code = TerrainPackErrorCode;
        using detail::Describe;
        using detail::Fail;

        struct IndexRange
        {
            int64_t first = 0;
            int64_t last = -1;

            bool Empty() const
            {
                return first > last;
            }
        };

        // Half-open [low, high): a sample on a texel edge belongs to the next texel only.
        IndexRange SamplesIn(double low, double high, double spacing)
        {
            if (!(low < high))
                return {};
            int64_t first = static_cast<int64_t>(std::ceil(low / spacing));
            if (first * spacing < low)
                ++first;
            int64_t last = static_cast<int64_t>(std::ceil(high / spacing)) - 1;
            if (last * spacing >= high)
                --last;
            return {first, last};
        }

        int64_t NearestSample(double center, double spacing, int64_t first, int64_t last)
        {
            const int64_t nearest = static_cast<int64_t>(std::llround(center / spacing));
            return std::clamp(nearest, first, last);
        }

        std::string DescribeRegion(TerrainRegionCoord region)
        {
            return std::format("Region L{} ({}, {})", region.level, region.x, region.z);
        }

        struct LayerSlot
        {
            uint32_t layer = 0;
            size_t column = 0;
        };

        TerrainResult<TerrainRegionWeightImage> BuildOne(const TerrainRegionGrid &grid,
                                                          std::span<const std::string> table,
                                                          const TerrainWeightTileLookup &lookup, uint32_t side,
                                                          TerrainRegionCoord region)
        {
            using R = TerrainRegionWeightImage;
            const TerrainLayout &layout = grid.Reference();
            const TerrainLocalBounds rect = grid.Rect(region);
            const TerrainLocalBounds &bounds = layout.Bounds();
            const double texelSpacing = (rect.maxX - rect.minX) / double(side - 1);
            if (!(texelSpacing > 0.0))
                return Fail<R>(Code::InvalidInput, "{}: the Region rect is empty for side {}", DescribeRegion(region),
                               side);

            const size_t count = size_t(side) * side;
            const size_t columns = table.size();
            std::vector<double> sums(columns * count, 0.0);
            std::vector<uint32_t> taken(count, 0);
            std::unordered_map<TileCoord, const TerrainWeightTile *> tiles;
            std::unordered_map<TileCoord, std::vector<LayerSlot>> tileColumns;

            auto loadTile = [&](TileCoord tile) -> const TerrainWeightTile *
            {
                const auto cached = tiles.find(tile);
                if (cached != tiles.end())
                    return cached->second;
                const TerrainWeightTile *weights = lookup(tile);
                if (!weights)
                    return nullptr;
                if (weights->intervals != layout.IntervalsPerTile())
                    return nullptr;
                std::vector<LayerSlot> columnsOf;
                columnsOf.reserve(weights->layers.size());
                for (size_t l = 0; l < weights->layers.size(); ++l)
                {
                    const auto at = std::find(table.begin(), table.end(), weights->layers[l]);
                    if (at == table.end())
                        return nullptr;
                    columnsOf.push_back({static_cast<uint32_t>(l), size_t(at - table.begin())});
                }
                tiles.emplace(tile, weights);
                tileColumns.emplace(tile, std::move(columnsOf));
                return weights;
            };

            auto addSample = [&](int64_t i, int64_t k, size_t texel) -> bool
            {
                const TileRange mapTiles = layout.Tiles();
                const TileCoord at{std::clamp(layout.TileOfCell(i), mapTiles.min.x, mapTiles.max.x),
                                   std::clamp(layout.TileOfCell(k), mapTiles.min.z, mapTiles.max.z)};
                const TerrainWeightTile *weights = loadTile(at);
                if (!weights)
                    return false;
                const std::vector<LayerSlot> &columnsOf = tileColumns.at(at);
                const int64_t sideOfTile = int64_t(layout.IntervalsPerTile()) + 1;
                const int64_t local =
                    (k - layout.TileFirstSample(at.z)) * sideOfTile + (i - layout.TileFirstSample(at.x));
                for (const LayerSlot &slot : columnsOf)
                    sums[slot.column * count + texel] +=
                        double(weights->weights[slot.layer * size_t(sideOfTile * sideOfTile) + size_t(local)]);
                ++taken[texel];
                return true;
            };

            for (uint32_t j = 0; j < side; ++j)
                for (uint32_t i = 0; i < side; ++i)
                {
                    const size_t texel = size_t(j) * side + i;
                    const double cx = rect.minX + double(i) * texelSpacing;
                    const double cz = rect.minZ + double(j) * texelSpacing;
                    const double lowX = std::max(cx - texelSpacing * 0.5, bounds.minX);
                    const double highX = std::min(cx + texelSpacing * 0.5, bounds.maxX);
                    const double lowZ = std::max(cz - texelSpacing * 0.5, bounds.minZ);
                    const double highZ = std::min(cz + texelSpacing * 0.5, bounds.maxZ);
                    const IndexRange x = SamplesIn(lowX, highX, layout.SampleSpacing());
                    const IndexRange z = SamplesIn(lowZ, highZ, layout.SampleSpacing());
                    if (!x.Empty() && !z.Empty())
                        for (int64_t k = z.first; k <= z.last; ++k)
                            for (int64_t s = x.first; s <= x.last; ++s)
                                if (!addSample(s, k, texel))
                                    return Fail<R>(Code::InvalidInput, "{}: tile {} has no weights",
                                                   DescribeRegion(region),
                                                   Describe(TileCoord{layout.TileOfCell(s), layout.TileOfCell(k)}));
                    if (taken[texel] != 0)
                        continue;

                    const TileRange tilesOfMap = layout.Tiles();
                    const int64_t n = layout.IntervalsPerTile();
                    const int64_t firstX = int64_t(tilesOfMap.min.x) * n;
                    const int64_t firstZ = int64_t(tilesOfMap.min.z) * n;
                    const int64_t lastX = (int64_t(tilesOfMap.max.x) + 1) * n;
                    const int64_t lastZ = (int64_t(tilesOfMap.max.z) + 1) * n;
                    const int64_t nearX = NearestSample(cx, layout.SampleSpacing(), firstX, lastX);
                    const int64_t nearZ = NearestSample(cz, layout.SampleSpacing(), firstZ, lastZ);
                    if (!addSample(nearX, nearZ, texel))
                        return Fail<R>(Code::InvalidInput, "{}: tile {} has no weights", DescribeRegion(region),
                                       Describe(TileCoord{layout.TileOfCell(nearX), layout.TileOfCell(nearZ)}));
                }

            TerrainWeightTile averaged;
            averaged.tile = {region.x, region.z};
            averaged.intervals = static_cast<int32_t>(side) - 1;
            averaged.layers.assign(table.begin(), table.end());
            averaged.weights.resize(columns * count);
            for (size_t texel = 0; texel < count; ++texel)
            {
                const double divisor = double(taken[texel]);
                for (size_t column = 0; column < columns; ++column)
                    averaged.weights[column * count + texel] = float(sums[column * count + texel] / divisor);
            }

            TerrainResult<TerrainEncodedWeights> encoded = EncodeTerrainWeightPayload(averaged, table);
            if (!encoded)
                return detail::Failure<R>(encoded.error, DescribeRegion(region) + ": ");
            TerrainRegionWeightImage image;
            image.region = region;
            image.side = side;
            image.slots = encoded.value->slots;
            image.bytes = std::move(encoded.value->bytes);
            TerrainResult<R> result;
            result.value = std::move(image);
            return result;
        }
    } // namespace

    TerrainResult<std::vector<TerrainRegionWeightImage>>
    BuildTerrainRegionWeights(const TerrainRegionGrid &grid, std::span<const std::string> table,
                              const TerrainWeightTileLookup &tiles, const TerrainRegionWeightOptions &options)
    {
        using R = std::vector<TerrainRegionWeightImage>;
        if (!tiles)
            return Fail<R>(Code::InvalidInput, "no tile weight lookup");
        if (options.side < 2 || options.side > uint32_t(kMaxTerrainTileIntervals) + 1)
            return Fail<R>(Code::InvalidInput, "Region weight side {} is outside 2 to {}", options.side,
                           uint32_t(kMaxTerrainTileIntervals) + 1);
        if (table.empty())
            return Fail<R>(Code::InvalidInput, "the layer table is empty");

        const uint64_t total = grid.RegionCount();
        std::vector<TerrainResult<TerrainRegionWeightImage>> results(static_cast<size_t>(total));
        const auto build = [&](uint64_t begin, uint64_t end)
        {
            for (uint64_t ordinal = begin; ordinal < end; ++ordinal)
                results[size_t(ordinal)] = BuildOne(grid, table, tiles, options.side, grid.AtOrdinal(ordinal));
        };

        const uint32_t workers = std::min<uint32_t>(options.workers, static_cast<uint32_t>(std::max<uint64_t>(total, 1)));
        if (workers <= 1)
            build(0, total);
        else
        {
            std::atomic<uint64_t> next{0};
            constexpr uint64_t kChunk = 1;
            std::vector<std::thread> threads;
            threads.reserve(workers);
            for (uint32_t w = 0; w < workers; ++w)
                threads.emplace_back(
                    [&]
                    {
                        for (;;)
                        {
                            const uint64_t begin = next.fetch_add(kChunk);
                            if (begin >= total)
                                break;
                            build(begin, std::min(begin + kChunk, total));
                        }
                    });
            for (std::thread &thread : threads)
                thread.join();
        }

        R images;
        images.reserve(size_t(total));
        for (auto &result : results)
        {
            if (!result)
                return detail::Failure<R>(result.error, "");
            images.push_back(std::move(*result.value));
        }
        return {std::move(images), {}};
    }
} // namespace mir

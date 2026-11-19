/**
 * @file TerrainEditTiles.h
 * @author Jackey Cen
 * @brief The Edit Layer tiles a brush stroke changed, before and after.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainEditLayer.h>

#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <utility>
#include <optional>

namespace mir
{
    struct TerrainTileOrder
    {
        bool operator()(const TileCoord &left, const TileCoord &right) const
        {
            return left.z != right.z ? left.z < right.z : left.x < right.x;
        }
    };

    inline SampleRange ClipBrushSamples(const TerrainLayout &layout, const SampleRange &tile,
                                        double centerX, double centerZ, double radius)
    {
        const auto clipAxis = [&](int64_t first, int64_t last, double center) {
            const auto lowerBound = [&](double distance) {
                int64_t lo = first;
                int64_t hi = last + 1;
                while (lo < hi)
                {
                    const int64_t mid = std::midpoint(lo, hi);
                    if (layout.SamplePosition(mid) - center < distance)
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                return lo;
            };
            const auto pastRadius = std::nextafter(radius, std::numeric_limits<double>::infinity());
            return std::pair{lowerBound(-radius), lowerBound(pastRadius) - 1};
        };
        const auto [minX, maxX] = clipAxis(tile.min.x, tile.max.x, centerX);
        const auto [minZ, maxZ] = clipAxis(tile.min.z, tile.max.z, centerZ);
        return {{minX, minZ}, {maxX, maxZ}};
    }

    // A missing tile is an absent layer tile, not an empty one.
    using TerrainEditTileStates = std::map<TileCoord, std::optional<TerrainEditTile>, TerrainTileOrder>;
} // namespace mir

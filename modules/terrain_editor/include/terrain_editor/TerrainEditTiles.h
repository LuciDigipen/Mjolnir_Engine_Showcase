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

#include <map>
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

    // A missing tile is an absent layer tile, not an empty one.
    using TerrainEditTileStates = std::map<TileCoord, std::optional<TerrainEditTile>, TerrainTileOrder>;
} // namespace mir

/**
 * @file TerrainRegionWeights.h
 * @author Jackey Cen
 * @brief Derives every Region's RGBA8 material weight image from a release's tile weights.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainConstants.h>

#include <terrain/TerrainPackWriter.h>
#include <terrain/TerrainRegionGrid.h>
#include <terrain/TerrainWeights.h>
#include <terrain/TileKey.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace mir
{
    // Nullopt when the tile has no weight payload.
    using TerrainWeightTileLookup = std::function<const TerrainWeightTile *(TileCoord)>;

    struct TerrainRegionWeightOptions
    {
        uint32_t side = kTerrainRegionWeightSide;
        // Every count gives the same images. 0 runs on the caller.
        uint32_t workers = 0;
    };

    // One image per Region of grid, in ordinal order. A texel averages, in the layer table's space, the tile
    // weights of the reference samples in the square of one texel spacing around it, clipped to the map; a texel
    // whose square holds no sample takes the nearest map sample. Levels are independent: an image comes from the
    // tile weights alone, never from a child's image, so a bake and a validation agree byte for byte.
    TerrainResult<std::vector<TerrainRegionWeightImage>>
    BuildTerrainRegionWeights(const TerrainRegionGrid &grid, std::span<const std::string> table,
                              const TerrainWeightTileLookup &tiles, const TerrainRegionWeightOptions &options = {});
} // namespace mir

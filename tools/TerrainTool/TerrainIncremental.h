/**
 * @file TerrainIncremental.h
 * @author Jackey Cen
 * @brief Decides whether a published release can supply a candidate's unchanged derivative payloads.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainRecipe.h"
#include "TerrainRegions.h"

#include <terrain/TerrainRelease.h>
#include <terrain/TileHeights.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mir::terrainbake
{
    // Tiles whose final source height changed, plus every existing tile sharing an edge or corner.
    std::vector<TileCoord> TerrainAffectedTiles(const TerrainReleaseHandle &base,
                                                std::span<const TileHeights> finalHeights, std::size_t &changedTiles);

    // Empty when the base index matches what a full bake derives from; otherwise why a full bake is needed.
    // Weight payloads are never reused, so only the height structure takes part.
    std::optional<std::string> TerrainBaseMismatch(const TerrainReleaseHandle &base, const TerrainRecipe &recipe,
                                                   std::span<const TileHeights> finalHeights,
                                                   const TerrainRegionSettings &regions);
}

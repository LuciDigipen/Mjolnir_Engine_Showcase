/**
 * @file TerrainDerivatives.h
 * @author Jackey Cen
 * @brief Derives terrain LODs, normals and query heights from final source heights.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "TerrainRecipe.h"
#include "TerrainRegions.h"
#include <terrain/TerrainLod.h>
#include <terrain/TerrainPackWriter.h>
#include <cstddef>
#include <optional>
#include <unordered_set>
#include <vector>

namespace mir::terrainbake
{
    inline constexpr double kTerrainInitialMaxLodSpacing = 8.0;
    struct TerrainDerivatives
    {
        std::vector<TerrainLayout> layers;
        std::vector<TileHeights> queryHeights;
        std::vector<uint32_t> queryLayers;
        std::vector<double> queryErrors;
        std::vector<TerrainPayloadWrite> payloads;
        // Baked only when settings are given.
        std::optional<TerrainRegionBake> regions;
    };
    // The layers BakeTerrainDerivatives builds from the recipe and final sources.
    std::vector<TerrainLayout> TerrainDerivativeLayers(const TerrainRecipe &recipe,
                                                       std::span<const TileHeights> finalHeights);

    // No value uses JobExecutor::DefaultWorkerCount(); 0 runs on the caller.
    // onlyTiles limits the per-tile payloads; Regions still use every final source.
    std::optional<TerrainDerivatives> BakeTerrainDerivatives(const TerrainRecipe &recipe,
                                                             std::span<const TileHeights> finalHeights,
                                                             std::string &error,
                                                             const TerrainRegionSettings *regions = nullptr,
                                                             std::optional<std::size_t> workers = std::nullopt,
                                                             const std::unordered_set<TileCoord> *onlyTiles = nullptr);
}

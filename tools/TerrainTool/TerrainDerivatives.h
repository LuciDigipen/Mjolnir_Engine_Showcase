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
#include <terrain/TerrainLod.h>
#include <terrain/TerrainPackWriter.h>
#include <optional>

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
    };
    std::optional<TerrainDerivatives> BakeTerrainDerivatives(const TerrainRecipe &recipe,
                                                             std::span<const TileHeights> finalHeights,
                                                             std::string &error);
}

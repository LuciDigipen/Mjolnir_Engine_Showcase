#pragma once

#include "TerrainRecipe.h"
#include "TerrainReport.h"
#include <terrain/TileHeights.h>
#include <span>

namespace mir::terrainbake
{
    inline constexpr uint32_t kTerrainSeamVersion = 1;
    inline constexpr double kTerrainSeamBandMultiplier = 2.0;
    inline constexpr double kTerrainSeamWarningThreshold = 0.5;

    enum class TerrainSeamWeight { Smoothstep, Linear };
    struct TerrainSeamOptions
    {
        double bandMultiplier = kTerrainSeamBandMultiplier;
        TerrainSeamWeight weight = TerrainSeamWeight::Smoothstep;
    };

    bool AdjustTerrainSeams(const TerrainRecipe &recipe, std::span<TileHeights> tiles,
                            TerrainReport &report, TerrainSeamOptions options = {});
    void ReportSeamWarnings(TerrainReport &report);
}

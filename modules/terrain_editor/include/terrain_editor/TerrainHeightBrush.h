/**
 * @file TerrainHeightBrush.h
 * @author Jackey Cen
 * @brief Applies a height brush's dabs to one Edit Layer and records the tiles it changed.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain_editor/TerrainEditBase.h>
#include <terrain_editor/TerrainEditMode.h>
#include <terrain_editor/TerrainEditTiles.h>

#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainLayout.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mir
{
    // Raise or lower speed at full strength and the brush center.
    inline constexpr double kTerrainRaiseMetersPerSecond = 10.0;
    // Share of the way to the smooth or flatten target covered a second at full strength and the brush center.
    inline constexpr double kTerrainSettleRatePerSecond = 8.0;

    struct TerrainBrushSettings
    {
        TerrainBrush brush = TerrainBrush::RaiseLower;
        double radius = 8.0;
        double strength = 0.5;
        double falloff = 0.5;
        bool lower = false;
    };

    struct TerrainBrushTileSource
    {
        // Tile addressing and bounds; each tile still edits on its own source grid.
        TerrainLayout reference;
        std::function<std::optional<TerrainLayout>(TileCoord)> layout;
        TerrainBaseTileProvider base;
    };

    struct TerrainBrushDabResult
    {
        std::vector<TileCoord> changed;
        std::string error;

        bool Ok() const { return error.empty(); }
    };

    class TerrainBrushStroke
    {
    public:
        // stack holds the document's layers, including the one this stroke edits.
        TerrainBrushStroke(TerrainEditLayer &layer, std::span<const TerrainEditLayer> stack,
                           TerrainBrushTileSource source, double startHeight,
                           std::function<void()> changed = {});

        TerrainBrushDabResult Dab(TerrainLocalPosition center, const TerrainBrushSettings &settings, double seconds);

        TerrainEditTileStates Before() const { return mBefore; }
        TerrainEditTileStates After() const;
        bool Empty() const { return mBefore.empty(); }

    private:
        TerrainEditLayer &mLayer;
        std::span<const TerrainEditLayer> mStack;
        TerrainBrushTileSource mSource;
        std::vector<TerrainEditLayer> mBelowStack;
        std::function<void()> mChanged;
        double mStartHeight = 0.0;
        TerrainEditTileStates mBefore;
    };
} // namespace mir

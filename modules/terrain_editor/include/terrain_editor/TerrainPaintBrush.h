/**
 * @file TerrainPaintBrush.h
 * @author Jackey Cen
 * @brief Paints a material layer's weight channels into one Edit Layer.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain_editor/TerrainEditTiles.h>
#include <terrain_editor/TerrainHeightBrush.h>

#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainLayout.h>

#include <functional>
#include <string>
#include <string_view>

namespace mir
{
    class TerrainPaintStroke
    {
    public:
        TerrainPaintStroke(TerrainEditLayer &layer, TerrainLayout reference, std::string materialLayer,
                           std::function<void()> changed = {});

        TerrainBrushDabResult Dab(TerrainLocalPosition center, const TerrainBrushSettings &settings, double seconds);

        TerrainEditTileStates Before() const { return mBefore; }
        TerrainEditTileStates After() const;
        bool Empty() const { return mBefore.empty(); }

    private:
        TerrainEditLayer &mLayer;
        TerrainLayout mReference;
        std::string mMaterialLayer;
        std::function<void()> mChanged;
        TerrainEditTileStates mBefore;
    };
} // namespace mir

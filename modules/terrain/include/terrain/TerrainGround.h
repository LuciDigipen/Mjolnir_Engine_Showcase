/**
 * @file TerrainGround.h
 * @author Jackey Cen
 * @brief Ground heights at the finest baked source precision, for the editor's snap.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/GroundQuery.h>
#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainHeightQuery.h>

namespace mir
{
    class TerrainWorld;

    // Coarse residency while the source tile is requested stays Loading.
    GroundQueryResult ProbeTerrainHeightResult(const TerrainHeightResult &result);

    // Queries every instance; asks for the finest baked spacing at the point, never a coarser layer.
    GroundQueryResult ProbeTerrainGround(TerrainWorld &world, WorldPosition position);

    // Layout bounds and source spacing only; never requests or reads a payload.
    GroundQueryCoverage ProbeTerrainGroundCoverage(TerrainWorld &world, WorldPosition position);
}

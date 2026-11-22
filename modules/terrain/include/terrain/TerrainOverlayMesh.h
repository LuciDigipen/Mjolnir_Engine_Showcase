/**
 * @file TerrainOverlayMesh.h
 * @author Jackey Cen
 * @brief Rebuilds a tile's finest-level mesh from the heights an overlay snapshot holds for it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLod.h>
#include <terrain/TerrainOverlay.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainTileMesh.h>
#include <terrain/TileHeights.h>
#include <terrain/TileKey.h>

#include <memory>
#include <span>
#include <vector>

namespace mir
{
    // The source tiles an overlay mesh reads besides the overlay's own tile: the eight neighbours the release bakes,
    // since the bake's normal stencil can reach a diagonal one where incident tiles are coarser.
    std::vector<TileCoord> TerrainOverlaySourceTiles(const TerrainIndex &index, TileCoord tile);

    // The release's level-0 positions and indices with the tile's heights read as the bake reads them: its own source
    // inside, the owner's at a tile edge. sources holds the release source blocks of TerrainOverlaySourceTiles; a
    // missing one is read from the overlay tile, as the bake treats a tile it does not have.
    TerrainResult<TerrainTileMesh> BuildTerrainOverlayTileMesh(
        const TerrainIndex &index, const TerrainReleaseInfo &release, TileCoord tile, const TerrainLodSurface &topology,
        const TerrainOverlay &overlay, std::span<const std::shared_ptr<const TileHeights>> sources);
} // namespace mir

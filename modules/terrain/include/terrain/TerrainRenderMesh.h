/**
 * @file TerrainRenderMesh.h
 * @author Jackey Cen
 * @brief Reads render meshes from indexed terrain LOD and normal payloads.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once

#include <terrain/TerrainRelease.h>
#include <terrain/TerrainTileMesh.h>

namespace mir
{
    TerrainResult<TerrainTileMesh> ReadTerrainRenderMesh(const TerrainReleaseHandle &release, TileCoord tile,
                                                         std::optional<uint32_t> layer = std::nullopt);
    TerrainResult<TerrainTileMesh> MakeTerrainRenderMesh(const TerrainIndex &index, const TerrainLodSurface &lod,
                                                         const TerrainLodNormals &normals);
}

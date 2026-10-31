/**
 * @file TerrainMaterialUv.h
 * @author Jackey Cen
 * @brief Stable terrain-local material UVs and the tile-period rule.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace mir
{
    // The validation period for material tile sizes, in meters. A material's
    // tile size must divide it so far origins stay distinguishable in float.
    inline constexpr double kTerrainMaterialUvPeriodMeters = 3840.0;

    // A positive tile period that divides the validation period within the
    // error of its float32 material parameter. remainderMeters receives the
    // distance from the nearest whole number of repeats.
    bool TerrainMaterialTilePeriodSupported(double tilePeriodMeters, double *remainderMeters = nullptr);

    // The mesh origin reduced once in double, so a vertex's UV can stay affine.
    glm::dvec2 TerrainMaterialUvOrigin(glm::dvec3 terrainLocalOriginMeters);

    // Terrain-local meters, with the origin reduced once and the vertex added
    // before narrowing. The result is in meters; a material divides by its tile size.
    glm::vec2 TerrainMaterialUv(glm::dvec3 terrainLocalOriginMeters, glm::vec3 localVertexMeters);
}

/**
 * @file TerrainMaterial.h
 * @author Jackey Cen
 * @brief Recipe material layers, rule coverage and the weight fingerprint.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainRecipeMaterials.h>
#include <terrain/TerrainWeights.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir::terrainbake
{
    // Bumping it changes every weight fingerprint.
    inline constexpr uint32_t kMaterialCanonicalVersion = 1;
    inline constexpr size_t kMaxMaterialNameLength = mir::kMaxMaterialNameLength;

    using TerrainMaterialHeightRule = mir::TerrainMaterialHeightRule;
    using TerrainMaterialSlopeRule = mir::TerrainMaterialSlopeRule;
    using TerrainMaterialNoiseRule = mir::TerrainMaterialNoiseRule;
    using TerrainMaterialRules = mir::TerrainMaterialRules;
    using TerrainMaterialLayer = mir::TerrainRecipeMaterialLayer;
    using mir::DefaultTerrainMaterialLayers;

    struct TerrainRecipe;
    struct TerrainFinalSurface;

    // 1 inside [min, max], falling linearly to 0 at blend past either edge; a zero blend is a hard edge.
    double TerrainMaterialRamp(double value, double min, double max, double blend);
    double TerrainMaterialNoiseFactor(double strength, double fbm);
    uint64_t TerrainMaterialLayerSeed(uint64_t recipeSeed, std::string_view layerName);

    // Reference-layout weights of one tile, from its final surface heights and normals.
    std::optional<TerrainWeightTile> EvaluateTerrainWeights(const TerrainRecipe &recipe,
                                                            const TerrainFinalSurface &surface, TileCoord tile);

    std::string CanonicalMaterialText(const TerrainRecipe &recipe);
    // FNV-1a 64 of the material text and the height fingerprint; zero maps to 1.
    TerrainSourceFingerprint FingerprintWeights(const TerrainRecipe &recipe, TerrainSourceFingerprint heights);
} // namespace mir::terrainbake

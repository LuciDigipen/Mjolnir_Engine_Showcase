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
#include <terrain/TerrainWeights.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir::terrainbake
{
    // Bumping it changes every weight fingerprint.
    inline constexpr uint32_t kMaterialCanonicalVersion = 1;
    inline constexpr size_t kMaxMaterialNameLength = 32;

    struct TerrainMaterialHeightRule
    {
        double min = 0.0;   // m
        double max = 0.0;   // m
        double blend = 0.0; // m; 0 is a hard edge

        bool operator==(const TerrainMaterialHeightRule &) const = default;
    };

    struct TerrainMaterialSlopeRule
    {
        double min = 0.0;   // degrees
        double max = 0.0;   // degrees
        double blend = 0.0; // degrees; 0 is a hard edge

        bool operator==(const TerrainMaterialSlopeRule &) const = default;
    };

    struct TerrainMaterialNoiseRule
    {
        double wavelength = 1.0; // m
        int32_t octaves = 1;
        double strength = 0.0; // [0, 1]

        bool operator==(const TerrainMaterialNoiseRule &) const = default;
    };

    struct TerrainMaterialRules
    {
        std::optional<TerrainMaterialHeightRule> height;
        std::optional<TerrainMaterialSlopeRule> slope;
        std::optional<TerrainMaterialNoiseRule> noise;

        bool operator==(const TerrainMaterialRules &) const = default;
    };

    // Layers composite in order; the first is the fallback and takes no rules.
    struct TerrainMaterialLayer
    {
        std::string name;
        TerrainMaterialRules rules;

        bool operator==(const TerrainMaterialLayer &) const = default;
    };

    struct TerrainRecipe;
    struct TerrainFinalSurface;

    // 1 inside [min, max], falling linearly to 0 at blend past either edge; a zero blend is a hard edge.
    double TerrainMaterialRamp(double value, double min, double max, double blend);
    double TerrainMaterialNoiseFactor(double strength, double fbm);
    uint64_t TerrainMaterialLayerSeed(uint64_t recipeSeed, std::string_view layerName);

    // One layer named "base"; a recipe without materials behaves as it.
    std::vector<TerrainMaterialLayer> DefaultTerrainMaterialLayers();

    // Reference-layout weights of one tile, from its final surface heights and normals.
    std::optional<TerrainWeightTile> EvaluateTerrainWeights(const TerrainRecipe &recipe,
                                                            const TerrainFinalSurface &surface, TileCoord tile);

    std::string CanonicalMaterialText(const TerrainRecipe &recipe);
    // FNV-1a 64 of the material text and the height fingerprint; zero maps to 1.
    TerrainSourceFingerprint FingerprintWeights(const TerrainRecipe &recipe, TerrainSourceFingerprint heights);
} // namespace mir::terrainbake

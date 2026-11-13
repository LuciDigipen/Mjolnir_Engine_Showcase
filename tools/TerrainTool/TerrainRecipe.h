/**
 * @file TerrainRecipe.h
 * @author Jackey Cen
 * @brief Terrain source recipe: JSON validation, canonical text and source fingerprint.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainMaterial.h"

#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir::terrainbake
{
    inline constexpr uint32_t kTerrainRecipeVersion = 2;
    // Bumping it changes every fingerprint.
    inline constexpr uint32_t kRecipeCanonicalVersion = 4;
    inline constexpr std::string_view kReliefGeneratorId = "mir.relief";
    // Bump when anything changes the samples.
    inline constexpr uint32_t kReliefGeneratorVersion = 2;
    inline constexpr std::string_view kHeightmapGeneratorId = "mir.heightmap";
    // Bump when anything changes the samples.
    inline constexpr uint32_t kHeightmapGeneratorVersion = 1;
    // float32 still resolves ~1 mm here.
    inline constexpr double kMaxHeightMagnitude = 10000.0;

    class TerrainHeightSource;

    struct NoiseTerm
    {
        double amplitude = 0.0;  // m, [0, 10000]
        double wavelength = 1.0; // m, [0.01, 1e6]; finest octave >= 2 * sampleSpacing
        int32_t octaves = 1;     // [1, 8]
    };

    // "mir.relief" v1; formula in TerrainGenerator.cpp. Must stay within kMaxHeightMagnitude.
    struct ReliefParameters
    {
        double baseHeight = 0.0;   // m, [-10000, 10000]
        double tiltX = 0.0;        // m per m, [-1, 1]
        double tiltZ = 0.0;        // m per m, [-1, 1]
        NoiseTerm broad;           // world-scale relief, fBm in [-1, 1]
        NoiseTerm ridges;          // sharp crest lines, ridged in [0, 1]
        NoiseTerm detail;          // local variation, fBm in [-1, 1]
        double plainsHeight = 0.0; // m, [-10000, 10000]
        double plainsBlend = 0.0;  // m, [0, 1000]; 0 is a hard floor
    };

    struct TerrainSourceRule
    {
        TileCoord tile;
        double spacing = 1.0;
    };

    struct TerrainSourceEstimate
    {
        double spacing = 1.0;
        uint64_t tileCount = 0;
        uint64_t samplesPerTile = 0;
        uint64_t heightBytes = 0;
    };

    // Every field is required, so no hidden default can change a fingerprint.
    struct TerrainRecipe
    {
        uint32_t recipeVersion = kTerrainRecipeVersion;
        std::string generatorId;
        uint32_t generatorVersion = 0;
        uint64_t seed = 0;
        TerrainLayout layout;
        ReliefParameters relief;
        // v1 is uniform; v2 null requires explicit coverage.
        std::optional<double> defaultSpacing;
        std::vector<TerrainSourceRule> sourceRules;
        // "mir.heightmap" only. The hash, not the path, identifies the samples, so only it is fingerprinted.
        std::string heightmapPath;    // as written; relative to the recipe's directory
        uint64_t heightmapHash = 0;   // FNV-1a 64 of the whole file
        // Material layers; one named "base" when the recipe has no materials section.
        std::vector<TerrainMaterialLayer> materials;
        // True when the JSON had a materials section; the implicit base layer publishes no weights.
        bool hasMaterialsSection = false;
        std::filesystem::path recipeDir; // set by LoadTerrainRecipe
        // Loaded only when a bake runs, so a current release needs no source file.
        std::shared_ptr<const TerrainHeightSource> heightSource;
        // The stack beside the recipe; null for a recipe parsed from text.
        std::shared_ptr<const std::vector<TerrainEditLayer>> editStack;
        // TerrainEditStackHash of the enabled layers; 0 when none of them stores a tile.
        uint64_t editStackHash = 0;
        // TerrainEditWeightStackHash of the enabled layers; 0 when none stores a weight channel.
        uint64_t editWeightStackHash = 0;

        bool IsHeightmap() const { return generatorId == kHeightmapGeneratorId; }
        std::optional<double> SourceSpacing(TileCoord tile) const;
        std::optional<TerrainLayout> SourceLayout(TileCoord tile) const;
        std::vector<TerrainLayout> SourceLayers() const;
        std::vector<TerrainSourceEstimate> SourceEstimates() const;
    };

    // Dotted JSON path; empty for the whole document.
    struct RecipeIssue
    {
        std::string field;
        std::string message;
    };

    struct RecipeParseResult
    {
        std::optional<TerrainRecipe> recipe;
        std::vector<RecipeIssue> issues;
    };

    // Reports every bad field in one pass; never clamps.
    RecipeParseResult ParseTerrainRecipe(std::string_view json);
    RecipeParseResult LoadTerrainRecipe(const std::filesystem::path &path);

    // Sorted "key=value" lines; doubles as bit patterns, so JSON spelling does not matter.
    std::string CanonicalRecipeText(const TerrainRecipe &recipe);
    TerrainSourceFingerprint FingerprintRecipe(const TerrainRecipe &recipe);
    // The recipe check's line: the fingerprint, then the edit stack when an enabled layer stores a tile.
    std::string DescribeRecipeFingerprint(const TerrainRecipe &recipe, TerrainSourceFingerprint fingerprint);
}

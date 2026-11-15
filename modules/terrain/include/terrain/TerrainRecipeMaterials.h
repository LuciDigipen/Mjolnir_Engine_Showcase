/**
 * @file TerrainRecipeMaterials.h
 * @author Jackey Cen
 * @brief A recipe's material layers: JSON rules, their diagnostics and the on-disk form.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    inline constexpr size_t kMaxMaterialNameLength = 32;
    inline constexpr double kMaxTerrainHeightMagnitude = 10000.0;
    inline constexpr double kMinTerrainNoiseWavelength = 0.01;
    inline constexpr double kMaxTerrainNoiseWavelength = 1.0e6;
    inline constexpr double kMaxTerrainMaterialSlopeDegrees = 90.0;
    inline constexpr int32_t kMaxTerrainMaterialNoiseOctaves = 8;

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
    struct TerrainRecipeMaterialLayer
    {
        std::string name;
        TerrainMaterialRules rules;

        bool operator==(const TerrainRecipeMaterialLayer &) const = default;
    };

    // 1 to kMaxMaterialNameLength characters of [a-z0-9_].
    bool IsValidTerrainMaterialName(std::string_view name);

    // One layer named "base"; a recipe without materials behaves as it.
    std::vector<TerrainRecipeMaterialLayer> DefaultTerrainMaterialLayers();

    // Dotted JSON path; every issue of this section starts with "materials".
    struct TerrainRecipeMaterialIssue
    {
        std::string path;
        std::string message;
    };

    struct TerrainRecipeMaterialsParseResult
    {
        std::vector<TerrainRecipeMaterialLayer> layers;
        std::vector<TerrainRecipeMaterialIssue> issues;
    };

    // Null is a recipe without a materials section: the default layers and no issues.
    TerrainRecipeMaterialsParseResult ParseTerrainRecipeMaterials(const nlohmann::json *materials);

    // The materials section as a recipe writes it.
    nlohmann::ordered_json TerrainRecipeMaterialsToJson(std::span<const TerrainRecipeMaterialLayer> layers);

    struct TerrainRecipeMaterialsWriteResult
    {
        std::optional<std::string> text;
        std::string error;
    };

    // Replaces only the materials section; every other key keeps its order and value.
    TerrainRecipeMaterialsWriteResult ReplaceTerrainRecipeMaterials(std::string_view recipeText,
                                                                   std::span<const TerrainRecipeMaterialLayer> layers);
} // namespace mir

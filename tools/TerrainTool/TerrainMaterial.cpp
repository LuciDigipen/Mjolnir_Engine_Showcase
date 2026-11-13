/**
 * @file TerrainMaterial.cpp
 * @author Jackey Cen
 * @brief Recipe material layers, rule coverage and the weight fingerprint.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainMaterial.h"

#include "TerrainFinalSurface.h"
#include "TerrainNoise.h"
#include "TerrainRecipe.h"

#include <assets/AssetId.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <map>

namespace mir::terrainbake
{
    namespace
    {
        constexpr double kRadiansToDegrees = 57.295779513082320876798154814105;

        const std::vector<TerrainMaterialLayer> &LayersOf(const TerrainRecipe &recipe)
        {
            static const std::vector<TerrainMaterialLayer> kFallback = DefaultTerrainMaterialLayers();
            return recipe.materials.empty() ? kFallback : recipe.materials;
        }

        std::string Bits(double value)
        {
            const uint64_t bits = std::bit_cast<uint64_t>(value == 0.0 ? 0.0 : value);
            constexpr char kHex[] = "0123456789abcdef";
            std::string text = "f64:";
            for (int shift = 60; shift >= 0; shift -= 4)
                text += kHex[(bits >> shift) & 0xF];
            return text;
        }

        std::string DescribeRule(bool present, std::string text)
        {
            return present ? std::move(text) : "none";
        }

        double Coverage(const TerrainMaterialLayer &layer, double height, double slopeDegrees, double fbm)
        {
            double coverage = 1.0;
            if (const std::optional<TerrainMaterialHeightRule> &rule = layer.rules.height)
                coverage *= TerrainMaterialRamp(height, rule->min, rule->max, rule->blend);
            if (const std::optional<TerrainMaterialSlopeRule> &rule = layer.rules.slope)
                coverage *= TerrainMaterialRamp(slopeDegrees, rule->min, rule->max, rule->blend);
            if (const std::optional<TerrainMaterialNoiseRule> &rule = layer.rules.noise)
                coverage *= TerrainMaterialNoiseFactor(rule->strength, fbm);
            return std::clamp(coverage, 0.0, 1.0);
        }
    }

    double TerrainMaterialRamp(double value, double min, double max, double blend)
    {
        if (value >= min && value <= max)
            return 1.0;
        if (!(blend > 0.0))
            return 0.0;
        const double outside = value < min ? min - value : value - max;
        return std::clamp(1.0 - outside / blend, 0.0, 1.0);
    }

    double TerrainMaterialNoiseFactor(double strength, double fbm)
    {
        return std::clamp(1.0 + strength * fbm, 0.0, 1.0);
    }

    uint64_t TerrainMaterialLayerSeed(uint64_t recipeSeed, std::string_view layerName)
    {
        return MixBits(recipeSeed ^ Fnv1a64(layerName));
    }

    std::vector<TerrainMaterialLayer> DefaultTerrainMaterialLayers()
    {
        return {TerrainMaterialLayer{std::string("base"), {}}};
    }

    std::optional<TerrainWeightTile> EvaluateTerrainWeights(const TerrainRecipe &recipe,
                                                            const TerrainFinalSurface &surface, TileCoord tile)
    {
        const TileHeights *own = surface.Tile(tile);
        if (!own)
            return std::nullopt;

        const std::vector<TerrainMaterialLayer> &layers = LayersOf(recipe);
        const TerrainLayout &layout = recipe.layout;
        const int32_t intervals = layout.IntervalsPerTile();
        const int32_t side = intervals + 1;
        const size_t count = static_cast<size_t>(side) * static_cast<size_t>(side);

        bool needsSlope = false;
        std::vector<std::optional<OctaveNoise>> noise(layers.size());
        for (size_t layer = 1; layer < layers.size(); ++layer)
        {
            if (layers[layer].rules.slope)
                needsSlope = true;
            if (const std::optional<TerrainMaterialNoiseRule> &rule = layers[layer].rules.noise)
                noise[layer].emplace(TerrainMaterialLayerSeed(recipe.seed, layers[layer].name), rule->wavelength,
                                     rule->octaves);
        }

        TerrainWeightTile weights;
        weights.tile = tile;
        weights.intervals = intervals;
        for (const TerrainMaterialLayer &layer : layers)
            weights.layers.push_back(layer.name);
        weights.weights.assign(layers.size() * count, 0.0f);

        std::vector<double> sampleWeights(layers.size());
        for (int32_t k = 0; k < side; ++k)
        {
            const double z = layout.SamplePosition(layout.TileFirstSample(tile.z) + k);
            for (int32_t i = 0; i < side; ++i)
            {
                const double x = layout.SamplePosition(layout.TileFirstSample(tile.x) + i);
                const double height = surface.SharedHeight(x, z, *own);
                double slope = 0.0;
                if (needsSlope)
                {
                    const glm::vec3 normal = surface.Normal(x, z, *surface.Owner(x, z, *own));
                    slope = std::acos(std::clamp(static_cast<double>(normal.y), -1.0, 1.0)) * kRadiansToDegrees;
                }

                std::fill(sampleWeights.begin(), sampleWeights.end(), 0.0);
                sampleWeights[0] = 1.0;
                for (size_t layer = 1; layer < layers.size(); ++layer)
                {
                    const double fbm = noise[layer] ? noise[layer]->Fbm(x, z) : 0.0;
                    const double coverage = Coverage(layers[layer], height, slope, fbm);
                    for (size_t below = 0; below < layers.size(); ++below)
                        sampleWeights[below] *= 1.0 - coverage;
                    sampleWeights[layer] += coverage;
                }

                const size_t sample = static_cast<size_t>(k) * static_cast<size_t>(side) + static_cast<size_t>(i);
                for (size_t layer = 0; layer < layers.size(); ++layer)
                    weights.weights[layer * count + sample] = static_cast<float>(sampleWeights[layer]);
            }
        }
        return weights;
    }

    std::string CanonicalMaterialText(const TerrainRecipe &recipe)
    {
        const std::vector<TerrainMaterialLayer> &layers = LayersOf(recipe);
        std::map<std::string, std::string> fields;
        fields["canonicalVersion"] = std::to_string(kMaterialCanonicalVersion);
        fields["layerCount"] = std::to_string(layers.size());
        for (size_t i = 0; i < layers.size(); ++i)
        {
            const std::string prefix = "layer." + std::to_string(i) + ".";
            fields[prefix + "name"] = layers[i].name;
            const TerrainMaterialRules &rules = layers[i].rules;
            fields[prefix + "height"] =
                DescribeRule(rules.height.has_value(),
                             rules.height ? "min=" + Bits(rules.height->min) + ",max=" + Bits(rules.height->max) +
                                                ",blend=" + Bits(rules.height->blend)
                                          : std::string{});
            fields[prefix + "slope"] =
                DescribeRule(rules.slope.has_value(),
                             rules.slope ? "min=" + Bits(rules.slope->min) + ",max=" + Bits(rules.slope->max) +
                                               ",blend=" + Bits(rules.slope->blend)
                                         : std::string{});
            fields[prefix + "noise"] =
                DescribeRule(rules.noise.has_value(),
                             rules.noise ? "wavelength=" + Bits(rules.noise->wavelength) + ",octaves=" +
                                               std::to_string(rules.noise->octaves) +
                                               ",strength=" + Bits(rules.noise->strength)
                                         : std::string{});
        }

        std::string text;
        for (const auto &[key, value] : fields)
            text += key + '=' + value + '\n';
        return text;
    }

    TerrainSourceFingerprint FingerprintWeights(const TerrainRecipe &recipe, TerrainSourceFingerprint heights)
    {
        const std::string text = CanonicalMaterialText(recipe) + std::format("heights={:016x}\n", heights.value);
        const uint64_t hash = Fnv1a64(text);
        return TerrainSourceFingerprint(hash == 0 ? 1 : hash);
    }
} // namespace mir::terrainbake

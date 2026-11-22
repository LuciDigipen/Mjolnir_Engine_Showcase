/**
 * @file TerrainLayerBlend.h
 * @author Jackey Cen
 * @brief CPU mirror of the layered terrain material's blend factors.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace mir
{
    inline constexpr uint32_t kMaxTerrainLayers = 4;

    enum class TerrainLayerBlendMode : uint8_t
    {
        Weight,
        Height,
        Alpha
    };

    // Layer weights are the first layerCount components of a draw's layer weights; heights are the
    // layer surfaces' height outputs. Every other component of the returned factors is zero.
    inline std::array<float, kMaxTerrainLayers> TerrainLayerBlendFactors(
        const std::array<float, kMaxTerrainLayers> &weights,
        const std::array<float, kMaxTerrainLayers> &heights, uint32_t layerCount,
        TerrainLayerBlendMode mode, float heightContrast)
    {
        std::array<float, kMaxTerrainLayers> factors{};
        const uint32_t count = std::min(layerCount, kMaxTerrainLayers);
        if (count == 0)
        {
            factors[0] = 1.0f;
            return factors;
        }

        std::array<float, kMaxTerrainLayers> normalized{};
        float sum = 0.0f;
        for (uint32_t index = 0; index < count; ++index)
        {
            normalized[index] = std::max(weights[index], 0.0f);
            sum += normalized[index];
        }
        if (!(sum > 0.0f))
        {
            normalized = {};
            normalized[0] = 1.0f;
            sum = 1.0f;
        }
        for (uint32_t index = 0; index < count; ++index)
            normalized[index] /= sum;

        switch (mode)
        {
        case TerrainLayerBlendMode::Height:
        {
            const float contrast = std::max(heightContrast, 1e-4f);
            float top = -3.402823466e+38f;
            for (uint32_t index = 0; index < count; ++index)
                if (normalized[index] > 0.0f)
                    top = std::max(top, normalized[index] + heights[index]);
            float total = 0.0f;
            for (uint32_t index = 0; index < count; ++index)
            {
                if (normalized[index] > 0.0f)
                {
                    factors[index] = normalized[index] *
                        std::max(normalized[index] + heights[index] - (top - contrast), 0.0f);
                    total += factors[index];
                }
            }
            if (total > 0.0f)
                for (uint32_t index = 0; index < count; ++index)
                    factors[index] /= total;
            break;
        }
        case TerrainLayerBlendMode::Alpha:
        {
            float coverage = 1.0f;
            for (uint32_t index = count; index-- > 1;)
            {
                factors[index] = normalized[index] * coverage;
                coverage *= std::max(1.0f - normalized[index], 0.0f);
            }
            factors[0] = coverage;
            break;
        }
        case TerrainLayerBlendMode::Weight:
        default:
            for (uint32_t index = 0; index < count; ++index)
                factors[index] = normalized[index];
            break;
        }
        return factors;
    }
}

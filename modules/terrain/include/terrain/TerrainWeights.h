/**
 * @file TerrainWeights.h
 * @author Jackey Cen
 * @brief One tile's per-sample material layer weights on the reference layout.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TileHeights.h>
#include <terrain/TileKey.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    inline constexpr int32_t kMaxTerrainMaterialLayers = 32;
    inline constexpr double kTerrainWeightSumTolerance = 1e-5;

    enum class TerrainWeightStatus
    {
        Ok,
        InvalidIntervals,
        LayerCount,
        DuplicateLayerNames,
        SizeMismatch,
        OutOfRange,
        BadSum,
    };

    // Layer-major weights: (n + 1)^2 floats per layer in recipe order, row-major x fastest.
    struct TerrainWeightTile
    {
        TileCoord tile;
        int32_t intervals = 0;
        std::vector<std::string> layers;
        std::vector<float> weights;

        int32_t SamplesPerSide() const { return intervals + 1; }
        size_t SampleCount() const;
        size_t ExpectedWeightCount() const;
        std::span<const float> LayerWeights(size_t layer) const;
        float At(size_t layer, int32_t i, int32_t k) const;

        bool operator==(const TerrainWeightTile &) const = default;
    };

    // Size, layer names, [0, 1] and a sum of 1 per sample.
    TerrainWeightStatus ValidateTerrainWeightTile(const TerrainWeightTile &tile);
    std::string_view DescribeTerrainWeightStatus(TerrainWeightStatus status);
} // namespace mir

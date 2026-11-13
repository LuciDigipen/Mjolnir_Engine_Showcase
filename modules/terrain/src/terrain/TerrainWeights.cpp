/**
 * @file TerrainWeights.cpp
 * @author Jackey Cen
 * @brief One tile's per-sample material layer weights on the reference layout.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain/TerrainWeights.h>

#include <cmath>
#include <set>

namespace mir
{
    size_t TerrainWeightTile::SampleCount() const
    {
        if (intervals < 0)
            return 0;
        const size_t side = static_cast<size_t>(intervals) + 1;
        return side * side;
    }

    size_t TerrainWeightTile::ExpectedWeightCount() const
    {
        return SampleCount() * layers.size();
    }

    std::span<const float> TerrainWeightTile::LayerWeights(size_t layer) const
    {
        const size_t count = SampleCount();
        return std::span<const float>(weights).subspan(layer * count, count);
    }

    float TerrainWeightTile::At(size_t layer, int32_t i, int32_t k) const
    {
        const size_t side = static_cast<size_t>(intervals) + 1;
        return weights[layer * SampleCount() + static_cast<size_t>(k) * side + static_cast<size_t>(i)];
    }

    TerrainWeightStatus ValidateTerrainWeightTile(const TerrainWeightTile &tile)
    {
        if (tile.intervals < 1 || tile.intervals > kMaxTerrainTileIntervals)
            return TerrainWeightStatus::InvalidIntervals;
        if (tile.layers.empty() || tile.layers.size() > size_t(kMaxTerrainMaterialLayers))
            return TerrainWeightStatus::LayerCount;
        std::set<std::string_view> names;
        for (const std::string &name : tile.layers)
            if (!names.insert(name).second)
                return TerrainWeightStatus::DuplicateLayerNames;
        if (tile.weights.size() != tile.ExpectedWeightCount())
            return TerrainWeightStatus::SizeMismatch;
        for (float weight : tile.weights)
            if (!(weight >= 0.0f && weight <= 1.0f))
                return TerrainWeightStatus::OutOfRange;

        const size_t count = tile.SampleCount();
        for (size_t sample = 0; sample < count; ++sample)
        {
            double sum = 0.0;
            for (size_t layer = 0; layer < tile.layers.size(); ++layer)
                sum += tile.weights[layer * count + sample];
            if (!(std::abs(sum - 1.0) <= kTerrainWeightSumTolerance))
                return TerrainWeightStatus::BadSum;
        }
        return TerrainWeightStatus::Ok;
    }

    std::string_view DescribeTerrainWeightStatus(TerrainWeightStatus status)
    {
        switch (status)
        {
        case TerrainWeightStatus::Ok:
            return "ok";
        case TerrainWeightStatus::InvalidIntervals:
            return "invalid interval count";
        case TerrainWeightStatus::LayerCount:
            return "no layer or too many layers";
        case TerrainWeightStatus::DuplicateLayerNames:
            return "duplicate layer names";
        case TerrainWeightStatus::SizeMismatch:
            return "weight count does not match the layer and sample count";
        case TerrainWeightStatus::OutOfRange:
            return "a weight is outside [0, 1]";
        case TerrainWeightStatus::BadSum:
            return "weights do not sum to 1 at a sample";
        }
        return "?";
    }
} // namespace mir

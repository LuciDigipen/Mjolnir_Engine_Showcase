/**
 * @file TerrainWeights.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainWeights.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain/TerrainWeights.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace mir;

namespace
{
    TerrainWeightTile Uniform(int32_t intervals, std::vector<std::string> layers)
    {
        TerrainWeightTile tile;
        tile.tile = {2, -3};
        tile.intervals = intervals;
        tile.layers = std::move(layers);
        tile.weights.assign(tile.ExpectedWeightCount(), 0.0f);
        const size_t count = tile.SampleCount();
        const float share = 1.0f / static_cast<float>(tile.layers.size());
        for (size_t layer = 0; layer < tile.layers.size(); ++layer)
            for (size_t sample = 0; sample < count; ++sample)
                tile.weights[layer * count + sample] = share;
        return tile;
    }
}

TEST_CASE("TerrainWeightTile keeps layer-major weights with x fastest", "[terrain][weights]")
{
    const TerrainWeightTile tile = Uniform(2, {"base", "sand"});
    CHECK(tile.SamplesPerSide() == 3);
    CHECK(tile.SampleCount() == 9);
    CHECK(tile.ExpectedWeightCount() == 18);
    CHECK(tile.LayerWeights(0).size() == 9);
    CHECK(tile.LayerWeights(1).size() == 9);

    TerrainWeightTile indexed = tile;
    for (size_t s = 0; s < indexed.weights.size(); ++s)
        indexed.weights[s] = static_cast<float>(s);
    CHECK(indexed.At(0, 0, 0) == 0.0f);
    CHECK(indexed.At(0, 2, 0) == 2.0f);
    CHECK(indexed.At(0, 0, 1) == 3.0f);
    CHECK(indexed.At(0, 1, 2) == 7.0f);
    CHECK(indexed.At(1, 0, 0) == 9.0f);
    CHECK(indexed.At(1, 2, 2) == 17.0f);
}

TEST_CASE("TerrainWeightTile validates size, layers, range and sums", "[terrain][weights]")
{
    const TerrainWeightTile valid = Uniform(2, {"base", "sand", "rock"});
    CHECK(ValidateTerrainWeightTile(valid) == TerrainWeightStatus::Ok);
    CHECK(ValidateTerrainWeightTile(Uniform(2, {"base"})) == TerrainWeightStatus::Ok);

    TerrainWeightTile tile = valid;
    tile.intervals = 0;
    CHECK(ValidateTerrainWeightTile(tile) == TerrainWeightStatus::InvalidIntervals);

    tile = valid;
    tile.intervals = kMaxTerrainTileIntervals + 1;
    CHECK(ValidateTerrainWeightTile(tile) == TerrainWeightStatus::InvalidIntervals);

    tile = valid;
    tile.layers.clear();
    tile.weights.clear();
    CHECK(ValidateTerrainWeightTile(tile) == TerrainWeightStatus::LayerCount);

    tile = Uniform(1, std::vector<std::string>(kMaxTerrainMaterialLayers + 1, "layer"));
    CHECK(ValidateTerrainWeightTile(tile) == TerrainWeightStatus::LayerCount);

    tile = Uniform(1, {"base", "base"});
    CHECK(ValidateTerrainWeightTile(tile) == TerrainWeightStatus::DuplicateLayerNames);

    tile = valid;
    tile.weights.pop_back();
    CHECK(ValidateTerrainWeightTile(tile) == TerrainWeightStatus::SizeMismatch);

    tile = valid;
    tile.weights[0] = -0.25f;
    CHECK(ValidateTerrainWeightTile(tile) == TerrainWeightStatus::OutOfRange);

    tile = valid;
    tile.weights[1] = 1.5f;
    CHECK(ValidateTerrainWeightTile(tile) == TerrainWeightStatus::OutOfRange);

    tile = valid;
    for (size_t layer = 0; layer < tile.layers.size(); ++layer)
        tile.weights[layer * tile.SampleCount() + 4] = 0.1f;
    CHECK(ValidateTerrainWeightTile(tile) == TerrainWeightStatus::BadSum);

    CHECK(DescribeTerrainWeightStatus(TerrainWeightStatus::BadSum).find("sum") != std::string_view::npos);
}

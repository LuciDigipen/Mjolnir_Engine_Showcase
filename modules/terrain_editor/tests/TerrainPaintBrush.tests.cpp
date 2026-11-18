/**
 * @file TerrainPaintBrush.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain_editor/TerrainPaintBrush.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain_editor/TerrainPaintBrush.h>

#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainLayout.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using mir::GlobalSampleIndex;
    using mir::TerrainBrush;
    using mir::TerrainBrushSettings;
    using mir::TerrainEditLayer;
    using mir::TerrainLayout;
    using mir::SampleRange;
    using mir::TerrainLocalPosition;
    using mir::TerrainPaintStroke;
    using mir::TerrainTileOrder;
    using mir::TileCoord;
    using mir::TileRange;

    constexpr int32_t kIntervals = 4;
    constexpr int32_t kTilesPerSide = 2;
    constexpr double kRadius = 1.5;
    constexpr double kCenter = 4.0;
    constexpr double kStep = 0.25;
    // A dab at strength 0.5 that adds kStep at the brush center.
    constexpr double kDabSeconds = kStep / (0.5 * mir::kTerrainPaintRatePerSecond);

    TerrainLayout MakeLayout()
    {
        const std::optional<TerrainLayout> layout =
            TerrainLayout::Create(1.0, kIntervals, mir::TerrainLocalBounds{ 0.0, 0.0, 8.0, 8.0 });
        REQUIRE(layout);
        return *layout;
    }

    bool SameBits(float left, float right)
    {
        return std::bit_cast<uint32_t>(left) == std::bit_cast<uint32_t>(right);
    }

    std::optional<float> ChannelAt(const TerrainEditLayer &layer, const TerrainLayout &layout,
                                   GlobalSampleIndex sample, std::string_view name)
    {
        const int32_t intervals = layout.IntervalsPerTile();
        const TileRange range = layout.Tiles();
        const int64_t firstX = sample.x / intervals;
        const int64_t firstZ = sample.z / intervals;
        for (const int64_t x : { firstX, firstX - 1 })
        {
            for (const int64_t z : { firstZ, firstZ - 1 })
            {
                const TileCoord coord{ static_cast<int32_t>(x), static_cast<int32_t>(z) };
                if (x < range.min.x || x > range.max.x || z < range.min.z || z > range.max.z)
                    continue;
                const auto tile = layer.tiles.find(coord);
                if (tile == layer.tiles.end())
                    continue;
                const auto channel = std::find_if(tile->second.weights.begin(), tile->second.weights.end(),
                                                  [name](const mir::TerrainEditWeight &weight)
                                                  {
                                                      return weight.layer == name;
                                                  });
                if (channel == tile->second.weights.end())
                    continue;
                const int64_t localX = sample.x - x * intervals;
                const int64_t localZ = sample.z - z * intervals;
                if (localX < 0 || localZ < 0 || localX > intervals || localZ > intervals)
                    continue;
                const std::size_t index = static_cast<std::size_t>(localZ) * static_cast<std::size_t>(intervals + 1) +
                                          static_cast<std::size_t>(localX);
                if (index >= channel->values.size())
                    continue;
                return channel->values[index];
            }
        }
        return std::nullopt;
    }

    void SeedChannel(TerrainEditLayer &layer, const TerrainLayout &layout, std::string_view name, float value)
    {
        for (const TileCoord &tile : { TileCoord{ 0, 0 }, TileCoord{ 1, 0 }, TileCoord{ 0, 1 }, TileCoord{ 1, 1 } })
        {
            const std::optional<SampleRange> samples = layout.TileSamples(tile);
            REQUIRE(samples);
            for (int64_t z = samples->min.z; z <= samples->max.z; ++z)
            {
                for (int64_t x = samples->min.x; x <= samples->max.x; ++x)
                {
                    const GlobalSampleIndex sample{ x, z };
                    if (layout.IsPaddingSample(sample))
                        continue;
                    mir::SetTerrainEditWeightSample(layer, layout, sample, name, value);
                }
            }
        }
    }

    struct Surface
    {
        Surface()
        {
            layer.info.name = "Paint";
            layer.info.sequence = 1;
            layer.info.enabled = true;
            SeedChannel(layer, layout, "base", 0.25f);
            SeedChannel(layer, layout, "sand", 0.75f);
        }

        TerrainLayout layout = MakeLayout();
        TerrainEditLayer layer;
    };

    TerrainBrushSettings Settings(bool erase = false)
    {
        TerrainBrushSettings settings;
        settings.brush = TerrainBrush::Paint;
        settings.radius = kRadius;
        settings.strength = 0.5;
        settings.falloff = 0.0;
        settings.lower = erase;
        return settings;
    }

    TerrainLocalPosition Center()
    {
        const std::optional<TerrainLocalPosition> center =
            TerrainLocalPosition::FromMeters(glm::dvec3(kCenter, 0.0, kCenter));
        REQUIRE(center);
        return *center;
    }
}

TEST_CASE("A paint dab raises the chosen channel and fades the same layer's other channels",
    "[terrain_editor][brush][paint]")
{
    Surface surface;
    TerrainPaintStroke stroke(surface.layer, surface.layout, "sand");

    REQUIRE(stroke.Dab(Center(), Settings(), kDabSeconds).Ok());
    CHECK_FALSE(stroke.Empty());

    const GlobalSampleIndex center{ 4, 4 };
    const std::optional<float> sand = ChannelAt(surface.layer, surface.layout, center, "sand");
    const std::optional<float> base = ChannelAt(surface.layer, surface.layout, center, "base");
    REQUIRE(sand);
    REQUIRE(base);
    CHECK(*sand == Catch::Approx(0.75 + kStep * 0.25).margin(1e-6));
    CHECK(*base == Catch::Approx(0.25 * (1.0 - kStep)).margin(1e-6));

    const bool anyHeight = std::any_of(surface.layer.tiles.begin(), surface.layer.tiles.end(),
                                       [](const auto &entry) { return !entry.second.values.empty(); });
    CHECK_FALSE(anyHeight);
}

TEST_CASE("An erasing dab lowers only the chosen channel", "[terrain_editor][brush][paint]")
{
    Surface surface;
    TerrainPaintStroke stroke(surface.layer, surface.layout, "sand");

    REQUIRE(stroke.Dab(Center(), Settings(true), kDabSeconds).Ok());

    const GlobalSampleIndex center{ 4, 4 };
    const std::optional<float> sand = ChannelAt(surface.layer, surface.layout, center, "sand");
    const std::optional<float> base = ChannelAt(surface.layer, surface.layout, center, "base");
    REQUIRE(sand);
    REQUIRE(base);
    CHECK(*sand == Catch::Approx(0.75 * (1.0 - kStep)).margin(1e-6));
    CHECK(*base == Catch::Approx(0.25).margin(1e-6));
}

TEST_CASE("Samples outside the radius keep their bits and shared edges agree",
    "[terrain_editor][brush][paint]")
{
    Surface surface;
    const TerrainEditLayer before = surface.layer;
    TerrainPaintStroke stroke(surface.layer, surface.layout, "sand");
    REQUIRE(stroke.Dab(Center(), Settings(), kDabSeconds).Ok());

    for (const TileCoord &tile : { TileCoord{ 0, 0 }, TileCoord{ 1, 0 }, TileCoord{ 0, 1 }, TileCoord{ 1, 1 } })
    {
        const std::optional<SampleRange> samples = surface.layout.TileSamples(tile);
        REQUIRE(samples);
        for (int64_t z = samples->min.z; z <= samples->max.z; ++z)
        {
            for (int64_t x = samples->min.x; x <= samples->max.x; ++x)
            {
                const GlobalSampleIndex sample{ x, z };
                if (surface.layout.IsPaddingSample(sample))
                    continue;
                const double dx = surface.layout.SamplePosition(x) - kCenter;
                const double dz = surface.layout.SamplePosition(z) - kCenter;
                if (dx * dx + dz * dz <= kRadius * kRadius)
                    continue;
                for (const std::string_view name : { "base", "sand" })
                {
                    const std::optional<float> oldValue = ChannelAt(before, surface.layout, sample, name);
                    const std::optional<float> newValue = ChannelAt(surface.layer, surface.layout, sample, name);
                    REQUIRE(oldValue);
                    REQUIRE(newValue);
                    CHECK(SameBits(*oldValue, *newValue));
                }
            }
        }
    }

    const GlobalSampleIndex edge{ 4, 4 };
    for (const std::string_view name : { "base", "sand" })
    {
        std::optional<float> expected;
        for (const TileCoord &tile : { TileCoord{ 0, 0 }, TileCoord{ 1, 0 }, TileCoord{ 0, 1 }, TileCoord{ 1, 1 } })
        {
            const auto found = surface.layer.tiles.find(tile);
            REQUIRE(found != surface.layer.tiles.end());
            const auto channel = std::find_if(found->second.weights.begin(), found->second.weights.end(),
                                              [name](const mir::TerrainEditWeight &weight)
                                              {
                                                  return weight.layer == name;
                                              });
            REQUIRE(channel != found->second.weights.end());
            const int64_t localX = edge.x - static_cast<int64_t>(tile.x) * kIntervals;
            const int64_t localZ = edge.z - static_cast<int64_t>(tile.z) * kIntervals;
            const std::size_t index = static_cast<std::size_t>(localZ) * static_cast<std::size_t>(kIntervals + 1) +
                                      static_cast<std::size_t>(localX);
            REQUIRE(index < channel->values.size());
            if (!expected)
                expected = channel->values[index];
            else
                CHECK(SameBits(*expected, channel->values[index]));
        }
        REQUIRE(expected);
    }
}

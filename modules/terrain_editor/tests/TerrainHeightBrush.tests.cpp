/**
 * @file TerrainHeightBrush.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain_editor/TerrainHeightBrush.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain_editor/TerrainHeightBrush.h>

#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TileHeights.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace
{
    using mir::CompositeTerrainEditLayers;
    using mir::GlobalSampleIndex;
    using mir::TerrainBaseHeightsErrorCode;
    using mir::TerrainBaseHeightsResult;
    using mir::TerrainBaseTile;
    using mir::TerrainBrush;
    using mir::TerrainBrushSettings;
    using mir::TerrainBrushStroke;
    using mir::TerrainBrushTileSource;
    using mir::TerrainEditBlend;
    using mir::TerrainEditLayer;
    using mir::TerrainLayout;
    using mir::TerrainLocalPosition;
    using mir::TerrainResult;
    using mir::TerrainTileOrder;
    using mir::TileCoord;
    using mir::TileHeights;
    using mir::ValidateTerrainEditSharedSamples;

    constexpr int32_t kIntervals = 4;
    constexpr int32_t kTilesPerSide = 2;
    constexpr double kRadius = 1.5;
    constexpr double kCenter = 4.0;

    TerrainLayout MakeLayout()
    {
        const std::optional<TerrainLayout> layout =
            TerrainLayout::Create(1.0, kIntervals, mir::TerrainLocalBounds{ 0.0, 0.0, 8.0, 8.0 });
        REQUIRE(layout);
        return *layout;
    }

    float BaseHeight(int64_t x, int64_t z)
    {
        return 2.0f + (x == 4 && z == 4 ? 4.0f : 0.0f);
    }

    struct Surface
    {
        Surface() : layout(MakeLayout())
        {
            for (int32_t z = 0; z < kTilesPerSide; ++z)
            {
                for (int32_t x = 0; x < kTilesPerSide; ++x)
                {
                    const TileCoord coord{ x, z };
                    std::optional<TileHeights> tile = TileHeights::Create(coord, kIntervals);
                    REQUIRE(tile);
                    for (int32_t k = 0; k <= kIntervals; ++k)
                    {
                        for (int32_t i = 0; i <= kIntervals; ++i)
                            tile->Set(i, k, BaseHeight(x * kIntervals + i, z * kIntervals + k));
                    }
                    base.emplace(coord, std::make_shared<const TileHeights>(std::move(*tile)));
                }
            }
        }

        TerrainBrushTileSource Source() const
        {
            return TerrainBrushTileSource{ layout, [this](TileCoord) { return std::optional<TerrainLayout>(layout); },
                [this](TileCoord tile) -> TerrainBaseHeightsResult<TerrainBaseTile> {
                    const auto found = base.find(tile);
                    if (found == base.end())
                        return { std::nullopt, { TerrainBaseHeightsErrorCode::MissingTile, "missing synthetic tile" } };
                    return { found->second, {} };
                } };
        }

        TerrainLayout layout;
        std::map<TileCoord, TerrainBaseTile, TerrainTileOrder> base;
    };

    TerrainEditLayer MakeLayer(TerrainEditBlend blend)
    {
        TerrainEditLayer layer;
        layer.info.name = "Brush";
        layer.info.priority = 0;
        layer.info.sequence = 1;
        layer.info.enabled = true;
        layer.info.heightBlend = blend;
        return layer;
    }

    bool SameBits(float left, float right)
    {
        return std::bit_cast<uint32_t>(left) == std::bit_cast<uint32_t>(right);
    }

    float SampleAt(const TileHeights &tile, int64_t x, int64_t z)
    {
        const int32_t localX = static_cast<int32_t>(x - static_cast<int64_t>(tile.Tile().x) * kIntervals);
        const int32_t localZ = static_cast<int32_t>(z - static_cast<int64_t>(tile.Tile().z) * kIntervals);
        return tile.At(localX, localZ);
    }

    void CheckBrush(TerrainBrush brush, TerrainEditBlend blend)
    {
        Surface surface;
        std::vector<TerrainEditLayer> layers{ MakeLayer(blend) };
        TerrainEditLayer &target = layers.front();
        const std::span<const TerrainEditLayer> stack(layers);

        TerrainBrushStroke stroke(target, stack, surface.Source(), 8.0);
        TerrainBrushSettings settings;
        settings.brush = brush;
        settings.radius = kRadius;
        settings.strength = 0.5;
        settings.falloff = 0.5;
        for (int dab = 0; dab < 4; ++dab)
        {
            const std::optional<TerrainLocalPosition> center =
                TerrainLocalPosition::FromMeters(glm::dvec3(kCenter, 0.0, kCenter));
            REQUIRE(center);
            const mir::TerrainBrushDabResult result = stroke.Dab(*center, settings, 0.25);
            REQUIRE(result.Ok());
            if (dab == 0)
                CHECK_FALSE(result.changed.empty());
        }
        CHECK_FALSE(stroke.Empty());
        CHECK(ValidateTerrainEditSharedSamples(target, surface.layout).empty());

        const float initialCenter = BaseHeight(4, 4);
        float center = initialCenter;
        for (const auto &[coord, base] : surface.base)
        {
            const TerrainResult<TileHeights> composite = CompositeTerrainEditLayers(*base, stack);
            REQUIRE(composite);
            for (int32_t k = 0; k <= kIntervals; ++k)
            {
                for (int32_t i = 0; i <= kIntervals; ++i)
                {
                    const int64_t x = static_cast<int64_t>(coord.x) * kIntervals + i;
                    const int64_t z = static_cast<int64_t>(coord.z) * kIntervals + k;
                    const float height = composite.value->At(i, k);
                    const double dx = surface.layout.SamplePosition(x) - kCenter;
                    const double dz = surface.layout.SamplePosition(z) - kCenter;
                    if (dx * dx + dz * dz > kRadius * kRadius)
                    {
                        CHECK(SameBits(height, base->At(i, k)));
                        continue;
                    }
                    if (x == 4 && z == 4)
                        center = height;
                }
            }
        }

        switch (brush)
        {
        case TerrainBrush::RaiseLower:
            CHECK(center > initialCenter);
            break;
        case TerrainBrush::Smooth:
            CHECK(center < initialCenter);
            break;
        case TerrainBrush::Flatten:
            CHECK(std::abs(center - 8.0f) < std::abs(initialCenter - 8.0f));
            break;
        case TerrainBrush::Paint:
            break;
        }

        if (blend == TerrainEditBlend::AlphaBlend)
        {
            bool anyAlpha = false;
            for (const auto &[coord, tile] : target.tiles)
            {
                (void)coord;
                anyAlpha = anyAlpha || !tile.alpha.empty();
            }
            CHECK(anyAlpha);
        }
    }
}

TEST_CASE("A height brush moves the composite inside its radius on both layer blends",
    "[terrain_editor][brush]")
{
    for (const TerrainEditBlend blend : { TerrainEditBlend::Additive, TerrainEditBlend::AlphaBlend })
    {
        CheckBrush(TerrainBrush::RaiseLower, blend);
        CheckBrush(TerrainBrush::Smooth, blend);
        CheckBrush(TerrainBrush::Flatten, blend);
    }
}

namespace
{
    float CenterComposite(const Surface &surface, std::span<const TerrainEditLayer> stack)
    {
        const TerrainResult<TileHeights> composite =
            CompositeTerrainEditLayers(*surface.base.at(TileCoord{ 0, 0 }), stack);
        REQUIRE(composite);
        return composite.value->At(4, 4);
    }

    void CheckDabStep(TerrainBrush brush, TerrainEditBlend blend, bool lower, double seconds, double expected)
    {
        Surface surface;
        std::vector<TerrainEditLayer> layers{ MakeLayer(blend) };
        const std::span<const TerrainEditLayer> stack(layers);

        TerrainBrushStroke stroke(layers.front(), stack, surface.Source(), 8.0);
        TerrainBrushSettings settings;
        settings.brush = brush;
        settings.radius = kRadius;
        settings.strength = 0.5;
        settings.falloff = 0.5;
        settings.lower = lower;

        const std::optional<TerrainLocalPosition> center =
            TerrainLocalPosition::FromMeters(glm::dvec3(kCenter, 0.0, kCenter));
        REQUIRE(center);
        const float before = CenterComposite(surface, stack);
        REQUIRE(stroke.Dab(*center, settings, seconds).Ok());
        const float after = CenterComposite(surface, stack);
        CHECK(after == Catch::Approx(static_cast<float>(before + expected)).margin(1e-4));
    }
}

TEST_CASE("One dab moves the composite by the brush step on both blends", "[terrain_editor][brush]")
{
    for (const TerrainEditBlend blend : { TerrainEditBlend::Additive, TerrainEditBlend::AlphaBlend })
    {
        CheckDabStep(TerrainBrush::RaiseLower, blend, false, 0.25, 0.125);
        CheckDabStep(TerrainBrush::RaiseLower, blend, true, 0.25, -0.125);
        CheckDabStep(TerrainBrush::Smooth, blend, false, 0.25, -0.125);
        CheckDabStep(TerrainBrush::Flatten, blend, false, 0.25, 0.125);
        CheckDabStep(TerrainBrush::Smooth, blend, false, 10.0, -4.0);
        CheckDabStep(TerrainBrush::Flatten, blend, false, 10.0, 2.0);
    }
}

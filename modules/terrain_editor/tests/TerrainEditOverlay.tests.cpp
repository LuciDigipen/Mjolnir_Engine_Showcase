/**
 * @file TerrainEditOverlay.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain_editor/TerrainEditOverlay and the brush availability rule.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain_editor/TerrainEditBase.h>
#include <terrain_editor/TerrainEditOverlay.h>

#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainOverlay.h>
#include <terrain/TileHeights.h>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace
{
    using mir::AssetId;
    using mir::CompositeTerrainEditLayers;
    using mir::EvaluateTerrainBrushAvailability;
    using mir::TerrainBaseCache;
    using mir::TerrainBaseHeightsErrorCode;
    using mir::TerrainBaseHeightsResult;
    using mir::TerrainBaseTile;
    using mir::TerrainEditBlend;
    using mir::TerrainEditLayer;
    using mir::TerrainEditOverlaySession;
    using mir::TerrainLayout;
    using mir::TerrainOverlay;
    using mir::TerrainTileOrder;
    using mir::TileCoord;
    using mir::TileHeights;

    constexpr int32_t kIntervals = 4;
    constexpr AssetId kAsset{ 0x1234 };

    TerrainLayout MakeLayout()
    {
        const std::optional<TerrainLayout> layout =
            TerrainLayout::Create(1.0, kIntervals, mir::TerrainLocalBounds{ 0.0, 0.0, 8.0, 4.0 });
        REQUIRE(layout);
        return *layout;
    }

    TerrainBaseTile MakeBaseTile(TileCoord coord)
    {
        std::optional<TileHeights> tile = TileHeights::Create(coord, kIntervals);
        REQUIRE(tile);
        for (int32_t k = 0; k <= kIntervals; ++k)
        {
            for (int32_t i = 0; i <= kIntervals; ++i)
            {
                const float x = static_cast<float>(coord.x * kIntervals + i);
                const float z = static_cast<float>(coord.z * kIntervals + k);
                tile->Set(i, k, 3.0f + 0.5f * x + 0.25f * z);
            }
        }
        return std::make_shared<const TileHeights>(std::move(*tile));
    }

    struct Surface
    {
        Surface() : layout(MakeLayout())
        {
            for (const TileCoord &coord : { TileCoord{ 0, 0 }, TileCoord{ 1, 0 } })
                base.emplace(coord, MakeBaseTile(coord));
        }

        std::map<TileCoord, TerrainBaseTile, TerrainTileOrder> base;
        TerrainLayout layout;
    };

    bool SameBits(const TileHeights &left, const TileHeights &right)
    {
        if (left.Tile() != right.Tile() || left.IntervalsPerTile() != right.IntervalsPerTile())
            return false;
        const std::span<const float> a = left.Samples();
        const std::span<const float> b = right.Samples();
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::bit_cast<uint32_t>(a[i]) != std::bit_cast<uint32_t>(b[i]))
                return false;
        return true;
    }

    std::filesystem::path MakeScratchDir()
    {
        static std::random_device random;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "mir_terrain_editor_tests" / ("overlay_" + std::to_string(random()));
        std::filesystem::remove_all(dir);
        return dir;
    }
}

TEST_CASE("An overlay snapshot holds the session's edited tiles composited from the base and stack",
    "[terrain_editor][overlay]")
{
    Surface surface;
    TerrainEditOverlaySession session;
    session.MarkEdited(TileCoord{ 0, 0 });
    session.MarkEdited(TileCoord{ 1, 0 });

    const auto base = [&surface](TileCoord tile) -> TerrainBaseHeightsResult<TerrainBaseTile> {
        const auto found = surface.base.find(tile);
        if (found == surface.base.end())
            return { std::nullopt, { TerrainBaseHeightsErrorCode::MissingTile, "missing tile" } };
        return { found->second, {} };
    };

    TerrainEditLayer layer;
    layer.info.name = "Edits";
    layer.info.sequence = 1;
    layer.info.enabled = true;
    layer.info.heightBlend = TerrainEditBlend::Additive;
    const std::vector<TerrainEditLayer> layersBefore{ layer };

    std::string reason;
    std::shared_ptr<const TerrainOverlay> first = session.Build(kAsset, layersBefore, base, reason);
    REQUIRE(first);
    CHECK(reason.empty());
    CHECK(first->Asset() == kAsset);
    CHECK(first->Revision() == 1);
    CHECK(first->Tiles().size() == 2);
    for (const TileCoord &tile : session.EditedTiles())
    {
        const std::shared_ptr<const TileHeights> *found = first->Find(tile);
        REQUIRE(found != nullptr);
        const auto composite = CompositeTerrainEditLayers(*surface.base.at(tile), layersBefore);
        REQUIRE(composite);
        CHECK(SameBits(**found, *composite.value));
    }

    mir::TerrainEditTile edit;
    edit.tile = TileCoord{ 0, 0 };
    edit.intervals = kIntervals;
    edit.values.assign(static_cast<std::size_t>(kIntervals + 1) * static_cast<std::size_t>(kIntervals + 1), 1.0f);
    layer.tiles.emplace(edit.tile, std::move(edit));
    std::vector<TerrainEditLayer> layersAfter{ layer };
    std::shared_ptr<const TerrainOverlay> second = session.Build(kAsset, layersAfter, base, reason);
    REQUIRE(second);
    CHECK(second->Revision() > first->Revision());
    for (const TileCoord &tile : session.EditedTiles())
    {
        const std::shared_ptr<const TileHeights> *found = second->Find(tile);
        REQUIRE(found != nullptr);
        const auto composite = CompositeTerrainEditLayers(*surface.base.at(tile), layersAfter);
        REQUIRE(composite);
        CHECK(SameBits(**found, *composite.value));
    }

    std::vector<TerrainEditLayer> layersUndone = layersBefore;
    std::shared_ptr<const TerrainOverlay> undone = session.Build(kAsset, layersUndone, base, reason);
    REQUIRE(undone);
    CHECK(undone->Revision() > second->Revision());
    for (const TileCoord &tile : session.EditedTiles())
    {
        const std::shared_ptr<const TileHeights> *found = undone->Find(tile);
        REQUIRE(found != nullptr);
        const auto composite = CompositeTerrainEditLayers(*surface.base.at(tile), layersUndone);
        REQUIRE(composite);
        CHECK(SameBits(**found, *composite.value));
    }
}

TEST_CASE("A missing base cache disables the brush and applies no overlay", "[terrain_editor][overlay]")
{
    const std::filesystem::path dir = MakeScratchDir();
    TerrainBaseCache cache;
    CHECK_FALSE(cache.Ensure(dir, kAsset));
    CHECK_FALSE(cache.Ready());
    CHECK_FALSE(cache.Error().empty());

    const mir::TerrainBrushAvailability availability =
        EvaluateTerrainBrushAvailability(true, true, true, true, true, cache.Ready(), cache.Error());
    CHECK_FALSE(availability.enabled);
    CHECK(availability.message.find("TerrainTool sync") != std::string::npos);

    TerrainEditOverlaySession session;
    session.MarkEdited(TileCoord{ 0, 0 });
    std::string reason;
    TerrainEditLayer layer;
    layer.info.sequence = 1;
    const std::vector<TerrainEditLayer> layers{ layer };
    CHECK(session.Build(kAsset, layers, cache.Provider(), reason) == nullptr);
    CHECK_FALSE(reason.empty());
    CHECK(session.Revision() == 0);

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

TEST_CASE("A weight-only tile produces no overlay tile", "[terrain_editor][overlay][weights]")
{
    Surface surface;
    TerrainEditOverlaySession session;
    session.MarkEdited(TileCoord{ 0, 0 });
    session.MarkEdited(TileCoord{ 1, 0 });

    const auto base = [&surface](TileCoord tile) -> TerrainBaseHeightsResult<TerrainBaseTile> {
        const auto found = surface.base.find(tile);
        if (found == surface.base.end())
            return { std::nullopt, { TerrainBaseHeightsErrorCode::MissingTile, "missing tile" } };
        return { found->second, {} };
    };

    TerrainEditLayer layer;
    layer.info.name = "Weights";
    layer.info.sequence = 1;
    layer.info.enabled = true;

    mir::TerrainEditTile weightTile;
    weightTile.tile = TileCoord{ 0, 0 };
    weightTile.intervals = kIntervals;
    weightTile.weights.push_back(
        { "sand", std::vector<float>(static_cast<std::size_t>(kIntervals + 1) * static_cast<std::size_t>(kIntervals + 1),
                                     0.5f) });
    layer.tiles.emplace(weightTile.tile, weightTile);

    mir::TerrainEditTile heightTile;
    heightTile.tile = TileCoord{ 1, 0 };
    heightTile.intervals = kIntervals;
    heightTile.values.assign(
        static_cast<std::size_t>(kIntervals + 1) * static_cast<std::size_t>(kIntervals + 1), 1.0f);
    layer.tiles.emplace(heightTile.tile, heightTile);

    const std::vector<TerrainEditLayer> layers{ layer };
    std::string reason;
    const std::shared_ptr<const TerrainOverlay> overlay = session.Build(kAsset, layers, base, reason);
    REQUIRE(overlay);
    REQUIRE(overlay->Tiles().size() == 1);
    CHECK(overlay->Find(TileCoord{ 0, 0 }) == nullptr);
    REQUIRE(overlay->Find(TileCoord{ 1, 0 }) != nullptr);
}

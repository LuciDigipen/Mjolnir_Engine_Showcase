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

#include <core/fs/FileIO.h>
#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainOverlay.h>
#include <terrain/TerrainPackWriter.h>
#include <terrain/TerrainWeights.h>
#include <terrain/TileHeights.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
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

namespace
{
    using mir::CompositeTerrainEditWeights;
    using mir::EncodeTerrainWeightPayload;
    using mir::EvaluateTerrainPaintAvailability;
    using mir::TerrainBaseWeightTile;
    using mir::TerrainBaseWeightsCache;
    using mir::TerrainBaseWeightsErrorCode;
    using mir::TerrainBaseWeightsResult;
    using mir::TerrainDecodedWeights;
    using mir::TerrainEditTile;
    using mir::TerrainEditWeight;
    using mir::TerrainWeightTile;

    const std::vector<std::string> kWeightTable{ "base", "sand" };

    TerrainWeightTile MakeBaseWeights(TileCoord coord)
    {
        TerrainWeightTile tile;
        tile.tile = coord;
        tile.intervals = kIntervals;
        tile.layers = kWeightTable;
        const std::size_t count = tile.SampleCount();
        tile.weights.assign(count * tile.layers.size(), 0.0f);
        for (std::size_t sample = 0; sample < count; ++sample)
        {
            tile.weights[sample] = 0.7f;
            tile.weights[count + sample] = 0.3f;
        }
        return tile;
    }

    TerrainEditTile WeightEdit(TileCoord coord, float sand)
    {
        TerrainEditTile tile;
        tile.tile = coord;
        tile.intervals = kIntervals;
        const std::size_t count = static_cast<std::size_t>(kIntervals + 1) * static_cast<std::size_t>(kIntervals + 1);
        tile.weights.push_back(TerrainEditWeight{ "sand", std::vector<float>(count, sand) });
        return tile;
    }

    TerrainEditTile HeightEdit(TileCoord coord)
    {
        TerrainEditTile tile;
        tile.tile = coord;
        tile.intervals = kIntervals;
        tile.values.assign(static_cast<std::size_t>(kIntervals + 1) * static_cast<std::size_t>(kIntervals + 1), 1.0f);
        return tile;
    }

    mir::TerrainResult<mir::TerrainEncodedWeights> ExpectedEncoded(std::span<const TerrainEditLayer> layers,
                                                                  TileCoord tile)
    {
        const TerrainWeightTile baseTile = MakeBaseWeights(tile);
        const mir::TerrainResult<mir::TerrainEditWeightComposite> composite =
            CompositeTerrainEditWeights(baseTile, layers);
        if (!composite)
            return { std::nullopt, composite.error };
        return EncodeTerrainWeightPayload(composite.value->tile, kWeightTable);
    }
}

TEST_CASE("The overlay builds weight tiles for weight edits and keeps unchanged payloads",
    "[terrain_editor][overlay][weights]")
{
    Surface surface;
    TerrainEditOverlaySession session;
    session.MarkWeightEdited(TileCoord{ 0, 0 });
    session.MarkEdited(TileCoord{ 1, 0 });

    const auto base = [&surface](TileCoord tile) -> TerrainBaseHeightsResult<TerrainBaseTile> {
        const auto found = surface.base.find(tile);
        if (found == surface.base.end())
            return { std::nullopt, { TerrainBaseHeightsErrorCode::MissingTile, "missing tile" } };
        return { found->second, {} };
    };
    std::map<TileCoord, TerrainBaseWeightTile, TerrainTileOrder> ruleTiles{
        { TileCoord{ 0, 0 }, std::make_shared<const TerrainWeightTile>(MakeBaseWeights({ 0, 0 })) },
        { TileCoord{ 1, 0 }, std::make_shared<const TerrainWeightTile>(MakeBaseWeights({ 1, 0 })) },
    };
    const auto weights = [&ruleTiles](TileCoord tile) -> TerrainBaseWeightsResult<TerrainBaseWeightTile> {
        const auto found = ruleTiles.find(tile);
        if (found == ruleTiles.end())
            return { std::nullopt, { TerrainBaseWeightsErrorCode::MissingTile, "missing rule weights" } };
        return { found->second, {} };
    };

    TerrainEditLayer layer;
    layer.info.name = "Paint";
    layer.info.sequence = 1;
    layer.info.enabled = true;
    layer.tiles.emplace(TileCoord{ 0, 0 }, WeightEdit({ 0, 0 }, 0.5f));
    layer.tiles.emplace(TileCoord{ 1, 0 }, HeightEdit({ 1, 0 }));

    std::vector<TerrainEditLayer> layers{ layer };
    std::string reason;
    std::shared_ptr<const TerrainOverlay> first = session.Build(kAsset, layers, base, weights, kWeightTable,
        kWeightTable, 77, reason);
    REQUIRE(first);
    CHECK(reason.empty());
    CHECK(first->Tiles().size() == 1);
    CHECK(first->Find(TileCoord{ 0, 0 }) == nullptr);
    REQUIRE(first->Find(TileCoord{ 1, 0 }) != nullptr);
    REQUIRE(first->WeightTiles().size() == 1);
    CHECK(first->FindWeights(TileCoord{ 0, 0 }) != nullptr);

    const mir::TerrainResult<mir::TerrainEncodedWeights> encodedZero =
        ExpectedEncoded(layers, TileCoord{ 0, 0 });
    REQUIRE(encodedZero);
    const std::shared_ptr<const TerrainDecodedWeights> *zero = first->FindWeights(TileCoord{ 0, 0 });
    REQUIRE(zero);
    CHECK((*zero)->tile == TileCoord{ 0, 0 });
    CHECK((*zero)->slots == encodedZero.value->slots);
    CHECK((*zero)->bytes == encodedZero.value->bytes);

    std::shared_ptr<const TerrainOverlay> unchanged = session.Build(kAsset, layers, base, weights, kWeightTable,
        kWeightTable, 77, reason);
    REQUIRE(unchanged);
    CHECK(unchanged->FindWeights(TileCoord{ 0, 0 })->get() == (*zero).get());

    session.MarkWeightEdited(TileCoord{ 1, 0 });
    TerrainEditTile paintedTile = WeightEdit({ 1, 0 }, 0.75f);
    paintedTile.values = HeightEdit({ 1, 0 }).values;
    layer.tiles[TileCoord{ 1, 0 }] = std::move(paintedTile);
    layers = std::vector<TerrainEditLayer>{ layer };
    std::shared_ptr<const TerrainOverlay> painted = session.Build(kAsset, layers, base, weights, kWeightTable,
        kWeightTable, 77, reason);
    REQUIRE(painted);
    const std::shared_ptr<const TerrainDecodedWeights> *paintedZero = painted->FindWeights(TileCoord{ 0, 0 });
    REQUIRE(paintedZero);
    CHECK(paintedZero->get() == (*zero).get());
    const std::shared_ptr<const TerrainDecodedWeights> *paintedOne = painted->FindWeights(TileCoord{ 1, 0 });
    REQUIRE(paintedOne);
    const mir::TerrainResult<mir::TerrainEncodedWeights> encodedOne =
        ExpectedEncoded(layers, TileCoord{ 1, 0 });
    REQUIRE(encodedOne);
    CHECK((*paintedOne)->slots == encodedOne.value->slots);
    CHECK((*paintedOne)->bytes == encodedOne.value->bytes);

    TerrainEditLayer undoneLayer = layer;
    undoneLayer.tiles.clear();
    undoneLayer.tiles.emplace(TileCoord{ 0, 0 }, WeightEdit({ 0, 0 }, 0.5f));
    undoneLayer.tiles.emplace(TileCoord{ 1, 0 }, HeightEdit({ 1, 0 }));
    layers = std::vector<TerrainEditLayer>{ undoneLayer };
    std::shared_ptr<const TerrainOverlay> undone = session.Build(kAsset, layers, base, weights, kWeightTable,
        kWeightTable, 77, reason);
    REQUIRE(undone);
    CHECK(undone->FindWeights(TileCoord{ 0, 0 })->get() == (*zero).get());
    CHECK(undone->FindWeights(TileCoord{ 1, 0 })->get() != paintedOne->get());

    std::shared_ptr<const TerrainOverlay> stable = session.Build(kAsset, layers, base, weights, kWeightTable,
        kWeightTable, 77, reason);
    REQUIRE(stable);
    CHECK(stable->FindWeights(TileCoord{ 1, 0 })->get() == undone->FindWeights(TileCoord{ 1, 0 })->get());

    const std::vector<std::string> otherTable{ "base", "clay" };
    CHECK(session.Build(kAsset, layers, base, weights, kWeightTable, otherTable, 77, reason) == nullptr);
    CHECK(reason.find("layer table") != std::string::npos);
}

TEST_CASE("Open editor caches follow their own hex pointers when sync publishes",
    "[terrain_editor][overlay][weights]")
{
    using namespace mir;
    namespace fs = std::filesystem;
    const auto root = MakeScratchDir();
    struct Cleanup
    {
        fs::path root;
        ~Cleanup() { std::error_code ec; fs::remove_all(root, ec); }
    } cleanup{root};

    TerrainBaseCache heights;
    TerrainBaseWeightsCache weights;
    CHECK_FALSE(heights.Ensure(root, kAsset));
    CHECK_FALSE(weights.Ensure(root, kAsset));

    const Surface surface;
    const TerrainSourceFingerprint heightKey(1);
    const TerrainReleaseInfo info{kAsset, heightKey, kTerrainFormatVersion, TerrainReleaseId(heightKey.value)};
    const std::array tiles{TerrainTileWrite{surface.base.at({0, 0}).get()},
        TerrainTileWrite{surface.base.at({1, 0}).get()}};
    const auto heightsRelease = EncodeTerrainRelease(info, std::span(&surface.layout, 1), tiles);
    REQUIRE(heightsRelease);
    const auto heightDir = TerrainBaseHeightsDir(root, kAsset, heightKey);
    fs::create_directories(heightDir);
    REQUIRE(WriteBytes(heightDir / kTerrainIndexFileName, heightsRelease.value->index));
    for (const auto &pack : heightsRelease.value->packs)
        REQUIRE(WriteBytes(heightDir / pack.fileName, pack.bytes));
    REQUIRE(WriteText(TerrainBaseHeightsPointerPath(root, kAsset), TerrainIdHex(heightKey.value)));
    REQUIRE(heights.Ensure(root, kAsset));
    CHECK(heights.KeyValue() == heightKey.value);
    CHECK_FALSE(weights.Ensure(root, kAsset));

    const auto publishWeights = [&](TerrainSourceFingerprint key) {
        const auto dir = TerrainBaseWeightsDir(root, kAsset, key);
        const std::array tiles{MakeBaseWeights({0, 0}), MakeBaseWeights({1, 0})};
        const auto encoded = EncodeTerrainBaseWeights(kAsset, key, surface.layout, kWeightTable, tiles);
        REQUIRE(encoded);
        fs::create_directories(dir);
        REQUIRE(WriteBytes(dir / kTerrainBaseWeightsIndexFileName, encoded.value->index));
        REQUIRE(WriteBytes(dir / kTerrainBaseWeightsPackFileName, encoded.value->pack));
        REQUIRE(WriteTextAtomic(TerrainBaseWeightsPointerPath(root, kAsset), TerrainIdHex(key.value)));
        REQUIRE(weights.Ensure(root, kAsset));
        CHECK(weights.KeyValue() == key.value);
        const auto tile = weights.ReadTile({0, 0});
        REQUIRE(tile);
        return *tile.value;
    };
    const auto first = publishWeights(TerrainSourceFingerprint(2));
    const auto second = publishWeights(TerrainSourceFingerprint(3));
    CHECK(first != second);
    REQUIRE(heights.Ensure(root, kAsset));
    CHECK(heights.KeyValue() == heightKey.value);
}

TEST_CASE("Paint availability disables each missing prerequisite with its message",
    "[terrain_editor][overlay][paint]")
{
    CHECK_FALSE(EvaluateTerrainPaintAvailability(false, true, true, true, true, true, "", true).enabled);
    CHECK(EvaluateTerrainPaintAvailability(true, false, true, true, true, true, "", true).message ==
          "Terrain brushes work in Edit.");
    CHECK(EvaluateTerrainPaintAvailability(true, true, false, true, true, true, "", true).message.find("Prefab") !=
          std::string::npos);
    CHECK_FALSE(EvaluateTerrainPaintAvailability(true, true, true, false, true, true, "", true).enabled);
    CHECK(EvaluateTerrainPaintAvailability(true, true, true, true, false, true, "", true).message.find("Edit Layer") !=
          std::string::npos);
    CHECK(EvaluateTerrainPaintAvailability(true, true, true, true, true, false, "", true).message.find(
              "material weights cache") != std::string::npos);
    CHECK(EvaluateTerrainPaintAvailability(true, true, true, true, true, true, "", false).message.find(
              "material layer") != std::string::npos);
    CHECK(EvaluateTerrainPaintAvailability(true, true, true, true, true, true, "", true).enabled);

    const std::filesystem::path dir = MakeScratchDir();
    TerrainBaseWeightsCache cache;
    CHECK_FALSE(cache.Ensure(dir, kAsset));
    CHECK_FALSE(cache.Ready());
    CHECK_FALSE(cache.Error().empty());
    const mir::TerrainBrushAvailability availability = EvaluateTerrainPaintAvailability(
        true, true, true, true, true, cache.Ready(), cache.Error(), true);
    CHECK_FALSE(availability.enabled);
    CHECK(availability.message.find("material weights cache") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

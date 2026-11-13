/**
 * @file TerrainOverlay.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainOverlay.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"
#include "TerrainStreamingTestSupport.h"

#include <terrain/TerrainHeightQuery.h>
#include <terrain/TerrainOverlay.h>
#include <terrain/TerrainOverlayHandover.h>
#include <terrain/TerrainScene.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using Status = TerrainHeightStatus;

    constexpr std::string_view kRecipe = "terrain/overlay.terrain.json";

    // 3 x 3 tiles of 256 m; the overlay holds the four tiles around the origin.
    TerrainLayout OverlayLayout()
    {
        const auto layout = TerrainLayout::CreateFromTileSize(256.0, 4.0, {-384.0, -384.0, 384.0, 384.0});
        REQUIRE(layout);
        return *layout;
    }

    TerrainLocalPosition Local(double x, double z)
    {
        return *TerrainLocalPosition::FromMeters({x, 0.0, z});
    }

    WorldPosition WorldAt(double x, double z)
    {
        return *WorldPosition::FromDoubleMeters({x, 0.0, z});
    }

    const TileCoord kOverlayTiles[]{TileCoord{-1, -1}, TileCoord{0, -1}, TileCoord{-1, 0}, TileCoord{0, 0}};

    template <typename Tiles> auto TileOf(Tiles &tiles, TileCoord coord) -> decltype(tiles.front())
    {
        const auto it = std::find_if(tiles.begin(), tiles.end(),
                                     [coord](const TileHeights &tile) { return tile.Tile() == coord; });
        REQUIRE(it != tiles.end());
        return *it;
    }

    // The base release, edited inside one tile, on both copies of a shared edge and at the four-tile corner.
    std::vector<TileHeights> EditedTiles(const TerrainLayout &layout)
    {
        std::vector<TileHeights> tiles = BuildTiles(layout);
        const int32_t n = layout.IntervalsPerTile();
        TileOf(tiles, {-1, -1}).Set(11, 7, TileOf(tiles, {-1, -1}).At(11, 7) + 2.5f);
        TileOf(tiles, {-1, -1}).Set(n, 5, 123.25f);
        TileOf(tiles, {0, -1}).Set(0, 5, 123.25f);
        TileOf(tiles, {-1, -1}).Set(n, n, 77.5f);
        TileOf(tiles, {0, -1}).Set(0, n, 77.5f);
        TileOf(tiles, {-1, 0}).Set(n, 0, 77.5f);
        TileOf(tiles, {0, 0}).Set(0, 0, 77.5f);
        return tiles;
    }

    std::shared_ptr<const TerrainOverlay> OverlayFrom(const std::vector<TileHeights> &tiles, AssetId asset,
                                                      uint64_t revision)
    {
        std::unordered_map<TileCoord, std::shared_ptr<const TileHeights>> held;
        for (const TileCoord coord : kOverlayTiles)
            held.emplace(coord, std::make_shared<const TileHeights>(TileOf(tiles, coord)));
        return std::make_shared<const TerrainOverlay>(asset, revision, std::move(held));
    }

    std::shared_ptr<const TerrainOverlay> SingleTileOverlay(TileHeights tile, AssetId asset, uint64_t revision)
    {
        std::unordered_map<TileCoord, std::shared_ptr<const TileHeights>> held;
        held.emplace(tile.Tile(), std::make_shared<const TileHeights>(std::move(tile)));
        return std::make_shared<const TerrainOverlay>(asset, revision, std::move(held));
    }

    struct Releases
    {
        TerrainReleaseInfo base;
        TerrainReleaseInfo edited;
        std::vector<TileHeights> baseTiles;
        std::vector<TileHeights> editedTiles;
        std::filesystem::path root;
    };

    Releases PublishReleases()
    {
        const TerrainLayout layout = OverlayLayout();
        Releases releases;
        releases.base = Info(TerrainAsset(kRecipe), 0xB0BA);
        releases.edited = Info(TerrainAsset(kRecipe), 0xED17);
        releases.baseTiles = BuildTiles(layout);
        releases.editedTiles = EditedTiles(layout);
        releases.root = MakeScratchDir("overlay");
        WriteRelease(releases.root, releases.base, Encode(releases.base, layout, releases.baseTiles));
        WriteRelease(releases.root, releases.edited, Encode(releases.edited, layout, releases.editedTiles));
        PointCurrentAt(releases.root, releases.base.asset, releases.base.release);
        return releases;
    }

    TerrainSceneSpec InstanceSpec(AssetId asset)
    {
        TerrainSceneSpec spec;
        spec.instances.push_back({std::string(kRecipe), std::string(kRecipe), asset, WorldAt(0.0, 0.0), kNullEntity});
        return spec;
    }

    struct Resolved
    {
        std::unique_ptr<TerrainStreamer> streamer;
        TerrainWorld world;
    };

    Resolved Resolve(const std::filesystem::path &root, AssetId asset, TerrainResidencyMode mode)
    {
        Resolved resolved;
        resolved.streamer = std::make_unique<TerrainStreamer>(StreamingSettings(uint64_t(1) << 30, mode),
                                                             std::make_shared<TerrainWorkerPool>(2));
        std::vector<TerrainIssue> issues;
        resolved.world =
            ResolveTerrainWorld(InstanceSpec(asset), root, nullptr, issues, *resolved.streamer, TerrainEagerLoad::Wait);
        if (!issues.empty())
            INFO(issues.front().message);
        REQUIRE(issues.empty());
        REQUIRE(resolved.world.Instances().size() == 1);
        return resolved;
    }

    TerrainHeightQuery PreloadedQuery(const std::filesystem::path &root, const TerrainReleaseInfo &info)
    {
        const TerrainReleaseHandle handle = OpenOk(OpenTerrainRelease(root, info.asset, info.release));
        TerrainHeightQuery query = TerrainHeightQuery::Create(handle);
        const TerrainPreloadReport report = PreloadAllTerrainTiles(query, handle);
        INFO(report.firstError.message);
        REQUIRE(report.Ok());
        return query;
    }

    void CheckSameAnswer(const TerrainHeightResult &actual, const TerrainHeightResult &expected)
    {
        const auto same = [](double a, double b) { return a == b || (std::isnan(a) && std::isnan(b)); };
        CHECK(actual.status == expected.status);
        CHECK(actual.failure == expected.failure);
        CHECK(actual.tile == expected.tile);
        CHECK(actual.layer == expected.layer);
        CHECK(same(actual.localHeight, expected.localHeight));
        CHECK(same(actual.spacing, expected.spacing));
        CHECK(same(actual.sourceSpacing, expected.sourceSpacing));
        CHECK(actual.maxError == expected.maxError);
        REQUIRE(actual.worldPoint.has_value() == expected.worldPoint.has_value());
        if (actual.worldPoint && expected.worldPoint)
            CHECK(actual.worldPoint->Meters() == expected.worldPoint->Meters());
    }

    // Tile interiors and edges, both sides of every shared edge between overlay tiles, the four-tile corner and
    // a sweep of the overlay's own bounds.
    std::vector<glm::dvec2> OverlayPoints()
    {
        constexpr double kInf = std::numeric_limits<double>::infinity();
        const double east = std::nextafter(0.0, kInf), west = std::nextafter(0.0, -kInf);
        std::vector<glm::dvec2> points{
            {-100.5, -80.25},
            {37.375, -201.5},
            {-13.25, 44.5},
            {99.5, 12.75},
            {-212.0, -228.0},
            {0.0, -236.0},
            {east, -236.0},
            {west, -236.0},
            {0.0, -100.5},
            {east, -100.5},
            {west, -100.5},
            {0.0, 0.0},
            {east, east},
            {west, west},
            {east, west},
            {west, east},
            {0.5, 0.5},
            {-0.5, -0.5},
            {-0.5, 0.5},
            {0.5, -0.5},
        };

        std::mt19937_64 random(0x0FEED);
        const auto unit = [&random] { return double(random() >> 11) * 0x1.0p-53; };
        for (int n = 0; n < 200; ++n)
            points.push_back({-256.0 + 512.0 * unit(), -256.0 + 512.0 * unit()});
        return points;
    }
}

TEST_CASE("Overlay tiles answer bit-identically to the edited release at every spacing", "[terrain][overlay]")
{
    const Releases releases = PublishReleases();
    Resolved resolved = Resolve(releases.root, releases.base.asset, TerrainResidencyMode::Eager);
    TerrainHeightQuery edited = PreloadedQuery(releases.root, releases.edited);

    const auto overlay = OverlayFrom(releases.editedTiles, releases.base.asset, 7);
    const TerrainOverlayApplyResult applied = resolved.world.SetOverlay(releases.base.asset, overlay);
    INFO(applied.reason);
    REQUIRE(applied.applied);

    const TerrainPlacement placement{releases.edited.asset, WorldAt(0.0, 0.0)};
    for (const double required : {4.0, 8.0, 16.0, 1.0, 0.5})
        for (const glm::dvec2 point : OverlayPoints())
        {
            INFO("point " << point.x << ", " << point.y << " at " << required << " m");
            const TerrainHeightResult got = resolved.world.HeightAt(WorldAt(point.x, point.y), required);
            const TerrainHeightResult want = edited.HeightAt(WorldAt(point.x, point.y), placement,
                                                             TerrainHeightRequest{releases.edited, required});
            REQUIRE(got.HasHeight());
            REQUIRE(want.HasHeight());
            CHECK(got.status == want.status);
            CHECK(got.localHeight == want.localHeight);
            CHECK(got.tile == want.tile);
            CHECK(got.spacing == want.spacing);
            CHECK(got.sourceSpacing == want.sourceSpacing);
            REQUIRE(got.worldPoint);
            REQUIRE(want.worldPoint);
            CHECK(got.worldPoint->Meters() == want.worldPoint->Meters());
        }
    std::filesystem::remove_all(releases.root);
}

TEST_CASE("Outside overlay tiles the release answers unchanged, Loading or Ready", "[terrain][overlay]")
{
    const Releases releases = PublishReleases();
    const auto overlay = OverlayFrom(releases.editedTiles, releases.base.asset, 3);
    const std::vector<glm::dvec2> outside{{300.5, -100.25}, {-300.25, 300.5}, {256.5, 0.0},
                                          {0.0, 256.5},     {383.5, -383.5},    {-383.5, 383.5}};

    Resolved lazy = Resolve(releases.root, releases.base.asset, TerrainResidencyMode::Interest);
    TerrainWorld lazyBase = lazy.world;
    const TerrainOverlayApplyResult lazyApplied = lazy.world.SetOverlay(releases.base.asset, overlay);
    INFO(lazyApplied.reason);
    REQUIRE(lazyApplied.applied);
    for (const glm::dvec2 point : outside)
    {
        INFO("point " << point.x << ", " << point.y);
        const TerrainHeightResult base = lazyBase.HeightAt(WorldAt(point.x, point.y), 4.0);
        const TerrainHeightResult over = lazy.world.HeightAt(WorldAt(point.x, point.y), 4.0);
        CHECK(base.status == Status::Loading);
        CheckSameAnswer(over, base);
    }
    CHECK(lazy.world.HeightAt(WorldAt(-100.5, -80.25), 4.0).status == Status::MeetsRequirement);
    CHECK(lazyBase.HeightAt(WorldAt(-100.5, -80.25), 4.0).status == Status::Loading);

    Resolved ready = Resolve(releases.root, releases.base.asset, TerrainResidencyMode::Eager);
    TerrainWorld readyBase = ready.world;
    REQUIRE(ready.world.SetOverlay(releases.base.asset, overlay).applied);
    for (const glm::dvec2 point : outside)
        for (const double required : {4.0, 8.0, 1.0})
        {
            INFO("point " << point.x << ", " << point.y << " at " << required << " m");
            const TerrainHeightResult base = readyBase.HeightAt(WorldAt(point.x, point.y), required);
            const TerrainHeightResult over = ready.world.HeightAt(WorldAt(point.x, point.y), required);
            CHECK(base.status == (required >= 4.0 ? Status::MeetsRequirement : Status::CoarserAtSourceLimit));
            CheckSameAnswer(over, base);
        }
    std::filesystem::remove_all(releases.root);
}

TEST_CASE("A wrong-sized, unknown or non-finite overlay is refused whole and the previous snapshot stays",
          "[terrain][overlay]")
{
    const Releases releases = PublishReleases();
    Resolved resolved = Resolve(releases.root, releases.base.asset, TerrainResidencyMode::Eager);
    const int32_t intervals = OverlayLayout().IntervalsPerTile();
    const auto good = OverlayFrom(releases.editedTiles, releases.base.asset, 1);
    REQUIRE(resolved.world.SetOverlay(releases.base.asset, good).applied);
    const double editedHeight = resolved.world.HeightAt(WorldAt(-100.5, -80.25), 4.0).localHeight;

    const auto refuse = [&](std::shared_ptr<const TerrainOverlay> snapshot) -> std::string
    {
        const TerrainOverlayApplyResult result = resolved.world.SetOverlay(releases.base.asset, std::move(snapshot));
        UNSCOPED_INFO(result.reason);
        CHECK_FALSE(result.applied);
        CHECK(resolved.world.Assets().front().overlay == good);
        CHECK(resolved.world.HeightAt(WorldAt(-100.5, -80.25), 4.0).localHeight == editedHeight);
        return result.reason;
    };

    std::optional<TileHeights> wrongSize = TileHeights::Create({0, 0}, intervals / 2);
    REQUIRE(wrongSize);
    const std::string wrongSizeMessage = refuse(SingleTileOverlay(std::move(*wrongSize), releases.base.asset, 2));
    CHECK(wrongSizeMessage.find("(0, 0)") != std::string::npos);
    CHECK(wrongSizeMessage.find("intervals") != std::string::npos);

    std::optional<TileHeights> unknown = TileHeights::Create({7, 7}, intervals);
    REQUIRE(unknown);
    CHECK(refuse(SingleTileOverlay(std::move(*unknown), releases.base.asset, 3)).find("(7, 7)") != std::string::npos);

    TileHeights nonFinite = TileOf(releases.baseTiles, {0, 0});
    nonFinite.Set(3, 3, std::numeric_limits<float>::quiet_NaN());
    CHECK(refuse(SingleTileOverlay(std::move(nonFinite), releases.base.asset, 4)).find("non-finite") !=
          std::string::npos);

    const AssetId other = TerrainAsset("terrain/other.terrain.json");
    CHECK(refuse(OverlayFrom(releases.editedTiles, other, 5)).find("asset") != std::string::npos);
    std::filesystem::remove_all(releases.root);
}

TEST_CASE("Clearing restores the base and an older snapshot keeps its own heights", "[terrain][overlay]")
{
    const Releases releases = PublishReleases();
    Resolved resolved = Resolve(releases.root, releases.base.asset, TerrainResidencyMode::Eager);
    TerrainWorld base = resolved.world;
    const glm::dvec2 point{80.0, 84.0};

    const auto first = OverlayFrom(releases.editedTiles, releases.base.asset, 1);
    REQUIRE(resolved.world.SetOverlay(releases.base.asset, first).applied);
    const double firstHeight = resolved.world.HeightAt(WorldAt(point.x, point.y), 4.0).localHeight;
    CHECK(firstHeight == TileOf(releases.editedTiles, {0, 0}).At(20, 21));

    std::vector<TileHeights> changed = releases.editedTiles;
    TileOf(changed, {0, 0}).Set(20, 21, 42.5f);
    const auto second = OverlayFrom(changed, releases.base.asset, 2);
    REQUIRE(resolved.world.SetOverlay(releases.base.asset, second).applied);
    REQUIRE(resolved.world.Assets().front().overlay);
    CHECK(resolved.world.Assets().front().overlay->Revision() == 2);
    const double secondHeight = resolved.world.HeightAt(WorldAt(point.x, point.y), 4.0).localHeight;
    CHECK(secondHeight == 42.5);
    CHECK(secondHeight != firstHeight);

    TerrainHeightQuery &query = resolved.world.Instances().front().release->Query();
    const TerrainHeightRequest request{releases.base, 4.0};
    const std::optional<TerrainHeightResult> oldAnswer =
        query.OverlayHeightAt(*first, Local(point.x, point.y), request);
    REQUIRE(oldAnswer);
    CHECK(oldAnswer->localHeight == firstHeight);
    const std::optional<TerrainHeightResult> newAnswer =
        query.OverlayHeightAt(*second, Local(point.x, point.y), request);
    REQUIRE(newAnswer);
    CHECK(newAnswer->localHeight == secondHeight);

    std::vector<TerrainIssue> issues;
    TerrainWorld resolvedAgain = ResolveTerrainWorld(InstanceSpec(releases.base.asset), releases.root,
                                                     &resolved.world, issues, *resolved.streamer,
                                                     TerrainEagerLoad::Wait);
    CHECK(issues.empty());
    CHECK(resolvedAgain.HeightAt(WorldAt(point.x, point.y), 4.0).localHeight == secondHeight);

    REQUIRE(resolved.world.SetOverlay(releases.base.asset, nullptr).applied);
    CHECK(resolved.world.Assets().front().overlay == nullptr);
    const TerrainHeightResult restored = resolved.world.HeightAt(WorldAt(point.x, point.y), 4.0);
    CheckSameAnswer(restored, base.HeightAt(WorldAt(point.x, point.y), 4.0));
    CHECK(restored.localHeight != secondHeight);
    std::filesystem::remove_all(releases.root);
}

TEST_CASE("A release switch revalidates the carried overlay and drops only a mismatch", "[terrain][overlay]")
{
    const Releases releases = PublishReleases();
    Resolved resolved = Resolve(releases.root, releases.base.asset, TerrainResidencyMode::Eager);
    const auto overlay = OverlayFrom(releases.editedTiles, releases.base.asset, 9);
    REQUIRE(resolved.world.SetOverlay(releases.base.asset, overlay).applied);
    const double editedHeight = resolved.world.HeightAt(WorldAt(-100.5, -80.25), 4.0).localHeight;

    const auto resolveFrom = [&](const TerrainWorld &previous, std::vector<TerrainIssue> &issues)
    {
        return ResolveTerrainWorld(InstanceSpec(releases.base.asset), releases.root, &previous, issues,
                                   *resolved.streamer, TerrainEagerLoad::Wait);
    };

    std::vector<TerrainIssue> sameIssues;
    TerrainWorld same = resolveFrom(resolved.world, sameIssues);
    CHECK(sameIssues.empty());
    REQUIRE(same.Assets().front().overlay == overlay);
    CHECK(same.HeightAt(WorldAt(-100.5, -80.25), 4.0).localHeight == editedHeight);

    const TerrainReleaseInfo other = Info(releases.base.asset, 0xA11);
    WriteRelease(releases.root, other, Encode(other, OverlayLayout(), releases.baseTiles));
    PointCurrentAt(releases.root, releases.base.asset, other.release);
    std::vector<TerrainIssue> keptIssues;
    TerrainWorld kept = resolveFrom(same, keptIssues);
    CHECK(keptIssues.empty());
    REQUIRE(kept.Assets().front().overlay == overlay);
    CHECK(kept.HeightAt(WorldAt(-100.5, -80.25), 4.0).localHeight == editedHeight);

    const auto coarse = TerrainLayout::CreateFromTileSize(256.0, 8.0, {-384.0, -384.0, 384.0, 384.0});
    REQUIRE(coarse);
    const TerrainReleaseInfo changed = Info(releases.base.asset, 0xC0A5);
    WriteRelease(releases.root, changed, Encode(changed, *coarse, BuildTiles(*coarse)));
    PointCurrentAt(releases.root, releases.base.asset, changed.release);
    std::vector<TerrainIssue> droppedIssues;
    const TerrainWorld dropped = resolveFrom(kept, droppedIssues);
    REQUIRE(droppedIssues.size() == 1);
    CHECK(droppedIssues[0].kind == TerrainIssueKind::OverlayDropped);
    CHECK(droppedIssues[0].recipePath == kRecipe);
    CHECK(droppedIssues[0].message.find(kRecipe) != std::string::npos);
    CHECK(dropped.Assets().front().overlay == nullptr);
    std::filesystem::remove_all(releases.root);
}

TEST_CASE("Offered overlays apply once their release loads, once, and a refused one is reported", "[terrain][overlay]")
{
    const Releases releases = PublishReleases();
    const auto overlay = OverlayFrom(releases.editedTiles, releases.base.asset, 5);
    TerrainOverlayHandover handover;
    handover.Offer(releases.base.asset, std::string(kRecipe), overlay);
    CHECK_FALSE(handover.Empty());

    std::vector<TerrainIssue> issues;
    TerrainWorld beforeLoad;
    CHECK(handover.Apply(beforeLoad, issues) == 0);
    CHECK(issues.empty());
    CHECK_FALSE(handover.Empty());

    Resolved resolved = Resolve(releases.root, releases.base.asset, TerrainResidencyMode::Eager);
    CHECK(handover.Apply(resolved.world, issues) == 1);
    CHECK(issues.empty());
    CHECK(handover.Empty());
    REQUIRE(resolved.world.Assets().front().overlay == overlay);
    const double editedHeight = TileOf(releases.editedTiles, {0, 0}).At(20, 21);
    CHECK(resolved.world.HeightAt(WorldAt(80.0, 84.0), 4.0).localHeight == editedHeight);

    CHECK(handover.Apply(resolved.world, issues) == 0);
    CHECK(issues.empty());
    REQUIRE(resolved.world.Assets().front().overlay == overlay);
    CHECK(resolved.world.HeightAt(WorldAt(80.0, 84.0), 4.0).localHeight == editedHeight);

    std::optional<TileHeights> wrongSize = TileHeights::Create({0, 0}, OverlayLayout().IntervalsPerTile() / 2);
    REQUIRE(wrongSize);
    handover.Offer(releases.base.asset, std::string(kRecipe),
                   SingleTileOverlay(std::move(*wrongSize), releases.base.asset, 6));
    std::vector<TerrainIssue> refused;
    CHECK(handover.Apply(resolved.world, refused) == 0);
    CHECK(handover.Empty());
    REQUIRE(refused.size() == 1);
    CHECK(refused[0].kind == TerrainIssueKind::OverlayDropped);
    CHECK(refused[0].recipePath == kRecipe);
    CHECK(refused[0].message.find(kRecipe) != std::string::npos);
    CHECK(resolved.world.Assets().front().overlay == overlay);
    std::filesystem::remove_all(releases.root);
}

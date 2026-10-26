/**
 * @file TerrainGround.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainGround.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"
#include "TerrainStreamingTestSupport.h"

#include <terrain/ComponentRegistration.h>
#include <terrain/TerrainGround.h>
#include <terrain/TerrainScene.h>
#include <terrain/TerrainSurface.h>
#include <terrain/components/TerrainInstance.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    constexpr std::string_view kIdRoot = "games/TerrainFixture/assets";
    constexpr std::string_view kRecipe = "terrain/snap.terrain.json";

    WorldPosition At(double x, double y, double z)
    {
        return *WorldPosition::FromDoubleMeters({x, y, z});
    }

    TerrainInstance Recipe()
    {
        TerrainInstance instance{};
        std::memcpy(instance.recipePath, kRecipe.data(), kRecipe.size());
        return instance;
    }

    TileHeights Decimate(const TileHeights &fine)
    {
        std::optional<TileHeights> coarse = TileHeights::Create(fine.Tile(), fine.IntervalsPerTile() / 2);
        REQUIRE(coarse);
        for (int32_t k = 0; k <= coarse->IntervalsPerTile(); ++k)
            for (int32_t i = 0; i <= coarse->IntervalsPerTile(); ++i)
                coarse->Set(i, k, fine.At(2 * i, 2 * k));
        return *coarse;
    }

    struct Layers
    {
        TerrainReleaseInfo info = Info(TerrainAsset(kRecipe), 0x5A9);
        TerrainLayout fineLayout = ReliefLayout();
        TerrainLayout coarseLayout = ReliefLayout();
        std::vector<TileHeights> fine = BuildTiles(fineLayout);
        std::vector<TileHeights> coarse;
        std::filesystem::path root;
        TerrainWorld world;

        Layers()
        {
            std::optional<TerrainLayout> coarseLayoutValue =
                TerrainLayout::CreateFromTileSize(256.0, 8.0, {-250.0, -256.0, 256.0, 250.0});
            REQUIRE(coarseLayoutValue);
            coarseLayout = *coarseLayoutValue;
            for (const TileHeights &tile : fine)
                coarse.push_back(Decimate(tile));

            std::vector<TerrainTileWrite> writes;
            for (size_t i = 0; i < fine.size(); ++i)
            {
                writes.push_back({&fine[i], 0, true, std::nullopt, 0});
                writes.push_back({&coarse[i], 1, false, std::nullopt, 0});
            }
            const std::array<TerrainLayout, 2> layouts{fineLayout, coarseLayout};
            TerrainResult<TerrainEncodedRelease> encoded = EncodeTerrainRelease(info, layouts, writes);
            REQUIRE(encoded);
            root = MakeScratchDir("ground");
            WriteRelease(root, info, *encoded.value);
            PointCurrentAt(root, info.asset, info.release);
        }

        ~Layers()
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    };

    struct Scene
    {
        World world;

        Scene()
        {
            RegisterCoreComponents();
            RegisterTerrainComponents();
            EmplaceTestStreamer(world);
            const Entity entity = world.CreateEntity();
            AddTransform(world, entity, Transform(WorldVec3{0.0}));
            world.AddComponent(entity, Recipe());
        }

        TerrainWorld Resolve(const std::filesystem::path &root, TerrainEagerLoad eager)
        {
            std::vector<TerrainIssue> issues;
            const TerrainSceneSpec spec = CollectTerrainScene(world, kIdRoot);
            issues = spec.issues;
            return ResolveTerrainWorld(spec, root, nullptr, issues, world.Resource<TerrainStreamer>(), eager);
        }
    };

    const TileHeights *TileAt(const std::vector<TileHeights> &tiles, TileCoord coord)
    {
        const auto it = std::find_if(tiles.begin(), tiles.end(),
                                     [coord](const TileHeights &tile) { return tile.Tile() == coord; });
        return it != tiles.end() ? &*it : nullptr;
    }
}

TEST_CASE("A coarse resident terrain layer is not source precision", "[terrain][ground]")
{
    Layers layers;
    Scene scene;
    TerrainWorld world = scene.Resolve(layers.root, TerrainEagerLoad::Async);
    REQUIRE(world.Instances().size() == 1);
    const std::shared_ptr<TerrainResidentRelease> &release = world.Instances().front().release;

    const WorldPosition point = At(10.0, 0.0, 10.0);
    const GroundQueryResult waiting = ProbeTerrainGround(world, point);
    CHECK(waiting.status == GroundQueryStatus::Loading);
    CHECK(std::isnan(waiting.spacing));

    const TileHeights *coarse = TileAt(layers.coarse, TileCoord{0, 0});
    REQUIRE(coarse != nullptr);
    REQUIRE(release->Query().Insert(release->Info(), 1, std::make_shared<const TileHeights>(*coarse)).code ==
            TerrainPackErrorCode::None);

    const GroundQueryResult coarseOnly = ProbeTerrainGround(world, point);
    CHECK(coarseOnly.status == GroundQueryStatus::Loading);
    CHECK(coarseOnly.spacing == 8.0);
    CHECK(coarseOnly.sourceSpacing == 4.0);
    CHECK_FALSE(coarseOnly.surface);

    const TileHeights *fine = TileAt(layers.fine, TileCoord{0, 0});
    REQUIRE(fine != nullptr);
    REQUIRE(release->Query().Insert(release->Info(), 0, std::make_shared<const TileHeights>(*fine)).code ==
            TerrainPackErrorCode::None);

    const GroundQueryResult ready = ProbeTerrainGround(world, point);
    REQUIRE(ready.IsReady());
    CHECK(ready.spacing == 4.0);
    CHECK(ready.sourceSpacing == 4.0);
    const SurfaceSample surface = EvaluateSurface(layers.fineLayout, *TerrainLocalPosition::FromMeters({10.0, 0.0, 10.0}),
                                                  [&](TileCoord tile) -> std::span<const float>
                                                  {
                                                      const TileHeights *found = TileAt(layers.fine, tile);
                                                      return found ? found->Samples() : std::span<const float>{};
                                                  });
    REQUIRE(surface.IsOk());
    REQUIRE(ready.surface);
    CHECK(ready.surface->Meters().y == Catch::Approx(surface.height));
}

TEST_CASE("Coverage comes from layout metadata without requesting payloads", "[terrain][ground]")
{
    Layers layers;
    Scene scene;
    TerrainWorld world = scene.Resolve(layers.root, TerrainEagerLoad::Async);
    REQUIRE(world.Instances().size() == 1);
    const std::shared_ptr<TerrainResidentRelease> &release = world.Instances().front().release;

    const GroundQueryCoverage coverage = ProbeTerrainGroundCoverage(world, At(10.0, 0.0, 10.0));
    CHECK(coverage.mCovered);
    CHECK_FALSE(coverage.mSourceResident);
    CHECK(coverage.mSourceSpacing == 4.0);
    CHECK(coverage.mIdentity == layers.info.release.value);
    CHECK(release->Query().PayloadRequests().empty());

    const GroundQueryCoverage outside = ProbeTerrainGroundCoverage(world, At(100000.0, 0.0, 100000.0));
    CHECK_FALSE(outside.mCovered);
    CHECK(release->Query().PayloadRequests().empty());
}

TEST_CASE("Ground probes outside the map do not fall back to another instance", "[terrain][ground]")
{
    Layers layers;
    Scene scene;
    TerrainWorld world = scene.Resolve(layers.root, TerrainEagerLoad::Wait);
    const WorldPosition outside = At(100000.0, 0.0, 100000.0);
    const GroundQueryResult result = ProbeTerrainGround(world, outside);
    CHECK(result.status == GroundQueryStatus::OutOfBounds);
    CHECK_FALSE(result.surface);
}

TEST_CASE("Only a height whose spacing reached the source is ready", "[terrain][ground]")
{
    TerrainHeightResult result;
    result.status = TerrainHeightStatus::CoarserAtSourceLimit;
    result.failure = TerrainHeightFailure::None;
    result.localHeight = 3.0;
    result.worldPoint = At(1.0, 3.0, 2.0);
    result.spacing = 8.0;
    result.sourceSpacing = 4.0;
    result.release = Info(TerrainAsset(kRecipe), 7);

    const GroundQueryResult coarse = ProbeTerrainHeightResult(result);
    CHECK(coarse.status == GroundQueryStatus::Loading);
    CHECK(coarse.spacing == 8.0);
    CHECK(coarse.sourceSpacing == 4.0);

    result.spacing = 4.0;
    const GroundQueryResult source = ProbeTerrainHeightResult(result);
    CHECK(source.IsReady());
    CHECK(source.identity == 7);

    result.status = TerrainHeightStatus::OutsideMap;
    result.worldPoint.reset();
    CHECK(ProbeTerrainHeightResult(result).status == GroundQueryStatus::OutOfBounds);

    result.status = TerrainHeightStatus::Failed;
    result.failure = TerrainHeightFailure::LoadFailed;
    CHECK(ProbeTerrainHeightResult(result).status == GroundQueryStatus::Failed);
}

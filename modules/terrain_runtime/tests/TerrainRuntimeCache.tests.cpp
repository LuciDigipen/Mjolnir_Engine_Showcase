/**
 * @file TerrainRuntimeCache.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain_runtime/TerrainRuntimeCache.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainPackTestSupport.h"
#include "TerrainResidencyTestSupport.h"
#include "TerrainStreamingTestSupport.h"
#include "support/FakeTextureUploader.h"

#include <terrain_runtime/TerrainRuntimeCache.h>
#include <terrain_runtime/TerrainWeightResidency.h>

#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainMeshResidency.h>
#include <terrain/TerrainScene.h>
#include <terrain/TerrainStreamer.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec3.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    constexpr std::string_view kRecipe = "terrain/shared.terrain.json";

    std::shared_ptr<TerrainMeshUploader> Borrowed(TerrainMeshUploader &uploader)
    {
        return std::shared_ptr<TerrainMeshUploader>(&uploader, [](TerrainMeshUploader *) {});
    }

    TerrainSceneSpec OneInstance(AssetId asset)
    {
        TerrainSceneSpec spec;
        TerrainInstanceSpec instance;
        instance.recipePath = std::string(kRecipe);
        instance.logicalPath = instance.recipePath;
        instance.asset = asset;
        instance.translation = *WorldPosition::FromDoubleMeters(glm::dvec3(0.0));
        instance.entity = Entity{1, 0};
        spec.instances.push_back(std::move(instance));
        return spec;
    }

    std::vector<TerrainPayloadWant> Wants(const TerrainResidentRelease &release)
    {
        std::vector<TerrainPayloadWant> wants;
        const TerrainIndex &index = release.Handle().Index();
        for (const std::vector<TerrainTileRecord> *records : {&index.tiles, &index.regions})
            for (const TerrainTileRecord &record : *records)
                wants.push_back({TerrainPayloadKeyOf(release.Info(), record), TerrainDemand::Visible, 0.0f});
        return wants;
    }

    void Settle(TerrainRuntimeCache &cache, TerrainManualExecutor &executor)
    {
        const auto busy = [&cache] {
            const TerrainStreamingStats stats = cache.Streamer().Stats();
            return stats.loading + stats.waiting > 0;
        };
        for (int round = 0; round < 64 && busy(); ++round)
        {
            cache.Streamer().Update();
            executor.RunAll();
        }
        cache.Streamer().Update();
        executor.RunAll();
        cache.Streamer().Update();
    }

    std::span<const TerrainReleaseInfo> Held(const TerrainReleaseInfo &info)
    {
        return std::span<const TerrainReleaseInfo>(&info, 1);
    }

    // 4 x 4 tiles of 64 m at 4 m with an 8 m LOD and level 0 Regions of 2 x 2 tiles.
    TerrainEncodedRelease ResidencyRelease(const TerrainReleaseInfo &info)
    {
        std::vector<TerrainLayout> layers;
        for (double spacing : {4.0, 8.0})
            layers.push_back(*TerrainLayout::CreateFromTileSize(64.0, spacing, {0.0, 0.0, 256.0, 256.0}));
        const std::vector<TileHeights> tiles = BuildTiles(layers[0]);
        std::vector<TerrainTileWrite> sources;
        for (const TileHeights &tile : tiles)
            sources.push_back({&tile});
        const std::vector<TerrainPayloadWrite> lods = BuildTestLods(layers, tiles);
        const TerrainRegionWrites regions = BuildTestRegions(layers[0], tiles, 2, 4);
        TerrainResult<TerrainEncodedRelease> result = EncodeTerrainRelease(info, layers, sources, lods, regions);
        INFO(result.error.message);
        REQUIRE(result);
        return std::move(*result.value);
    }

    void BuildMesh(TerrainRuntimeCache &cache, TerrainManualExecutor &executor, FakeUploader &uploader,
                   const TerrainSelectionInstance &instance, const std::vector<TerrainPayloadWant> &wants, double now)
    {
        cache.Meshes().Request({&instance, 1}, wants, cache.Streamer(), now);
        executor.RunAll();
        cache.Meshes().Install();
        cache.Meshes().Request({&instance, 1}, wants, cache.Streamer(), now);
        uploader.Frame();
        uploader.Frame();
        cache.Meshes().Install();
    }

    TerrainEncodedRelease WeightedRelease(const TerrainReleaseInfo &info, const TerrainLayout &layout)
    {
        const std::vector<TileHeights> tiles = BuildTiles(layout);
        std::vector<TerrainWeightTile> weights;
        const TileRange range = layout.Tiles();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                TerrainWeightTile tile;
                tile.tile = {x, z};
                tile.intervals = layout.IntervalsPerTile();
                tile.layers = {"dirt"};
                tile.weights.assign(tile.SampleCount(), 1.0f);
                weights.push_back(std::move(tile));
            }
        TerrainWeightsWrites writes;
        writes.layers = {"dirt"};
        writes.fingerprint = 0xF00D;
        writes.tiles = weights;
        return EncodeWithWeights(info, layout, tiles, writes, {.chunkTiles = 2});
    }
} // namespace

TEST_CASE("A second World resolving a shared release reads no payload again", "[terrain_runtime][cache]")
{
    const AssetId asset = TerrainAsset(std::string(kRecipe));
    const TerrainReleaseInfo info = Info(asset, 7);
    const std::filesystem::path root = MakeScratchDir("shared_cache_reads");
    WriteRelease(root, info, EncodeEdge(asset, 7));
    PointCurrentAt(root, asset, info.release);

    auto executor = std::make_shared<TerrainManualExecutor>();
    FakeUploader uploader;
    TerrainRuntimeCache cache(StreamingSettings(uint64_t(1) << 24, TerrainResidencyMode::Interest), executor,
                              Borrowed(uploader));
    const TerrainSceneSpec spec = OneInstance(asset);

    std::vector<TerrainIssue> issues;
    const TerrainResolveOptions options;
    const TerrainWorld first = ResolveTerrainWorld(spec, root, nullptr, issues, cache.Streamer(), options);
    REQUIRE(issues.empty());
    REQUIRE(first.Instances().size() == 1);
    const TerrainInterestId interest = cache.Streamer().AddInterest();
    cache.Streamer().SetInterest(interest, Wants(*first.Instances()[0].release));
    Settle(cache, *executor);
    const uint64_t reads = cache.Streamer().Stats().readsStarted;
    REQUIRE(reads > 0);

    std::vector<TerrainIssue> otherIssues;
    const TerrainWorld second = ResolveTerrainWorld(spec, root, nullptr, otherIssues, cache.Streamer(), options);
    REQUIRE(otherIssues.empty());
    REQUIRE(second.Instances().size() == 1);
    CHECK(second.Instances()[0].release == first.Instances()[0].release);
    const TerrainInterestId otherInterest = cache.Streamer().AddInterest();
    cache.Streamer().SetInterest(otherInterest, Wants(*second.Instances()[0].release));
    Settle(cache, *executor);

    CHECK(cache.Streamer().Stats().readsStarted == reads);
    for (const TerrainPayloadWant &want : Wants(*second.Instances()[0].release))
        CHECK(cache.Streamer().Status(want.key).state == TerrainPayloadState::Ready);
    std::filesystem::remove_all(root);
}

TEST_CASE("A mesh one runtime holds is reused by the other and survives its unload", "[terrain_runtime][cache]")
{
    const TerrainReleaseInfo info = Info(TerrainAsset(std::string(kRecipe)), 9);
    const std::shared_ptr<TerrainResidentRelease> release =
        TerrainResidentRelease::Create(OpenOk(TerrainReleaseHandle::Open(MakeTerrainMemorySource(ResidencyRelease(info)))));

    auto executor = std::make_shared<TerrainManualExecutor>();
    FakeUploader uploader;
    TerrainRuntimeCache cache(StreamingSettings(uint64_t(1) << 30, TerrainResidencyMode::Eager), executor,
                              Borrowed(uploader));
    cache.Streamer().Add(release);
    Settle(cache, *executor);

    const TerrainSelectionInstance instance{release->SharedHandle(),
                                            {info.asset, *WorldPosition::FromDoubleMeters(glm::dvec3(0.0))}};
    const TerrainPayloadKey tile =
        TerrainPayloadKeyOf(info, *release->Handle().Index().Find({0, 0}, 0, TerrainPayloadKind::LodHeight, 0));
    const std::vector<TerrainPayloadWant> wants{{tile, TerrainDemand::Visible, 0.0f}};
    double now = cache.Now();

    const TerrainCacheOwner first = cache.AddOwner();
    cache.SetHeldReleases(first, Held(info));
    BuildMesh(cache, *executor, uploader, instance, wants, now);
    REQUIRE(cache.Meshes().Ready(tile));
    REQUIRE(uploader.uploadOrder.size() == 1);

    const TerrainCacheOwner second = cache.AddOwner();
    cache.SetHeldReleases(second, Held(info));
    cache.Meshes().Request({&instance, 1}, wants, cache.Streamer(), now);
    CHECK(uploader.uploadOrder.size() == 1);
    CHECK(cache.Meshes().Ready(tile));

    cache.RemoveOwner(first);
    CHECK(uploader.retired.empty());
    CHECK(cache.Meshes().Ready(tile));

    now += 0.5;
    cache.Meshes().Request({&instance, 1}, {}, cache.Streamer(), now);
    CHECK(cache.Meshes().Ready(tile));
    now += 1.0;
    cache.Meshes().Request({&instance, 1}, {}, cache.Streamer(), now);
    CHECK_FALSE(cache.Meshes().Ready(tile));
    CHECK(uploader.retired == std::vector<TerrainPayloadKey>{tile});
}

TEST_CASE("The last runtime dropping a release retires its meshes at once", "[terrain_runtime][cache]")
{
    const TerrainReleaseInfo info = Info(TerrainAsset(std::string(kRecipe)), 11);
    const std::shared_ptr<TerrainResidentRelease> release =
        TerrainResidentRelease::Create(OpenOk(TerrainReleaseHandle::Open(MakeTerrainMemorySource(ResidencyRelease(info)))));

    auto executor = std::make_shared<TerrainManualExecutor>();
    FakeUploader uploader;
    TerrainRuntimeCache cache(StreamingSettings(uint64_t(1) << 30, TerrainResidencyMode::Eager), executor,
                              Borrowed(uploader));
    cache.Streamer().Add(release);
    Settle(cache, *executor);

    const TerrainSelectionInstance instance{release->SharedHandle(),
                                            {info.asset, *WorldPosition::FromDoubleMeters(glm::dvec3(0.0))}};
    const TerrainPayloadKey tile =
        TerrainPayloadKeyOf(info, *release->Handle().Index().Find({1, 1}, 0, TerrainPayloadKind::LodHeight, 0));
    const std::vector<TerrainPayloadWant> wants{{tile, TerrainDemand::Visible, 0.0f}};
    const double now = cache.Now();

    const TerrainCacheOwner owner = cache.AddOwner();
    cache.SetHeldReleases(owner, Held(info));
    BuildMesh(cache, *executor, uploader, instance, wants, now);
    REQUIRE(cache.Meshes().Ready(tile));

    cache.RemoveOwner(owner);
    CHECK_FALSE(cache.Meshes().Ready(tile));
    CHECK(uploader.retired == std::vector<TerrainPayloadKey>{tile});
}

TEST_CASE("Weight textures survive one runtime's unload and retire with the last", "[terrain_runtime][cache]")
{
    const TerrainReleaseInfo info = Info(TerrainAsset(std::string(kRecipe)), 13);
    const TerrainLayout layout = EdgeLayout();
    const auto release = std::make_shared<const TerrainReleaseHandle>(
        std::move(OpenInMemory(WeightedRelease(info, layout)).handle));

    auto executor = std::make_shared<TerrainManualExecutor>();
    FakeUploader uploader;
    FakeTextureUploader textures;
    TerrainStreamingSettings settings = StreamingSettings(uint64_t(1) << 30);
    settings.gpu.weightBudgetBytes = uint64_t(1) << 24;
    TerrainRuntimeCache cache(settings, executor, Borrowed(uploader), &textures, 0);
    REQUIRE(cache.Weights());

    const TerrainCacheOwner edit = cache.AddOwner();
    const TerrainCacheOwner play = cache.AddOwner();
    cache.SetHeldReleases(edit, Held(info));
    cache.SetHeldReleases(play, Held(info));

    TerrainWeightResidency &weights = *cache.Weights();
    weights.BeginRequest();
    weights.Request({release, {info.asset, TerrainUnitKind::Tile, 0, layout.Tiles().min}});
    weights.Install(cache.Now());
    executor->RunAll();
    weights.Install(cache.Now());
    REQUIRE(textures.mQueued == 1);

    cache.SetHeldReleases(play, {});
    cache.RemoveOwner(play);
    CHECK(textures.mRetired.empty());

    cache.SetHeldReleases(edit, {});
    CHECK(textures.mRetired.size() == 1);
}

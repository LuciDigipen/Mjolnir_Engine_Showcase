/**
 * @file TerrainWeightResidency.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain_runtime/TerrainWeightResidency: weight textures of drawn tiles.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainPackTestSupport.h"

#include <terrain_runtime/TerrainRender.h>
#include <terrain_runtime/TerrainWeightResidency.h>

#include <renderer/TerrainDrawData.h>

#include "support/FakeTextureUploader.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    constexpr uint32_t kSamplerRow = 11;

    TerrainStreamingSettings::Gpu WeightGpu(uint64_t budgetBytes)
    {
        TerrainStreamingSettings::Gpu gpu;
        gpu.weightBudgetBytes = budgetBytes;
        gpu.meshRetainSeconds = 4.0;
        return gpu;
    }

    std::vector<TerrainWeightTile> ConstantWeights(const TerrainLayout &layout, const std::vector<std::string> &layers,
                                                   const std::vector<float> &split)
    {
        std::vector<TerrainWeightTile> tiles;
        const TileRange range = layout.Tiles();
        const int32_t n = layout.IntervalsPerTile();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                TerrainWeightTile tile;
                tile.tile = {x, z};
                tile.intervals = n;
                tile.layers = layers;
                const size_t count = tile.SampleCount();
                tile.weights.resize(count * split.size());
                for (size_t layer = 0; layer < split.size(); ++layer)
                    for (size_t sample = 0; sample < count; ++sample)
                        tile.weights[layer * count + sample] = split[layer];
                tiles.push_back(std::move(tile));
            }
        return tiles;
    }

    struct WeightedRelease
    {
        TerrainLayout layout = EdgeLayout();
        std::vector<TileHeights> tiles = BuildTiles(layout);
        std::vector<std::string> table{"dirt", "sand"};
        AssetId asset = TerrainAsset("terrain/weighted.terrain.json");
        std::vector<TerrainWeightTile> weights;
        TerrainEncodedRelease encoded;
        std::shared_ptr<TerrainMemorySource> source;
        std::shared_ptr<const TerrainReleaseHandle> handle;

        WeightedRelease(uint64_t release, std::vector<float> split)
            : weights(ConstantWeights(layout, table, split))
        {
            TerrainWeightsWrites writes;
            writes.layers = table;
            writes.fingerprint = 0xF00D;
            writes.tiles = weights;
            encoded = EncodeWithWeights(Info(asset, release), layout, tiles, writes, {.chunkTiles = 2});
            OpenedRelease opened = OpenInMemory(encoded);
            source = opened.source;
            handle = std::make_shared<const TerrainReleaseHandle>(std::move(opened.handle));
        }

        TileKey Key(TileCoord tile) const
        {
            return {asset, tile};
        }
    };

    struct PlainRelease
    {
        TerrainLayout layout = EdgeLayout();
        std::vector<TileHeights> tiles = BuildTiles(layout);
        AssetId asset = TerrainAsset("terrain/plain.terrain.json");
        std::shared_ptr<TerrainMemorySource> source;
        std::shared_ptr<const TerrainReleaseHandle> handle;

        explicit PlainRelease(uint64_t release)
        {
            OpenedRelease opened = OpenInMemory(Encode(Info(asset, release), layout, tiles, {.chunkTiles = 2}));
            source = opened.source;
            handle = std::make_shared<const TerrainReleaseHandle>(std::move(opened.handle));
        }

        TileKey Key(TileCoord tile) const
        {
            return {asset, tile};
        }
    };

    struct Rig
    {
        std::shared_ptr<TerrainManualExecutor> executor = std::make_shared<TerrainManualExecutor>();
        FakeTextureUploader textures;
        TerrainWeightResidency residency;
        double now = 0.0;

        explicit Rig(uint64_t budgetBytes) : residency(WeightGpu(budgetBytes), executor, textures, kSamplerRow) {}

        void Begin()
        {
            residency.BeginRequest();
        }

        void Install()
        {
            residency.Install(now);
        }

        void Advance(double seconds)
        {
            now += seconds;
        }

        void Draw(const std::shared_ptr<const TerrainReleaseHandle> &release, TileCoord tile, float distance)
        {
            Begin();
            residency.Request({release, tile, distance});
            Install();
        }

        void FinishReads()
        {
            executor->RunAll();
            Install();
        }
    };
}

TEST_CASE("A drawn tile queues one weight texture and draws with its words", "[terrain_runtime][weights]")
{
    WeightedRelease release(0xE2, {0.5f, 0.5f});
    Rig rig(uint64_t(1) << 24);

    rig.Draw(release.handle, {0, 0}, 1.0f);
    CHECK(rig.textures.mQueued == 0);
    rig.FinishReads();

    REQUIRE(rig.textures.mQueued == 1);
    const int32_t side = release.layout.IntervalsPerTile() + 1;
    const FakeTextureUploader::Entry &entry = rig.textures.mEntries.at(0);
    CHECK(entry.mSource.desc.width == uint32_t(side));
    CHECK(entry.mSource.desc.height == uint32_t(side));
    CHECK(entry.mSource.desc.pixels.size_bytes() == size_t(side) * size_t(side) * 4);
    const TerrainResult<TerrainDecodedWeights> decoded = release.handle->ReadWeights({0, 0});
    REQUIRE(decoded);
    CHECK(std::equal(decoded.value->bytes.begin(), decoded.value->bytes.end(), entry.mSource.desc.pixels.begin()));

    const TerrainSlotTable defaults;
    CHECK(rig.residency.Words(TerrainUnitKind::Tile, release.Key({0, 0}), defaults) == kNoTerrainDrawWords);

    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();

    const std::array<uint32_t, 4> words = rig.residency.Words(TerrainUnitKind::Tile, release.Key({0, 0}), defaults);
    CHECK(words[0] == 1);
    CHECK(words[1] == kSamplerRow);
    CHECK(words[2] == 0xFFFF0000u);
    CHECK(std::bit_cast<float>(words[3]) == 1.0f / float(release.layout.TileSize()));

    TerrainSlotTable custom;
    const uint8_t table[2] = {3, 2};
    custom.Set(table);
    const std::array<uint32_t, 4> mapped = rig.residency.Words(TerrainUnitKind::Tile, release.Key({0, 0}), custom);
    CHECK(mapped[2] == 0xFFFF0203u);

    const std::optional<WorldDrawItem> draw =
        MakeTerrainDraw(TerrainResidentMesh{}, TerrainPlacement{release.asset, WorldPosition{}}, kNullMaterial,
                        kTerrainDiagnosticTint, words);
    REQUIRE(draw);
    CHECK(draw->terrain == words);

    rig.Draw(release.handle, {0, 0}, 1.0f);
    rig.FinishReads();
    CHECK(rig.textures.mQueued == 1);
}

TEST_CASE("Leaving the draw set and a changed release payload retire the texture", "[terrain_runtime][weights]")
{
    WeightedRelease first(0xE3, {0.5f, 0.5f});
    WeightedRelease sameWeights(0xE4, {0.5f, 0.5f});
    WeightedRelease changed(0xE5, {0.25f, 0.75f});
    Rig rig(uint64_t(1) << 24);
    const TerrainSlotTable slots;

    rig.Draw(first.handle, {0, 0}, 1.0f);
    rig.FinishReads();
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();
    REQUIRE(rig.residency.Words(TerrainUnitKind::Tile, first.Key({0, 0}), slots) != kNoTerrainDrawWords);

    rig.Draw(sameWeights.handle, {0, 0}, 1.0f);
    rig.FinishReads();
    CHECK(rig.textures.mQueued == 1);
    CHECK(rig.textures.mRetired.empty());
    CHECK(rig.residency.Words(TerrainUnitKind::Tile, first.Key({0, 0}), slots) != kNoTerrainDrawWords);

    rig.Draw(changed.handle, {0, 0}, 1.0f);
    CHECK(rig.textures.mRetired.size() == 1);
    CHECK(rig.residency.Words(TerrainUnitKind::Tile, first.Key({0, 0}), slots) == kNoTerrainDrawWords);
    rig.FinishReads();
    CHECK(rig.textures.mQueued == 2);
    rig.textures.Complete(TextureHandle{1, 1});
    rig.Install();
    const std::array<uint32_t, 4> words = rig.residency.Words(TerrainUnitKind::Tile, first.Key({0, 0}), slots);
    CHECK(words[0] == 2);

    rig.Advance(5.0);
    rig.Begin();
    rig.Install();
    CHECK(rig.textures.mRetired.size() == 2);
    CHECK(rig.residency.Words(TerrainUnitKind::Tile, first.Key({0, 0}), slots) == kNoTerrainDrawWords);
}

TEST_CASE("A tile keeps its texture while its mesh would still be resident", "[terrain_runtime][weights]")
{
    WeightedRelease release(0xE9, {0.5f, 0.5f});
    Rig rig(uint64_t(1) << 24);
    const TerrainSlotTable slots;

    rig.Draw(release.handle, {0, 0}, 1.0f);
    rig.FinishReads();
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();
    REQUIRE(rig.residency.Words(TerrainUnitKind::Tile, release.Key({0, 0}), slots) != kNoTerrainDrawWords);

    rig.Advance(1.0);
    rig.Begin();
    rig.Install();
    CHECK(rig.textures.mQueued == 1);
    CHECK(rig.textures.mRetired.empty());
    CHECK(rig.executor->RunAll() == 0);
    CHECK(rig.residency.Words(TerrainUnitKind::Tile, release.Key({0, 0}), slots) != kNoTerrainDrawWords);

    rig.Advance(4.0);
    rig.Begin();
    rig.Install();
    CHECK(rig.textures.mRetired.size() == 1);
    CHECK(rig.residency.Words(TerrainUnitKind::Tile, release.Key({0, 0}), slots) == kNoTerrainDrawWords);
}

TEST_CASE("Budget pressure retires the farthest texture before a nearer tile starts", "[terrain_runtime][weights]")
{
    WeightedRelease release(0xEA, {0.5f, 0.5f});
    const uint64_t side = uint64_t(release.layout.IntervalsPerTile() + 1);
    Rig rig(side * side * 4);
    const TerrainSlotTable slots;

    rig.Draw(release.handle, {-2, -1}, 100.0f);
    rig.FinishReads();
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();
    REQUIRE(rig.residency.Words(TerrainUnitKind::Tile, release.Key({-2, -1}), slots) != kNoTerrainDrawWords);

    rig.Draw(release.handle, {0, 0}, 1.0f);
    CHECK(rig.textures.mRetired.size() == 1);
    CHECK(rig.residency.Words(TerrainUnitKind::Tile, release.Key({-2, -1}), slots) == kNoTerrainDrawWords);
    rig.FinishReads();
    rig.textures.Complete(TextureHandle{1, 1});
    rig.Install();

    CHECK(rig.residency.Words(TerrainUnitKind::Tile, release.Key({0, 0}), slots) != kNoTerrainDrawWords);
    CHECK(rig.residency.Words(TerrainUnitKind::Tile, release.Key({-2, -1}), slots) == kNoTerrainDrawWords);
}

TEST_CASE("The weight budget goes to the nearest tiles and the rest draw without weights",
          "[terrain_runtime][weights]")
{
    WeightedRelease release(0xE6, {0.5f, 0.5f});
    const uint64_t side = uint64_t(release.layout.IntervalsPerTile() + 1);
    Rig rig(side * side * 4);
    const TerrainSlotTable slots;

    rig.Begin();
    rig.residency.Request({release.handle, {-2, -1}, 100.0f});
    rig.residency.Request({release.handle, {0, 0}, 1.0f});
    rig.Install();

    CHECK(rig.residency.Stats().reading == 1);
    CHECK(rig.residency.Stats().skippedBudget == 1);
    rig.executor->RunAll();
    rig.Install();
    CHECK(rig.textures.mQueued == 1);
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();

    CHECK(rig.residency.Words(TerrainUnitKind::Tile, release.Key({0, 0}), slots) != kNoTerrainDrawWords);
    CHECK(rig.residency.Words(TerrainUnitKind::Tile, release.Key({-2, -1}), slots) == kNoTerrainDrawWords);
}

TEST_CASE("A release without weights queues nothing and Regions carry no words", "[terrain_runtime][weights]")
{
    PlainRelease plain(0xE7);
    WeightedRelease weighted(0xE8, {0.5f, 0.5f});
    Rig rig(uint64_t(1) << 24);
    const TerrainSlotTable slots;

    rig.Draw(plain.handle, {0, 0}, 1.0f);
    CHECK(rig.executor->RunAll() == 0);
    rig.Install();
    CHECK(rig.textures.mQueued == 0);
    CHECK(rig.residency.Words(TerrainUnitKind::Tile, plain.Key({0, 0}), slots) == kNoTerrainDrawWords);

    rig.Draw(weighted.handle, {0, 0}, 1.0f);
    rig.FinishReads();
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();

    CHECK(rig.residency.Words(TerrainUnitKind::Tile, weighted.Key({0, 0}), slots) != kNoTerrainDrawWords);
    CHECK(rig.residency.Words(TerrainUnitKind::Region, weighted.Key({0, 0}), slots) == kNoTerrainDrawWords);
}

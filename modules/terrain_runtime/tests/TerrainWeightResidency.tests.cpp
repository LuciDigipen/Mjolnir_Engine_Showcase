/**
 * @file TerrainWeightResidency.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain_runtime/TerrainWeightResidency: weight textures of drawn tiles and Regions.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainPackTestSupport.h"

#include <terrain_runtime/TerrainRender.h>
#include <terrain_runtime/TerrainWeightResidency.h>

#include <renderer/TerrainDrawData.h>
#include <terrain/TerrainOverlay.h>
#include <terrain/TerrainRegionWeights.h>

#include "support/FakeTextureUploader.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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

    TerrainWeightUnit TileUnit(AssetId asset, TileCoord tile)
    {
        return {asset, TerrainUnitKind::Tile, 0, tile};
    }

    TerrainWeightUnit RegionUnit(AssetId asset, TerrainRegionCoord region)
    {
        return {asset, TerrainUnitKind::Region, region.level, {region.x, region.z}};
    }

    uint32_t SlotWord(const std::array<uint8_t, kTerrainDrawSlots> &slots)
    {
        return uint32_t(slots[0]) | (uint32_t(slots[1]) << 8) | (uint32_t(slots[2]) << 16) |
               (uint32_t(slots[3]) << 24);
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
    };

    struct RegionWeightedRelease
    {
        TerrainLayout layout = EdgeLayout();
        std::vector<TileHeights> tiles = BuildTiles(layout);
        std::vector<std::string> table{"dirt", "sand"};
        AssetId asset = TerrainAsset("terrain/region-weighted.terrain.json");
        std::vector<TerrainWeightTile> weights;
        TerrainRegionWrites regions = BuildTestRegions(layout, tiles, 2, 2);
        std::vector<TerrainRegionWeightImage> images;
        TerrainEncodedRelease encoded;
        std::shared_ptr<TerrainMemorySource> source;
        std::shared_ptr<const TerrainReleaseHandle> handle;

        RegionWeightedRelease(uint64_t release, std::vector<float> split)
        {
            weights = ConstantWeights(layout, table, split);
            REQUIRE(regions.grid);
            std::unordered_map<TileCoord, const TerrainWeightTile *> byTile;
            for (const TerrainWeightTile &tile : weights)
                byTile[tile.tile] = &tile;
            TerrainResult<std::vector<TerrainRegionWeightImage>> built = BuildTerrainRegionWeights(
                *regions.grid, table,
                [&](TileCoord tile) {
                    const auto found = byTile.find(tile);
                    return found == byTile.end() ? nullptr : found->second;
                },
                {.side = 3});
            INFO(built.error.message);
            REQUIRE(built);
            images = std::move(*built.value);

            TerrainWeightsWrites writes;
            writes.layers = table;
            writes.fingerprint = 0x2B2B;
            writes.tiles = weights;
            writes.regionImages = images;
            encoded = EncodeWithRegionsAndWeights(Info(asset, release), layout, tiles, regions, writes,
                                                  {.chunkTiles = 2});
            OpenedRelease opened = OpenInMemory(encoded);
            source = opened.source;
            handle = std::make_shared<const TerrainReleaseHandle>(std::move(opened.handle));
        }

        TerrainRegionCoord Region() const
        {
            return regions.grid->AtOrdinal(0);
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
            DrawUnit(release, TileUnit(release->Info().asset, tile), distance);
        }

        void DrawUnit(const std::shared_ptr<const TerrainReleaseHandle> &release, const TerrainWeightUnit &unit,
                      float distance, std::shared_ptr<const TerrainOverlay> overlay = {})
        {
            Begin();
            residency.Request({release, unit, distance, std::move(overlay)});
            Install();
        }

        void FinishReads()
        {
            executor->RunAll();
            Install();
        }
    };

    using OverlayWeightMap = std::unordered_map<TileCoord, std::shared_ptr<const TerrainDecodedWeights>>;

    std::shared_ptr<const TerrainDecodedWeights> OverlayWeights(
        TileCoord tile, int32_t intervals, std::array<uint8_t, kTerrainWeightSlots> slots,
        std::array<std::string, kTerrainWeightSlots> layers, uint8_t first)
    {
        auto weights = std::make_shared<TerrainDecodedWeights>();
        weights->tile = tile;
        weights->intervals = intervals;
        weights->slots = slots;
        weights->layers = std::move(layers);
        const int32_t side = intervals + 1;
        weights->bytes.resize(size_t(side) * size_t(side) * kTerrainWeightSlots);
        for (int32_t z = 0; z < side; ++z)
            for (int32_t x = 0; x < side; ++x)
            {
                const size_t texel = (size_t(z) * size_t(side) + size_t(x)) * kTerrainWeightSlots;
                weights->bytes[texel] = std::byte(first);
                weights->bytes[texel + 1] = std::byte(255 - first);
            }
        return weights;
    }

    std::shared_ptr<const TerrainOverlay> OverlayWith(AssetId asset, uint64_t revision, OverlayWeightMap weights)
    {
        return std::make_shared<const TerrainOverlay>(
            asset, revision, std::unordered_map<TileCoord, std::shared_ptr<const TileHeights>>{}, std::move(weights));
    }
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
    CHECK(rig.residency.Words(TileUnit(release.asset, {0, 0}), defaults) == kNoTerrainDrawWords);

    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();

    const std::array<uint32_t, 4> words = rig.residency.Words(TileUnit(release.asset, {0, 0}), defaults);
    CHECK(words[0] == 1);
    CHECK(words[1] == kSamplerRow);
    CHECK(words[2] == 0xFFFF0000u);
    CHECK(std::bit_cast<float>(words[3]) == 1.0f / float(release.layout.TileSize()));

    TerrainSlotTable custom;
    const uint8_t table[2] = {3, 2};
    custom.Set(table);
    const std::array<uint32_t, 4> mapped = rig.residency.Words(TileUnit(release.asset, {0, 0}), custom);
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

TEST_CASE("A drawn Region queues its weight image and draws with its words", "[terrain_runtime][weights]")
{
    RegionWeightedRelease release(0xE3, {0.5f, 0.5f});
    Rig rig(uint64_t(1) << 24);
    const TerrainRegionCoord region = release.Region();
    const TerrainWeightUnit unit = RegionUnit(release.asset, region);

    rig.DrawUnit(release.handle, unit, 1.0f);
    CHECK(rig.textures.mQueued == 0);
    rig.FinishReads();
    REQUIRE(rig.textures.mQueued == 1);

    const TerrainResult<TerrainDecodedWeights> decoded = release.handle->ReadRegionWeights(region);
    REQUIRE(decoded);
    const uint32_t side = decoded.value->intervals + 1;
    const FakeTextureUploader::Entry &entry = rig.textures.mEntries.at(0);
    CHECK(entry.mSource.desc.width == side);
    CHECK(entry.mSource.desc.height == side);
    CHECK(entry.mSource.desc.pixels.size_bytes() == size_t(side) * size_t(side) * 4);
    CHECK(std::equal(decoded.value->bytes.begin(), decoded.value->bytes.end(), entry.mSource.desc.pixels.begin()));

    const TerrainSlotTable defaults;
    CHECK(rig.residency.Words(unit, defaults) == kNoTerrainDrawWords);

    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();

    const std::array<uint32_t, 4> words = rig.residency.Words(unit, defaults);
    const TerrainLocalBounds rect = release.handle->Index().regionGrid->Rect(region);
    CHECK(words[0] == 1);
    CHECK(words[1] == kSamplerRow);
    CHECK(words[2] == SlotWord(defaults.Map(decoded.value->slots)));
    CHECK(std::bit_cast<float>(words[3]) == float(1.0 / (rect.maxX - rect.minX)));
    CHECK(rig.residency.Stats().residentRegions == 1);

    TerrainSlotTable custom;
    const uint8_t table[2] = {3, 2};
    custom.Set(table);
    const std::array<uint32_t, 4> mapped = rig.residency.Words(unit, custom);
    CHECK(mapped[2] == SlotWord(custom.Map(decoded.value->slots)));
}

TEST_CASE("A tile and a Region with the same coordinates keep separate images", "[terrain_runtime][weights]")
{
    RegionWeightedRelease release(0xE4, {0.5f, 0.5f});
    Rig rig(uint64_t(1) << 24);
    const TerrainRegionCoord region = release.Region();
    const TerrainWeightUnit tileUnit = TileUnit(release.asset, {0, 0});
    const TerrainWeightUnit regionUnit = RegionUnit(release.asset, region);

    rig.Begin();
    rig.residency.Request({release.handle, tileUnit, 1.0f});
    rig.residency.Request({release.handle, regionUnit, 1.0f});
    rig.Install();
    rig.FinishReads();
    REQUIRE(rig.textures.mQueued == 2);
    rig.textures.Complete(TextureHandle{0, 1});
    rig.textures.Complete(TextureHandle{1, 1});
    rig.Install();

    const TerrainSlotTable slots;
    const std::array<uint32_t, 4> tile = rig.residency.Words(tileUnit, slots);
    const std::array<uint32_t, 4> drawnRegion = rig.residency.Words(regionUnit, slots);
    CHECK(tile != kNoTerrainDrawWords);
    CHECK(drawnRegion != kNoTerrainDrawWords);
    CHECK(tile[0] != drawnRegion[0]);
    CHECK(rig.residency.Stats().ready == 2);
    CHECK(rig.residency.Stats().residentRegions == 1);
}

TEST_CASE("Leaving the draw set and a changed release payload retire the texture", "[terrain_runtime][weights]")
{
    WeightedRelease first(0xE5, {0.5f, 0.5f});
    WeightedRelease sameWeights(0xE6, {0.5f, 0.5f});
    WeightedRelease changed(0xE7, {0.25f, 0.75f});
    Rig rig(uint64_t(1) << 24);
    const TerrainSlotTable slots;
    const TerrainWeightUnit unit = TileUnit(first.asset, {0, 0});

    rig.Draw(first.handle, {0, 0}, 1.0f);
    rig.FinishReads();
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();
    REQUIRE(rig.residency.Words(unit, slots) != kNoTerrainDrawWords);

    rig.Draw(sameWeights.handle, {0, 0}, 1.0f);
    rig.FinishReads();
    CHECK(rig.textures.mQueued == 1);
    CHECK(rig.textures.mRetired.empty());
    CHECK(rig.residency.Words(unit, slots) != kNoTerrainDrawWords);

    rig.Draw(changed.handle, {0, 0}, 1.0f);
    CHECK(rig.textures.mRetired.size() == 1);
    CHECK(rig.residency.Words(unit, slots) == kNoTerrainDrawWords);
    rig.FinishReads();
    CHECK(rig.textures.mQueued == 2);
    rig.textures.Complete(TextureHandle{1, 1});
    rig.Install();
    const std::array<uint32_t, 4> words = rig.residency.Words(unit, slots);
    CHECK(words[0] == 2);

    rig.Advance(5.0);
    rig.Begin();
    rig.Install();
    CHECK(rig.textures.mRetired.size() == 2);
    CHECK(rig.residency.Words(unit, slots) == kNoTerrainDrawWords);
}

TEST_CASE("A Region image rereads only when its release carries a different Region checksum",
          "[terrain_runtime][weights]")
{
    RegionWeightedRelease first(0xE8, {0.5f, 0.5f});
    RegionWeightedRelease sameWeights(0xE9, {0.5f, 0.5f});
    RegionWeightedRelease changed(0xEA, {0.25f, 0.75f});
    Rig rig(uint64_t(1) << 24);
    const TerrainSlotTable slots;
    const TerrainWeightUnit unit = RegionUnit(first.asset, first.Region());

    rig.DrawUnit(first.handle, unit, 1.0f);
    rig.FinishReads();
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();
    REQUIRE(rig.residency.Words(unit, slots) != kNoTerrainDrawWords);

    rig.DrawUnit(sameWeights.handle, unit, 1.0f);
    rig.FinishReads();
    CHECK(rig.textures.mQueued == 1);
    CHECK(rig.textures.mRetired.empty());
    CHECK(rig.residency.Words(unit, slots) != kNoTerrainDrawWords);

    rig.DrawUnit(changed.handle, unit, 1.0f);
    CHECK(rig.textures.mRetired.size() == 1);
    CHECK(rig.residency.Words(unit, slots) == kNoTerrainDrawWords);
    rig.FinishReads();
    CHECK(rig.textures.mQueued == 2);
    rig.textures.Complete(TextureHandle{1, 1});
    rig.Install();
    const std::array<uint32_t, 4> words = rig.residency.Words(unit, slots);
    CHECK(words[0] == 2);

    rig.Advance(5.0);
    rig.Begin();
    rig.Install();
    CHECK(rig.textures.mRetired.size() == 2);
    CHECK(rig.residency.Words(unit, slots) == kNoTerrainDrawWords);
    CHECK(rig.residency.Stats().residentRegions == 0);
}

TEST_CASE("A tile keeps its texture while its mesh would still be resident", "[terrain_runtime][weights]")
{
    WeightedRelease release(0xEB, {0.5f, 0.5f});
    Rig rig(uint64_t(1) << 24);
    const TerrainSlotTable slots;
    const TerrainWeightUnit unit = TileUnit(release.asset, {0, 0});

    rig.Draw(release.handle, {0, 0}, 1.0f);
    rig.FinishReads();
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();
    REQUIRE(rig.residency.Words(unit, slots) != kNoTerrainDrawWords);

    rig.Advance(1.0);
    rig.Begin();
    rig.Install();
    CHECK(rig.textures.mQueued == 1);
    CHECK(rig.textures.mRetired.empty());
    CHECK(rig.executor->RunAll() == 0);
    CHECK(rig.residency.Words(unit, slots) != kNoTerrainDrawWords);

    rig.Advance(4.0);
    rig.Begin();
    rig.Install();
    CHECK(rig.textures.mRetired.size() == 1);
    CHECK(rig.residency.Words(unit, slots) == kNoTerrainDrawWords);
}

TEST_CASE("Budget pressure retires the farthest texture before a nearer tile starts", "[terrain_runtime][weights]")
{
    WeightedRelease release(0xEC, {0.5f, 0.5f});
    const uint64_t side = uint64_t(release.layout.IntervalsPerTile() + 1);
    Rig rig(side * side * 4);
    const TerrainSlotTable slots;

    rig.Draw(release.handle, {-2, -1}, 100.0f);
    rig.FinishReads();
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();
    REQUIRE(rig.residency.Words(TileUnit(release.asset, {-2, -1}), slots) != kNoTerrainDrawWords);

    rig.Draw(release.handle, {0, 0}, 1.0f);
    CHECK(rig.textures.mRetired.size() == 1);
    CHECK(rig.residency.Words(TileUnit(release.asset, {-2, -1}), slots) == kNoTerrainDrawWords);
    rig.FinishReads();
    rig.textures.Complete(TextureHandle{1, 1});
    rig.Install();

    CHECK(rig.residency.Words(TileUnit(release.asset, {0, 0}), slots) != kNoTerrainDrawWords);
    CHECK(rig.residency.Words(TileUnit(release.asset, {-2, -1}), slots) == kNoTerrainDrawWords);
}

TEST_CASE("A newly visible tile takes the budget from a retained tile with a stale near distance",
          "[terrain_runtime][weights]")
{
    WeightedRelease release(0xEC, {0.5f, 0.5f});
    const uint64_t side = uint64_t(release.layout.IntervalsPerTile() + 1);
    Rig rig(side * side * 4);
    const TerrainSlotTable slots;
    const TerrainWeightUnit oldTile = TileUnit(release.asset, {-2, -1});
    const TerrainWeightUnit visible = TileUnit(release.asset, {0, 0});

    rig.DrawUnit(release.handle, oldTile, 1.0f);
    rig.FinishReads();
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();
    REQUIRE(rig.residency.Words(oldTile, slots) != kNoTerrainDrawWords);

    rig.DrawUnit(release.handle, visible, 2.0f);
    CHECK(rig.residency.Words(oldTile, slots) == kNoTerrainDrawWords);
    rig.FinishReads();
    REQUIRE(rig.textures.mQueued == 2);
    rig.textures.Complete(TextureHandle{1, 1});
    rig.Install();
    CHECK(rig.residency.Words(visible, slots) != kNoTerrainDrawWords);
}

TEST_CASE("The weight budget goes to the nearest tiles and the rest draw without weights",
          "[terrain_runtime][weights]")
{
    WeightedRelease release(0xED, {0.5f, 0.5f});
    const uint64_t side = uint64_t(release.layout.IntervalsPerTile() + 1);
    Rig rig(side * side * 4);
    const TerrainSlotTable slots;

    rig.Begin();
    rig.residency.Request({release.handle, TileUnit(release.asset, {-2, -1}), 100.0f});
    rig.residency.Request({release.handle, TileUnit(release.asset, {0, 0}), 1.0f});
    rig.Install();

    CHECK(rig.residency.Stats().reading == 1);
    CHECK(rig.residency.Stats().skippedBudget == 1);
    rig.executor->RunAll();
    rig.Install();
    CHECK(rig.textures.mQueued == 1);
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();

    CHECK(rig.residency.Words(TileUnit(release.asset, {0, 0}), slots) != kNoTerrainDrawWords);
    CHECK(rig.residency.Words(TileUnit(release.asset, {-2, -1}), slots) == kNoTerrainDrawWords);
}

TEST_CASE("Lowering the weight budget retires the farthest drawn tiles before a Region",
          "[terrain_runtime][weights]")
{
    RegionWeightedRelease release(0xEE, {0.5f, 0.5f});
    Rig rig(uint64_t(1) << 24);
    const TerrainWeightUnit region = RegionUnit(release.asset, release.Region());
    const TerrainWeightUnit nearTile = TileUnit(release.asset, {0, 0});
    const TerrainWeightUnit farTile = TileUnit(release.asset, {-2, -1});
    const TerrainSlotTable slots;

    rig.Begin();
    rig.residency.Request({release.handle, farTile, 100.0f});
    rig.residency.Request({release.handle, nearTile, 1.0f});
    rig.residency.Request({release.handle, region, 1000.0f});
    rig.Install();
    rig.FinishReads();
    REQUIRE(rig.textures.mQueued == 3);
    for (uint32_t row = 0; row < 3; ++row)
        rig.textures.Complete(TextureHandle{row, 1});
    rig.Install();
    REQUIRE(rig.residency.Stats().ready == 3);

    const uint64_t side = uint64_t(release.layout.IntervalsPerTile() + 1);
    const uint64_t tileBytes = side * side * 4;
    const uint64_t budget = rig.residency.Stats().residentBytes - tileBytes;
    rig.residency.ApplySettings(WeightGpu(budget));
    rig.Install();
    CHECK(rig.residency.Stats().residentBytes == budget);
    CHECK(rig.residency.Words(farTile, slots) == kNoTerrainDrawWords);
    CHECK(rig.residency.Words(nearTile, slots) != kNoTerrainDrawWords);
    CHECK(rig.residency.Words(region, slots) != kNoTerrainDrawWords);

    rig.residency.ApplySettings(WeightGpu(budget - tileBytes));
    rig.Install();
    CHECK(rig.residency.Stats().residentBytes == budget - tileBytes);
    CHECK(rig.residency.Words(nearTile, slots) == kNoTerrainDrawWords);
    CHECK(rig.residency.Words(region, slots) != kNoTerrainDrawWords);
    CHECK(rig.textures.mRetired.size() == 2);
    CHECK(rig.executor->RunAll() == 0);
}

TEST_CASE("A finished read evicted by a smaller weight budget never queues an upload", "[terrain_runtime][weights]")
{
    WeightedRelease release(0xEC, {0.5f, 0.5f});
    Rig rig(uint64_t(1) << 24);
    rig.Draw(release.handle, {0, 0}, 1.0f);
    REQUIRE(rig.executor->RunAll() == 1);
    REQUIRE(rig.residency.Stats().reading == 1);

    rig.residency.ApplySettings(WeightGpu(0));
    rig.Install();
    CHECK(rig.residency.Stats().residentBytes == 0);
    CHECK(rig.residency.Stats().reading == 0);
    CHECK(rig.textures.mQueued == 0);
}

TEST_CASE("Region images outrank tiles in the weight budget", "[terrain_runtime][weights]")
{
    RegionWeightedRelease release(0xEE, {0.5f, 0.5f});
    const uint64_t tileBytes = uint64_t(release.layout.IntervalsPerTile() + 1) *
                               uint64_t(release.layout.IntervalsPerTile() + 1) * 4;
    Rig rig(tileBytes);
    const TerrainSlotTable slots;
    const TerrainWeightUnit regionUnit = RegionUnit(release.asset, release.Region());

    rig.Draw(release.handle, {-2, -1}, 1.0f);
    rig.FinishReads();
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();
    REQUIRE(rig.residency.Words(TileUnit(release.asset, {-2, -1}), slots) != kNoTerrainDrawWords);

    rig.DrawUnit(release.handle, regionUnit, 1000.0f);
    CHECK(rig.textures.mRetired.size() == 1);
    CHECK(rig.residency.Words(TileUnit(release.asset, {-2, -1}), slots) == kNoTerrainDrawWords);
    rig.FinishReads();
    REQUIRE(rig.textures.mQueued == 2);
    rig.textures.Complete(TextureHandle{1, 1});
    rig.Install();

    CHECK(rig.residency.Words(regionUnit, slots) != kNoTerrainDrawWords);
    CHECK(rig.residency.Words(TileUnit(release.asset, {-2, -1}), slots) == kNoTerrainDrawWords);
}

TEST_CASE("A wanted Region image is read while a nearer tile is skipped", "[terrain_runtime][weights]")
{
    RegionWeightedRelease release(0xEF, {0.5f, 0.5f});
    const uint64_t tileBytes = uint64_t(release.layout.IntervalsPerTile() + 1) *
                               uint64_t(release.layout.IntervalsPerTile() + 1) * 4;
    Rig rig(tileBytes);
    const TerrainSlotTable slots;
    const TerrainWeightUnit regionUnit = RegionUnit(release.asset, release.Region());

    rig.Begin();
    rig.residency.Request({release.handle, TileUnit(release.asset, {-2, -1}), 1.0f});
    rig.residency.Request({release.handle, regionUnit, 1000.0f});
    rig.Install();

    CHECK(rig.residency.Stats().reading == 1);
    CHECK(rig.residency.Stats().skippedBudget == 1);
    rig.executor->RunAll();
    rig.Install();
    REQUIRE(rig.textures.mQueued == 1);
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();

    CHECK(rig.residency.Words(regionUnit, slots) != kNoTerrainDrawWords);
    CHECK(rig.residency.Words(TileUnit(release.asset, {-2, -1}), slots) == kNoTerrainDrawWords);
}

TEST_CASE("A release without weights queues nothing for tiles or Regions", "[terrain_runtime][weights]")
{
    PlainRelease plain(0xF0);
    WeightedRelease weighted(0xF1, {0.5f, 0.5f});
    Rig rig(uint64_t(1) << 24);
    const TerrainSlotTable slots;

    rig.Draw(plain.handle, {0, 0}, 1.0f);
    CHECK(rig.executor->RunAll() == 0);
    rig.Install();
    CHECK(rig.textures.mQueued == 0);
    CHECK(rig.residency.Words(TileUnit(plain.asset, {0, 0}), slots) == kNoTerrainDrawWords);

    const TerrainWeightUnit plainRegion{plain.asset, TerrainUnitKind::Region, 0, {0, 0}};
    rig.DrawUnit(plain.handle, plainRegion, 1.0f);
    CHECK(rig.executor->RunAll() == 0);
    rig.Install();
    CHECK(rig.textures.mQueued == 0);
    CHECK(rig.residency.Words(plainRegion, slots) == kNoTerrainDrawWords);

    rig.Draw(weighted.handle, {0, 0}, 1.0f);
    rig.FinishReads();
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();

    CHECK(rig.residency.Words(TileUnit(weighted.asset, {0, 0}), slots) != kNoTerrainDrawWords);
    CHECK(rig.residency.Words({weighted.asset, TerrainUnitKind::Region, 0, {0, 0}}, slots) == kNoTerrainDrawWords);
}

TEST_CASE("An overlay weight tile uploads without a read and draws its texture", "[terrain_runtime][weights]")
{
    WeightedRelease release(0xF2, {0.5f, 0.5f});
    Rig rig(uint64_t(1) << 24);
    const TileCoord tile{0, 0};
    const int32_t intervals = release.layout.IntervalsPerTile();
    const TerrainWeightUnit unit = TileUnit(release.asset, tile);
    const std::shared_ptr<const TerrainDecodedWeights> weights =
        OverlayWeights(tile, intervals, {0, 1, 0xFF, 0xFF}, {"dirt", "sand", "", ""}, 200);
    const std::shared_ptr<const TerrainOverlay> overlay = OverlayWith(release.asset, 1, {{tile, weights}});
    const TerrainByteStats before = release.source->Stats();

    rig.Begin();
    rig.residency.Request({release.handle, unit, 1.0f, overlay});
    rig.Install();
    CHECK(rig.executor->RunAll() == 0);
    CHECK(release.source->Stats().reads == before.reads);
    REQUIRE(rig.textures.mQueued == 1);

    const uint32_t side = uint32_t(intervals + 1);
    const FakeTextureUploader::Entry &entry = rig.textures.mEntries.at(0);
    CHECK(entry.mSource.desc.width == side);
    CHECK(entry.mSource.desc.height == side);
    CHECK(std::equal(weights->bytes.begin(), weights->bytes.end(), entry.mSource.desc.pixels.begin()));

    const TerrainSlotTable slots;
    CHECK(rig.residency.Words(unit, slots) == kNoTerrainDrawWords);
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();

    const std::array<uint32_t, 4> words = rig.residency.Words(unit, slots);
    CHECK(words[0] == 1);
    CHECK(words[1] == kSamplerRow);
    CHECK(words[2] == 0xFFFF0000u);
    CHECK(std::bit_cast<float>(words[3]) == 1.0f / float(release.layout.TileSize()));

    TerrainSlotTable custom;
    const uint8_t table[2] = {3, 2};
    custom.Set(table);
    CHECK(rig.residency.Words(unit, custom)[2] == 0xFFFF0203u);
}

TEST_CASE("A new overlay payload re-uploads a weight tile, a kept one does not, and dropping it returns to the "
          "release",
          "[terrain_runtime][weights]")
{
    WeightedRelease release(0xF3, {0.5f, 0.5f});
    Rig rig(uint64_t(1) << 24);
    const TerrainSlotTable slots;
    const TileCoord tile{0, 0};
    const int32_t intervals = release.layout.IntervalsPerTile();
    const TerrainWeightUnit unit = TileUnit(release.asset, tile);

    const auto first = OverlayWeights(tile, intervals, {0, 1, 0xFF, 0xFF}, {"dirt", "sand", "", ""}, 200);
    rig.DrawUnit(release.handle, unit, 1.0f, OverlayWith(release.asset, 1, {{tile, first}}));
    CHECK(rig.executor->RunAll() == 0);
    rig.textures.Complete(TextureHandle{0, 1});
    rig.Install();
    REQUIRE(rig.residency.Words(unit, slots)[0] == 1);

    rig.DrawUnit(release.handle, unit, 1.0f, OverlayWith(release.asset, 2, {{tile, first}}));
    CHECK(rig.textures.mQueued == 1);
    CHECK(rig.textures.mRetired.empty());
    CHECK(rig.residency.Words(unit, slots)[0] == 1);

    const auto changed = OverlayWeights(tile, intervals, {0, 1, 0xFF, 0xFF}, {"dirt", "sand", "", ""}, 64);
    rig.DrawUnit(release.handle, unit, 1.0f, OverlayWith(release.asset, 3, {{tile, changed}}));
    CHECK(rig.textures.mRetired.size() == 1);
    CHECK(rig.residency.Words(unit, slots) == kNoTerrainDrawWords);
    CHECK(rig.executor->RunAll() == 0);
    CHECK(rig.textures.mQueued == 2);
    rig.textures.Complete(TextureHandle{1, 1});
    rig.Install();
    CHECK(rig.residency.Words(unit, slots)[0] == 2);
    CHECK(std::equal(changed->bytes.begin(), changed->bytes.end(),
                     rig.textures.mEntries.at(1).mSource.desc.pixels.begin()));

    const TerrainByteStats before = release.source->Stats();
    rig.DrawUnit(release.handle, unit, 1.0f, OverlayWith(release.asset, 4, {}));
    CHECK(rig.textures.mRetired.size() == 2);
    CHECK(rig.residency.Words(unit, slots) == kNoTerrainDrawWords);
    CHECK(rig.executor->RunAll() == 1);
    CHECK(release.source->Stats().reads == before.reads + 1);
    rig.Install();
    CHECK(rig.textures.mQueued == 3);
    rig.textures.Complete(TextureHandle{2, 1});
    rig.Install();
    CHECK(rig.residency.Words(unit, slots)[0] == 3);

    const TerrainResult<TerrainDecodedWeights> decoded = release.handle->ReadWeights(tile);
    REQUIRE(decoded);
    CHECK(std::equal(decoded.value->bytes.begin(), decoded.value->bytes.end(),
                     rig.textures.mEntries.at(2).mSource.desc.pixels.begin()));
}

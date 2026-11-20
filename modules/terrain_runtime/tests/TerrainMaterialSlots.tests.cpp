/**
 * @file TerrainMaterialSlots.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain_runtime/TerrainMaterialSlots: release weight layers to material layers.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainPackTestSupport.h"

#include <terrain_runtime/TerrainMaterialSlots.h>
#include <terrain_runtime/TerrainWeightResidency.h>

#include <renderer/TerrainDrawData.h>

#include "support/FakeTextureUploader.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    constexpr uint32_t kSamplerRow = 11;

    TerrainStreamingSettings::Gpu WeightGpu()
    {
        TerrainStreamingSettings::Gpu gpu;
        gpu.weightBudgetBytes = uint64_t(1) << 24;
        gpu.meshRetainSeconds = 4.0;
        return gpu;
    }

    struct WeightedRelease
    {
        TerrainLayout layout = EdgeLayout();
        std::vector<TileHeights> tiles = BuildTiles(layout);
        std::vector<std::string> table;
        AssetId asset = TerrainAsset("terrain/layered.terrain.json");
        std::vector<TerrainWeightTile> weights;
        TerrainEncodedRelease encoded;
        std::shared_ptr<TerrainMemorySource> source;
        std::shared_ptr<const TerrainReleaseHandle> handle;

        WeightedRelease(uint64_t release, std::vector<std::string> layers, std::vector<float> split)
            : table(std::move(layers))
        {
            const TileRange range = layout.Tiles();
            const size_t count = size_t(layout.IntervalsPerTile() + 1) * size_t(layout.IntervalsPerTile() + 1);
            for (int32_t z = range.min.z; z <= range.max.z; ++z)
                for (int32_t x = range.min.x; x <= range.max.x; ++x)
                {
                    TerrainWeightTile tile;
                    tile.tile = {x, z};
                    tile.intervals = layout.IntervalsPerTile();
                    tile.layers = table;
                    tile.weights.resize(count * split.size());
                    for (size_t layer = 0; layer < split.size(); ++layer)
                        for (size_t sample = 0; sample < count; ++sample)
                            tile.weights[layer * count + sample] = split[layer];
                    weights.push_back(std::move(tile));
                }
            TerrainWeightsWrites writes;
            writes.layers = table;
            writes.fingerprint = 0xBEEF;
            writes.tiles = weights;
            encoded = EncodeWithWeights(Info(asset, release), layout, tiles, writes, {.chunkTiles = 2});
            OpenedRelease opened = OpenInMemory(encoded);
            source = opened.source;
            handle = std::make_shared<const TerrainReleaseHandle>(std::move(opened.handle));
        }
    };

    struct WeightRig
    {
        std::shared_ptr<TerrainManualExecutor> executor = std::make_shared<TerrainManualExecutor>();
        FakeTextureUploader textures;
        TerrainWeightResidency residency;
        double now = 0.0;

        WeightRig() : residency(WeightGpu(), executor, textures, kSamplerRow) {}

        void Prepare(const std::shared_ptr<const TerrainReleaseHandle> &release, TileCoord tile)
        {
            residency.BeginRequest();
            residency.Request({release, {release->Info().asset, TerrainUnitKind::Tile, 0, tile}, 1.0f});
            residency.Install(now);
            executor->RunAll();
            residency.Install(now);
            textures.Complete(TextureHandle{0, 1});
            residency.Install(now);
        }
    };

    std::array<uint8_t, kTerrainDrawSlots> SlotsOf(const std::shared_ptr<const TerrainReleaseHandle> &release,
                                                   const TerrainSlotTable &table)
    {
        WeightRig rig;
        rig.Prepare(release, {0, 0});
        return TerrainDrawData::Unpack(
                   rig.residency.Words({release->Info().asset, TerrainUnitKind::Tile, 0, {0, 0}}, table))
            .slots;
    }
}

TEST_CASE("Release weight layers draw with the same-named material layers", "[terrain_runtime][material]")
{
    WeightedRelease release(0xF1, {"dark_sand", "sand", "rock"}, {0.2f, 0.5f, 0.3f});
    TerrainMaterialSlotBinding binding;
    const std::vector<std::string> material{"sand", "dark_sand"};

    REQUIRE(binding.Update(release.table, material));
    CHECK(binding.Layers()[0] == 1);
    CHECK(binding.Layers()[1] == 0);
    CHECK(binding.Layers()[2] == 0);
    CHECK(binding.Unnamed() == std::vector<std::string>{"rock"});

    TerrainSlotTable slots;
    slots.Set(binding.Layers());
    const std::array<uint8_t, kTerrainDrawSlots> drawn = SlotsOf(release.handle, slots);
    CHECK(drawn[0] == 1);
    CHECK(drawn[1] == 0);
    CHECK(drawn[2] == 0);
    CHECK(drawn[3] == kTerrainSlotUnused);

    const std::string report = binding.ReportOnce("materials/x.terrainmaterial.json");
    CHECK(report.find("'rock'") != std::string::npos);
    CHECK(binding.ReportOnce("materials/x.terrainmaterial.json").empty());
}

TEST_CASE("A single material draws every weight layer with layer 0 and reports nothing", "[terrain_runtime][material]")
{
    WeightedRelease release(0xF2, {"dark_sand", "sand", "rock"}, {0.2f, 0.5f, 0.3f});
    TerrainMaterialSlotBinding binding;
    const std::vector<std::string> single;

    REQUIRE(binding.Update(release.table, single));
    CHECK(std::all_of(binding.Layers().begin(), binding.Layers().end(), [](uint8_t layer) { return layer == 0; }));
    CHECK(binding.Unnamed().empty());
    CHECK(binding.ReportOnce("materials/single.material.json").empty());

    TerrainSlotTable slots;
    slots.Set(binding.Layers());
    const std::array<uint8_t, kTerrainDrawSlots> drawn = SlotsOf(release.handle, slots);
    CHECK(drawn[0] == 0);
    CHECK(drawn[1] == 0);
    CHECK(drawn[2] == 0);
    CHECK(drawn[3] == kTerrainSlotUnused);
}

TEST_CASE("A release switch recomputes the material layers", "[terrain_runtime][material]")
{
    TerrainMaterialSlotBinding binding;
    const std::vector<std::string> material{"sand", "dark_sand"};
    const std::vector<std::string> releaseLayers{"dark_sand", "sand", "rock"};
    const std::vector<std::string> switched{"sand", "dark_sand"};
    REQUIRE(binding.Update(releaseLayers, material));
    CHECK(binding.Layers()[0] == 1);
    CHECK(binding.Layers()[1] == 0);

    REQUIRE(binding.Update(switched, material));
    CHECK(binding.Layers()[0] == 0);
    CHECK(binding.Layers()[1] == 1);
    CHECK(binding.Unnamed().empty());

    CHECK_FALSE(binding.Update(switched, material));
}

TEST_CASE("An unnamed release layer is reported once for a material and release", "[terrain_runtime][material]")
{
    TerrainMaterialSlotBinding binding;
    const std::vector<std::string> material{"sand"};
    const std::vector<std::string> unnamed{"sand", "gravel"};
    const std::vector<std::string> moreUnnamed{"sand", "gravel", "rock"};
    REQUIRE(binding.Update(unnamed, material));
    CHECK(binding.Unnamed() == std::vector<std::string>{"gravel"});

    const std::string first = binding.ReportOnce("materials/x.terrainmaterial.json");
    CHECK(first.find("'gravel'") != std::string::npos);
    CHECK(first.find("'sand'") != std::string::npos);
    CHECK(binding.ReportOnce("materials/x.terrainmaterial.json").empty());

    REQUIRE(binding.Update(moreUnnamed, material));
    const std::string next = binding.ReportOnce("materials/x.terrainmaterial.json");
    CHECK(next.find("'gravel'") != std::string::npos);
    CHECK(next.find("'rock'") != std::string::npos);
    CHECK(binding.ReportOnce("materials/x.terrainmaterial.json").empty());
}

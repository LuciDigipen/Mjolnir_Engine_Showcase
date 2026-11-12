/**
 * @file TerrainEditStore.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainEditStore.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"

#include <terrain/TerrainEditStore.h>

#include <core/fs/FileIO.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using Code = TerrainPackErrorCode;

    TerrainEditLayerInfo Info(std::string name, uint32_t sequence, TerrainEditBlend blend, bool enabled = true)
    {
        return {std::move(name), 0, sequence, enabled, blend};
    }

    TerrainEditLayer MakeLayer(std::string name, uint32_t sequence, TerrainEditBlend blend, bool enabled = true)
    {
        TerrainEditLayer layer;
        layer.info = Info(std::move(name), sequence, blend, enabled);
        return layer;
    }

    TerrainEditTile MakeTile(TileCoord coord, int32_t intervals, float value,
                             std::optional<float> alpha = std::nullopt)
    {
        TerrainEditTile tile;
        tile.tile = coord;
        tile.intervals = intervals;
        const size_t count = static_cast<size_t>(intervals + 1) * static_cast<size_t>(intervals + 1);
        tile.values.assign(count, value);
        if (alpha)
            tile.alpha.assign(count, *alpha);
        return tile;
    }

    std::vector<TerrainEditLayer> MakeStack()
    {
        TerrainEditLayer additive = MakeLayer("Ridge", 2, TerrainEditBlend::Additive);
        additive.tiles.emplace(TileCoord{0, 0}, MakeTile({0, 0}, 4, 0.5f));
        additive.tiles.emplace(TileCoord{-1, 3}, MakeTile({-1, 3}, 2, -0.25f));

        TerrainEditLayer blended = MakeLayer("Path", 0, TerrainEditBlend::AlphaBlend);
        blended.tiles.emplace(TileCoord{2, -1}, MakeTile({2, -1}, 3, 1.0f, 0.75f));

        TerrainEditLayer empty = MakeLayer("Detail", 5, TerrainEditBlend::Additive);

        TerrainEditLayer disabled = MakeLayer("Old", 1, TerrainEditBlend::AlphaBlend, false);
        disabled.tiles.emplace(TileCoord{0, 0}, MakeTile({0, 0}, 1, 0.125f, 0.5f));

        return {std::move(additive), std::move(blended), std::move(empty), std::move(disabled)};
    }

    std::filesystem::path WriteStack(const std::filesystem::path &layersDir, std::span<const TerrainEditLayer> layers)
    {
        const TerrainResult<std::vector<TerrainEditStackFile>> planned = PlanTerrainEditStackWrite(layers);
        INFO(planned.error.message);
        REQUIRE(planned);
        std::filesystem::create_directories(layersDir);
        for (const TerrainEditStackFile &file : *planned.value)
        {
            std::filesystem::create_directories((layersDir / file.path).parent_path());
            REQUIRE(WriteBytes(layersDir / file.path, file.bytes));
        }
        return layersDir;
    }

    void WriteManifest(const std::filesystem::path &layersDir, std::span<const TerrainEditLayerInfo> infos)
    {
        std::filesystem::create_directories(layersDir);
        REQUIRE(WriteText(layersDir / kTerrainEditManifestFileName, EncodeTerrainEditManifest(infos)));
    }

    std::vector<std::byte> EncodeTile(const TerrainEditTile &tile, TerrainEditBlend blend)
    {
        TerrainResult<std::vector<std::byte>> encoded = EncodeTerrainEditTile(tile, blend);
        INFO(encoded.error.message);
        REQUIRE(encoded);
        return std::move(*encoded.value);
    }

    void WriteTile(const std::filesystem::path &layersDir, uint32_t sequence, TileCoord name,
                   const TerrainEditTile &tile, TerrainEditBlend blend)
    {
        const std::filesystem::path dir = layersDir / TerrainEditLayerDirName(sequence);
        std::filesystem::create_directories(dir);
        REQUIRE(WriteBytes(dir / TerrainEditTileFileName(name), EncodeTile(tile, blend)));
    }
} // namespace

TEST_CASE("Edit layer paths name the recipe's sibling directory, layer and tile files", "[terrain][editstore]")
{
    CHECK(TerrainEditLayersDir("games/G/assets/terrain/hills.terrain.json").generic_string() ==
          "games/G/assets/terrain/hills.terrain.layers");
    CHECK(TerrainEditLayersDir("games/G/assets/deep/nested/hills.terrain.json").generic_string() ==
          "games/G/assets/deep/nested/hills.terrain.layers");
    CHECK(TerrainEditLayersDir("games\\G\\assets\\terrain\\hills.terrain.json").generic_string() ==
          "games/G/assets/terrain/hills.terrain.layers");
    CHECK(TerrainEditLayersDir("hills.terrain.json").generic_string() == "hills.terrain.layers");
    CHECK(TerrainEditLayersDir("terrain/hills.json").empty());
    CHECK(TerrainEditLayersDir("terrain/Hills.TERRAIN.JSON").empty());
    CHECK(TerrainEditLayersDir("terrain/.terrain.json").empty());

    CHECK(TerrainEditLayerDirName(0) == "L0");
    CHECK(TerrainEditLayerDirName(42) == "L42");

    CHECK(TerrainEditTileFileName({0, 0}) == "tile_0_0.tedit");
    CHECK(TerrainEditTileFileName({-1, 3}) == "tile_-1_3.tedit");
    CHECK(TerrainEditTileFileName({std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()}) ==
          "tile_-2147483648_2147483647.tedit");

    CHECK(ParseTerrainEditTileFileName("tile_0_0.tedit") == TileCoord{0, 0});
    CHECK(ParseTerrainEditTileFileName("tile_-7_-12.tedit") == TileCoord{-7, -12});
    CHECK(ParseTerrainEditTileFileName("tile_-2147483648_2147483647.tedit") ==
          TileCoord{std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()});

    for (const std::string_view bad : {
             "", "tile_1_2", "tile_1_2.tedit.bak", "tile_1_.tedit", "tile__2.tedit", "tile_.tedit",
             "tile_+1_2.tedit", "tile_1_-0.tedit", "tile_01_2.tedit", "tile_1_02.tedit", "tile_1_2_3.tedit",
             "tile_1_2.TEDIT", "TILE_1_2.tedit", "xtile_1_2.tedit", "tile_1_2147483648.tedit",
             "tile_1_2.tedit\n",
         })
    {
        INFO(bad);
        CHECK_FALSE(ParseTerrainEditTileFileName(bad));
    }
}

TEST_CASE("A planned Edit Layer stack loads back equal to the original", "[terrain][editstore]")
{
    const std::filesystem::path root = MakeScratchDir("editstore_roundtrip");
    const std::vector<TerrainEditLayer> stack = MakeStack();

    const TerrainResult<std::vector<TerrainEditStackFile>> planned = PlanTerrainEditStackWrite(stack);
    INFO(planned.error.message);
    REQUIRE(planned);
    REQUIRE(planned.value->size() == 5);
    CHECK((*planned.value)[0].path.generic_string() == "layers.json");
    CHECK((*planned.value)[1].path.generic_string() == "L2/tile_0_0.tedit");
    CHECK((*planned.value)[2].path.generic_string() == "L2/tile_-1_3.tedit");
    CHECK((*planned.value)[3].path.generic_string() == "L0/tile_2_-1.tedit");
    CHECK((*planned.value)[4].path.generic_string() == "L1/tile_0_0.tedit");

    const std::filesystem::path layersDir =
        WriteStack(TerrainEditLayersDir(root / "nested" / "hills.terrain.json"), stack);
    CHECK(layersDir == root / "nested" / "hills.terrain.layers");
    CHECK(std::filesystem::exists(layersDir / "layers.json"));
    CHECK(std::filesystem::exists(layersDir / "L2" / "tile_0_0.tedit"));
    CHECK(std::filesystem::exists(layersDir / "L2" / "tile_-1_3.tedit"));
    CHECK(std::filesystem::exists(layersDir / "L0" / "tile_2_-1.tedit"));
    CHECK(std::filesystem::exists(layersDir / "L1" / "tile_0_0.tedit"));
    CHECK_FALSE(std::filesystem::exists(layersDir / "L5"));

    const TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(layersDir);
    INFO(loaded.error.message);
    REQUIRE(loaded);
    CHECK(*loaded.value == stack);
    std::filesystem::remove_all(root);
}

TEST_CASE("A missing Edit Layer directory or layer directory contributes no tiles", "[terrain][editstore]")
{
    const std::filesystem::path root = MakeScratchDir("editstore_missing");
    const TerrainResult<std::vector<TerrainEditLayer>> absent = LoadTerrainEditStack(root / "hills.terrain.layers");
    INFO(absent.error.message);
    REQUIRE(absent);
    CHECK(absent.value->empty());

    const std::filesystem::path layersDir = root / "hills.terrain.layers";
    WriteManifest(layersDir, std::vector<TerrainEditLayerInfo>{Info("Lonely", 7, TerrainEditBlend::Additive)});
    const TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(layersDir);
    INFO(loaded.error.message);
    REQUIRE(loaded);
    REQUIRE(loaded.value->size() == 1);
    CHECK(loaded.value->front().info.name == "Lonely");
    CHECK(loaded.value->front().tiles.empty());
    std::filesystem::remove_all(root);
}

TEST_CASE("Loading an Edit Layer stack refuses a missing or invalid manifest", "[terrain][editstore]")
{
    {
        const std::filesystem::path dir = MakeScratchDir("editstore_no_manifest");
        std::filesystem::create_directories(dir / "L0");
        const TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(dir);
        CHECK_FALSE(loaded);
        CHECK(loaded.error.code == Code::Io);
        CHECK(Mentions(loaded.error, "layers.json"));
        std::filesystem::remove_all(dir);
    }
    {
        const std::filesystem::path dir = MakeScratchDir("editstore_bad_manifest");
        std::filesystem::create_directories(dir);
        REQUIRE(WriteText(dir / kTerrainEditManifestFileName, "[]"));
        const TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(dir);
        CHECK_FALSE(loaded);
        CHECK(loaded.error.code == Code::Malformed);
        CHECK(Mentions(loaded.error, "layers.json"));
        std::filesystem::remove_all(dir);
    }
}

TEST_CASE("Loading an Edit Layer stack refuses a repeated sequence", "[terrain][editstore]")
{
    const std::filesystem::path dir = MakeScratchDir("editstore_duplicate");
    WriteManifest(dir, std::vector<TerrainEditLayerInfo>{Info("First", 3, TerrainEditBlend::Additive),
                                                        Info("Second", 3, TerrainEditBlend::AlphaBlend)});
    const TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(dir);
    CHECK_FALSE(loaded);
    CHECK(loaded.error.code == Code::Malformed);
    CHECK(Mentions(loaded.error, "layers.json"));
    CHECK(Mentions(loaded.error, "sequence 3"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("Loading an Edit Layer stack refuses tiles that disagree with their name or layer", "[terrain][editstore]")
{
    {
        const std::filesystem::path dir = MakeScratchDir("editstore_coords");
        WriteManifest(dir, std::vector<TerrainEditLayerInfo>{Info("A", 0, TerrainEditBlend::Additive)});
        WriteTile(dir, 0, TileCoord{1, 3}, MakeTile({2, 3}, 2, 1.0f), TerrainEditBlend::Additive);
        const TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(dir);
        CHECK_FALSE(loaded);
        CHECK(loaded.error.code == Code::Corrupt);
        CHECK(Mentions(loaded.error, "tile_1_3.tedit"));
        std::filesystem::remove_all(dir);
    }
    {
        const std::filesystem::path dir = MakeScratchDir("editstore_blend");
        WriteManifest(dir, std::vector<TerrainEditLayerInfo>{Info("A", 0, TerrainEditBlend::Additive)});
        WriteTile(dir, 0, TileCoord{0, 0}, MakeTile({0, 0}, 2, 1.0f, 0.5f), TerrainEditBlend::AlphaBlend);
        const TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(dir);
        CHECK_FALSE(loaded);
        CHECK(loaded.error.code == Code::Corrupt);
        CHECK(Mentions(loaded.error, "tile_0_0.tedit"));
        std::filesystem::remove_all(dir);
    }
}

TEST_CASE("Loading an Edit Layer stack refuses a corrupt tile blob", "[terrain][editstore]")
{
    const std::filesystem::path dir = MakeScratchDir("editstore_corrupt");
    WriteManifest(dir, std::vector<TerrainEditLayerInfo>{Info("A", 0, TerrainEditBlend::Additive)});
    std::vector<std::byte> bytes = EncodeTile(MakeTile({0, 0}, 2, 1.0f), TerrainEditBlend::Additive);
    bytes.back() ^= std::byte{0x01};
    std::filesystem::create_directories(dir / "L0");
    REQUIRE(WriteBytes(dir / "L0" / "tile_0_0.tedit", bytes));

    const TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(dir);
    CHECK_FALSE(loaded);
    CHECK(loaded.error.code == Code::Corrupt);
    CHECK(Mentions(loaded.error, "tile_0_0.tedit"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("Loading an Edit Layer stack refuses stray files and directories", "[terrain][editstore]")
{
    {
        const std::filesystem::path dir = MakeScratchDir("editstore_stray_layer");
        WriteStack(dir, MakeStack());
        REQUIRE(WriteText(dir / "L2" / "notes.txt", "x"));
        const TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(dir);
        CHECK_FALSE(loaded);
        CHECK(loaded.error.code == Code::Malformed);
        CHECK(Mentions(loaded.error, "notes.txt"));
        std::filesystem::remove_all(dir);
    }
    {
        const std::filesystem::path dir = MakeScratchDir("editstore_stray_tile_dir");
        WriteStack(dir, MakeStack());
        std::filesystem::create_directories(dir / "L2" / "tile_9_9.tedit");
        const TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(dir);
        CHECK_FALSE(loaded);
        CHECK(loaded.error.code == Code::Malformed);
        CHECK(Mentions(loaded.error, "tile_9_9.tedit"));
        std::filesystem::remove_all(dir);
    }
    {
        const std::filesystem::path dir = MakeScratchDir("editstore_stray_root");
        WriteStack(dir, MakeStack());
        std::filesystem::create_directories(dir / "L9");
        const TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(dir);
        CHECK_FALSE(loaded);
        CHECK(loaded.error.code == Code::Malformed);
        CHECK(Mentions(loaded.error, "L9"));
        std::filesystem::remove_all(dir);
    }
    {
        const std::filesystem::path dir = MakeScratchDir("editstore_stray_root_file");
        WriteStack(dir, MakeStack());
        REQUIRE(WriteText(dir / "layers.json.bak", "x"));
        const TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(dir);
        CHECK_FALSE(loaded);
        CHECK(loaded.error.code == Code::Malformed);
        CHECK(Mentions(loaded.error, "layers.json.bak"));
        std::filesystem::remove_all(dir);
    }
}

TEST_CASE("The write plan refuses duplicate sequences and unencodable tiles", "[terrain][editstore]")
{
    std::vector<TerrainEditLayer> duplicate;
    duplicate.push_back(MakeLayer("A", 4, TerrainEditBlend::Additive));
    duplicate.push_back(MakeLayer("B", 4, TerrainEditBlend::AlphaBlend));
    const TerrainResult<std::vector<TerrainEditStackFile>> repeated = PlanTerrainEditStackWrite(duplicate);
    CHECK_FALSE(repeated);
    CHECK(repeated.error.code == Code::InvalidInput);
    CHECK(Mentions(repeated.error, "sequence 4"));

    TerrainEditLayer wrongCount = MakeLayer("Bad", 1, TerrainEditBlend::Additive);
    TerrainEditTile shortTile = MakeTile({0, 0}, 2, 1.0f);
    shortTile.values.pop_back();
    wrongCount.tiles.emplace(TileCoord{0, 0}, shortTile);
    const TerrainResult<std::vector<TerrainEditStackFile>> shortPlan =
        PlanTerrainEditStackWrite(std::span(&wrongCount, 1));
    CHECK_FALSE(shortPlan);
    CHECK(shortPlan.error.code == Code::InvalidInput);
    CHECK(Mentions(shortPlan.error, "tile_0_0.tedit"));

    TerrainEditLayer nonFinite = MakeLayer("Bad", 2, TerrainEditBlend::Additive);
    nonFinite.tiles.emplace(TileCoord{1, 1}, MakeTile({1, 1}, 1, std::numeric_limits<float>::quiet_NaN()));
    const TerrainResult<std::vector<TerrainEditStackFile>> badPlan =
        PlanTerrainEditStackWrite(std::span(&nonFinite, 1));
    CHECK_FALSE(badPlan);
    CHECK(badPlan.error.code == Code::InvalidInput);
    CHECK(Mentions(badPlan.error, "tile_1_1.tedit"));
}

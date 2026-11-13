/**
 * @file TerrainLayerDocument.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain_editor/TerrainLayerDocument.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainLayerDocument.h>

#include <terrain/TerrainEditStore.h>

#include <core/fs/FileIO.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using mir::LoadTerrainEditStack;
    using mir::TerrainEditBlend;
    using mir::TerrainEditLayer;
    using mir::TerrainEditLayerInfo;
    using mir::TerrainEditTile;
    using mir::TerrainLayerDocument;
    using mir::TileCoord;

    using Files = std::map<std::string, std::string>;

    std::filesystem::path MakeScratchDir(std::string_view tag)
    {
        static std::random_device random;
        const std::filesystem::path dir = std::filesystem::temp_directory_path() /
            "mir_terrain_editor_save_tests" / (std::string(tag) + "_" + std::to_string(random()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }

    std::filesystem::path ScratchRecipe(const std::filesystem::path &root)
    {
        return root / "hills.terrain.json";
    }

    TerrainEditTile Tile(TileCoord coord, int32_t intervals, float value)
    {
        TerrainEditTile tile;
        tile.tile = coord;
        tile.intervals = intervals;
        const std::size_t count = static_cast<std::size_t>(intervals + 1) * static_cast<std::size_t>(intervals + 1);
        tile.values.assign(count, value);
        return tile;
    }

    TerrainEditLayer Layer(std::string name, uint32_t sequence, TerrainEditBlend blend)
    {
        TerrainEditLayer layer;
        layer.info = TerrainEditLayerInfo{ std::move(name), 0, sequence, true, blend };
        if (blend == TerrainEditBlend::AlphaBlend)
        {
            TerrainEditTile tile = Tile({ 1, -2 }, 2, 0.5f);
            tile.alpha.assign(tile.values.size(), 0.25f);
            layer.tiles.emplace(tile.tile, std::move(tile));
        }
        else
            layer.tiles.emplace(TileCoord{ 0, 0 }, Tile({ 0, 0 }, 1, -0.5f));
        return layer;
    }

    TerrainEditLayer EmptyLayer(std::string name, uint32_t sequence)
    {
        TerrainEditLayer layer;
        layer.info = TerrainEditLayerInfo{ std::move(name), 0, sequence, true, TerrainEditBlend::Additive };
        return layer;
    }

    Files ReadTree(const std::filesystem::path &root)
    {
        Files files;
        for (const std::filesystem::directory_entry &entry :
            std::filesystem::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
                continue;

            std::ifstream file(entry.path(), std::ios::binary);
            files[std::filesystem::relative(entry.path(), root).generic_string()] =
                std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        return files;
    }
} // namespace

TEST_CASE("A saved Edit Layer stack loads back equal", "[terrain_editor][editlayersave]")
{
    const std::filesystem::path root = MakeScratchDir("roundtrip");
    const std::filesystem::path layersDir = mir::TerrainEditLayersDir(ScratchRecipe(root));

    TerrainLayerDocument document(layersDir);
    document.Load();
    REQUIRE(document.Editable());
    REQUIRE_FALSE(document.Unsaved());

    document.Append(Layer("Ridge", 2, TerrainEditBlend::Additive));
    document.Append(Layer("Path", 5, TerrainEditBlend::AlphaBlend));
    document.Append(EmptyLayer("Detail", 6));
    CHECK(document.Unsaved());

    REQUIRE(document.Save());
    CHECK_FALSE(document.Unsaved());
    CHECK(document.SaveError().empty());

    const mir::TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(layersDir);
    REQUIRE(loaded);
    CHECK(*loaded.value == document.Layers());

    CHECK_FALSE(std::filesystem::exists(root / "hills.terrain.layers.saving"));
    CHECK_FALSE(std::filesystem::exists(root / "hills.terrain.layers.old"));
    std::filesystem::remove_all(root);
}

TEST_CASE("Saving an empty Edit Layer stack removes its directory", "[terrain_editor][editlayersave]")
{
    const std::filesystem::path root = MakeScratchDir("empty");
    const std::filesystem::path layersDir = mir::TerrainEditLayersDir(ScratchRecipe(root));

    TerrainLayerDocument document(layersDir);
    document.Load();
    document.Append(EmptyLayer("Ridge", 1));
    REQUIRE(document.Save());
    REQUIRE(std::filesystem::exists(layersDir));

    while (!document.Layers().empty())
        document.Remove(0);
    CHECK(document.Unsaved());
    REQUIRE(document.Save());

    CHECK_FALSE(std::filesystem::exists(layersDir));
    CHECK_FALSE(document.Unsaved());

    const mir::TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(layersDir);
    REQUIRE(loaded);
    CHECK(loaded.value->empty());
    std::filesystem::remove_all(root);
}

TEST_CASE("A refused Edit Layer plan leaves the saved directory untouched", "[terrain_editor][editlayersave]")
{
    const std::filesystem::path root = MakeScratchDir("refused");
    const std::filesystem::path layersDir = mir::TerrainEditLayersDir(ScratchRecipe(root));

    TerrainLayerDocument document(layersDir);
    document.Load();
    document.Append(Layer("Ridge", 1, TerrainEditBlend::Additive));
    document.Append(Layer("Path", 2, TerrainEditBlend::Additive));
    REQUIRE(document.Save());

    const Files before = ReadTree(layersDir);
    REQUIRE_FALSE(before.empty());

    TerrainEditLayerInfo duplicate = document.Layers()[1].info;
    duplicate.sequence = document.Layers()[0].info.sequence;
    document.SetInfo(1, duplicate);
    REQUIRE(document.Unsaved());

    CHECK_FALSE(document.Save());
    CHECK_FALSE(document.SaveError().empty());
    CHECK(document.Unsaved());
    CHECK(ReadTree(layersDir) == before);
    std::filesystem::remove_all(root);
}

TEST_CASE("A leftover save directory is replaced, not loaded", "[terrain_editor][editlayersave]")
{
    const std::filesystem::path root = MakeScratchDir("leftover");
    const std::filesystem::path layersDir = mir::TerrainEditLayersDir(ScratchRecipe(root));
    const std::filesystem::path saving = root / "hills.terrain.layers.saving";

    TerrainLayerDocument document(layersDir);
    document.Load();
    document.Append(Layer("Ridge", 1, TerrainEditBlend::Additive));
    REQUIRE(document.Save());

    std::filesystem::create_directories(saving / "L9");
    REQUIRE(mir::WriteText(saving / "layers.json", "junk"));
    REQUIRE(mir::WriteText(saving / "L9" / "tile_9_9.tedit", "junk"));

    const mir::TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(layersDir);
    REQUIRE(loaded);
    CHECK(*loaded.value == document.Layers());

    document.Append(Layer("Path", 2, TerrainEditBlend::AlphaBlend));
    CHECK(document.Unsaved());
    REQUIRE(document.Save());
    CHECK_FALSE(std::filesystem::exists(saving));

    const mir::TerrainResult<std::vector<TerrainEditLayer>> rewritten = LoadTerrainEditStack(layersDir);
    REQUIRE(rewritten);
    CHECK(*rewritten.value == document.Layers());
    std::filesystem::remove_all(root);
}

TEST_CASE("An Edit Layer stack that fails to load is never written", "[terrain_editor][editlayersave]")
{
    const std::filesystem::path root = MakeScratchDir("broken");
    const std::filesystem::path layersDir = mir::TerrainEditLayersDir(ScratchRecipe(root));

    std::filesystem::create_directories(layersDir);
    REQUIRE(mir::WriteText(layersDir / "layers.json", "not a manifest"));

    TerrainLayerDocument document(layersDir);
    document.Load();
    CHECK_FALSE(document.Editable());
    CHECK_FALSE(document.LoadError().empty());
    CHECK_FALSE(document.Unsaved());

    CHECK_FALSE(document.Save());
    CHECK_FALSE(document.SaveError().empty());
    const std::optional<std::string> manifest = mir::ReadText(layersDir / "layers.json");
    REQUIRE(manifest);
    CHECK(*manifest == "not a manifest");
    std::filesystem::remove_all(root);
}

TEST_CASE("Every Edit Layer mutation moves the document's change count", "[terrain_editor][editlayersave]")
{
    const std::filesystem::path root = MakeScratchDir("changes");
    const std::filesystem::path layersDir = mir::TerrainEditLayersDir(ScratchRecipe(root));

    TerrainLayerDocument document(layersDir);
    document.Load();
    const uint64_t start = document.ChangeCount();

    document.Append(Layer("Ridge", 1, TerrainEditBlend::Additive));
    CHECK(document.ChangeCount() == start + 1);

    TerrainEditLayerInfo info = document.Layers().front().info;
    info.name = "Path";
    document.SetInfo(0, info);
    CHECK(document.ChangeCount() == start + 2);

    TerrainEditLayer removed = document.Remove(0);
    CHECK(document.ChangeCount() == start + 3);

    document.Insert(0, std::move(removed));
    CHECK(document.ChangeCount() == start + 4);

    document.MarkChanged();
    CHECK(document.ChangeCount() == start + 5);
    std::filesystem::remove_all(root);
}

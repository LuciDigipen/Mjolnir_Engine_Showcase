/**
 * @file TerrainMaterialLayersDraft.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain_editor/TerrainMaterialLayersDraft.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainEditBase.h>
#include <terrain_editor/TerrainLayerCommands.h>
#include <terrain_editor/TerrainLayerDocument.h>
#include <terrain_editor/TerrainMaterialLayersDraft.h>

#include <editor/history/History.h>
#include <editor/history/SceneOps.h>

#include <core/ecs/Entity.h>
#include <core/ecs/World.h>
#include <core/fs/Paths.h>
#include <terrain/TerrainEditLayer.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <cstdio>
#include <share.h>
#endif

namespace
{
    using mir::TerrainEditBlend;
    using mir::TerrainEditLayer;
    using mir::TerrainEditLayerInfo;
    using mir::TerrainEditTile;
    using mir::TerrainEditTileStates;
    using mir::TerrainLayerBlendMode;
    using mir::TerrainLayerDocument;
    using mir::TerrainMaterialChannelMigration;
    using mir::TerrainMaterialHeightRule;
    using mir::TerrainMaterialLayerRow;
    using mir::TerrainMaterialLayersDraft;
    using mir::TerrainMaterialLayersPlan;
    using mir::TerrainMaterialLayersWriteResult;
    using mir::TileCoord;

    constexpr const char *kTwoLayerRecipe = R"({
  "version": 1,
  "parameters": { "tileSize": 4.0, "keepMe": 7 },
  "other": { "x": 1 },
  "materials": {
    "layers": [
      { "name": "base" },
      { "name": "rock", "rules": { "height": { "min": 0.0, "max": 10.0, "blend": 1.0 } } }
    ]
  }
})";

    constexpr const char *kBoundLayers = R"({
  "version": 1,
  "blend": "alpha",
  "heightContrast": 0.25,
  "layers": [
    { "name": "base", "material": "materials/sand.material.json" },
    { "name": "rock", "material": "materials/rock.material.json" }
  ]
})";

    std::filesystem::path MakeScratchDir(std::string_view tag)
    {
        static std::random_device random;
        const std::filesystem::path dir = std::filesystem::temp_directory_path() /
            "mir_terrain_editor_material_tests" / (std::string(tag) + "_" + std::to_string(random()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }

    void WriteFile(const std::filesystem::path &path, std::string_view text)
    {
        std::error_code ignored;
        std::filesystem::create_directories(path.parent_path(), ignored);
        std::ofstream file(path, std::ios::binary);
        file << text;
    }

    std::string ReadFile(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    struct DraftFixture
    {
        explicit DraftFixture(std::string_view tag) : mRoot(MakeScratchDir(tag)), mAssets(mRoot / "assets")
        {
            WriteFile(mAssets / "materials" / "sand.material.json", R"({"version":1,"frontends":["terrain"]})");
            WriteFile(mAssets / "materials" / "rock.material.json", R"({"version":1,"frontends":["terrain"]})");
        }

        ~DraftFixture()
        {
            std::error_code ignored;
            std::filesystem::remove_all(mRoot, ignored);
        }

        std::filesystem::path RecipePath() const { return mAssets / "terrain" / "hills.terrain.json"; }

        DraftFixture &Recipe(std::string_view text)
        {
            WriteFile(RecipePath(), text);
            return *this;
        }

        DraftFixture &Material(std::string_view relative, std::string_view text)
        {
            WriteFile(mAssets / relative, text);
            return *this;
        }

        DraftFixture &Bound(std::string relative)
        {
            mBound = std::move(relative);
            return *this;
        }

        TerrainMaterialLayersDraft Load() const
        {
            TerrainMaterialLayersDraft draft;
            std::string error;
            REQUIRE(draft.Load({ mAssets, RecipePath(), mBound }, error));
            CHECK(error.empty());
            return draft;
        }

        std::filesystem::path AssetPath(std::string_view relative) const { return mAssets / relative; }

        std::filesystem::path mRoot;
        std::filesystem::path mAssets;
        std::string mBound;
    };

    class NoScene final : public mir::ISceneOps
    {
    public:
        mir::Entity CreateEntity() override { return mir::kNullEntity; }
        mir::Entity Duplicate(mir::Entity) override { return mir::kNullEntity; }
        mir::Entity CopyAsOrdinaryObject(mir::Entity) override { return mir::kNullEntity; }
        mir::CapturedSubtree Capture(mir::Entity) override { return {}; }
        std::vector<mir::Entity> Restore(const mir::CapturedSubtree &, mir::Entity, mir::Entity) override
        {
            return {};
        }
        bool Destroy(mir::Entity) override { return false; }
        mir::Entity PlacePrefab(std::string_view, mir::Entity, const mir::WorldVec3 &) override
        {
            return mir::kNullEntity;
        }
    };

    struct BrushEditing
    {
        BrushEditing() : mDir(MakeScratchDir("brush")), mDocument(mDir / "hills.terrain.layers")
        {
            mDocument.Load();
        }

        ~BrushEditing()
        {
            std::error_code ignored;
            std::filesystem::remove_all(mDir, ignored);
        }

        void Run(std::unique_ptr<mir::ICommand> command)
        {
            mHistory.Submit(std::move(command), true);
            Flush();
        }

        void Undo()
        {
            mHistory.RequestUndo();
            Flush();
        }

        void Redo()
        {
            mHistory.RequestRedo();
            Flush();
        }

        void Flush() { mHistory.Flush(mWorld, mScene); }

        std::filesystem::path mDir;
        TerrainLayerDocument mDocument;
        mir::World mWorld;
        NoScene mScene;
        mir::History mHistory;
    };

    TerrainEditTile ChannelTile(TileCoord coord, std::string layer, std::vector<float> values)
    {
        TerrainEditTile tile;
        tile.tile = coord;
        tile.intervals = 1;
        tile.weights.push_back({ std::move(layer), std::move(values) });
        return tile;
    }
} // namespace

TEST_CASE("A recipe without materials starts one layer bound to the entity's material",
    "[terrain_editor][materiallayers]")
{
    DraftFixture fixture("single");
    fixture.Recipe(R"({"version":1,"parameters":{"tileSize":4.0}})").Bound("materials/sand.material.json");

    const TerrainMaterialLayersDraft draft = fixture.Load();
    CHECK_FALSE(draft.RecipeHasMaterials());
    REQUIRE(draft.Layers().size() == 1);
    CHECK(draft.Layers().front().name == "base");
    CHECK(draft.Layers().front().material == "materials/sand.material.json");
    CHECK(draft.Blend() == TerrainLayerBlendMode::Height);
    CHECK(draft.HeightContrast() == Catch::Approx(0.1f));
    CHECK(draft.Validate().empty());
}

TEST_CASE("A layered entity material gives every recipe layer its surface material",
    "[terrain_editor][materiallayers]")
{
    DraftFixture fixture("layered");
    fixture.Recipe(kTwoLayerRecipe)
        .Material("materials/layers.terrainmaterial.json", kBoundLayers)
        .Bound("materials/layers.terrainmaterial.json");

    const TerrainMaterialLayersDraft draft = fixture.Load();
    CHECK(draft.RecipeHasMaterials());
    REQUIRE(draft.Layers().size() == 2);
    CHECK(draft.Layers()[0].name == "base");
    CHECK(draft.Layers()[0].material == "materials/sand.material.json");
    CHECK(draft.Layers()[1].name == "rock");
    CHECK(draft.Layers()[1].material == "materials/rock.material.json");
    REQUIRE(draft.Layers()[1].rules.height.has_value());
    CHECK(draft.Blend() == TerrainLayerBlendMode::Alpha);
    CHECK(draft.HeightContrast() == Catch::Approx(0.25f));
}

TEST_CASE("A recipe without materials takes its layers from a layered entity material",
    "[terrain_editor][materiallayers]")
{
    DraftFixture fixture("layeredonly");
    fixture.Recipe(R"({"version":1,"parameters":{"tileSize":1.0}})")
        .Material("materials/layers.terrainmaterial.json", kBoundLayers)
        .Bound("materials/layers.terrainmaterial.json");

    const TerrainMaterialLayersDraft draft = fixture.Load();
    CHECK_FALSE(draft.RecipeHasMaterials());
    REQUIRE(draft.Layers().size() == 2);
    CHECK(draft.Layers()[0].material == "materials/sand.material.json");
    CHECK(draft.Layers()[1].name == "rock");
    CHECK(draft.Layers()[1].material == "materials/rock.material.json");
}

TEST_CASE("Validation names the first problem that stops Apply", "[terrain_editor][materiallayers]")
{
    DraftFixture fixture("validation");
    fixture.Bound("materials/sand.material.json");
    fixture.Recipe(R"({"version":1,"materials":{"layers":[
        {"name":"a"},{"name":"b"},{"name":"c"},{"name":"d"},{"name":"e"}]}})");

    TerrainMaterialLayersDraft draft = fixture.Load();
    CHECK(draft.Validate().find("between 1 and 4") != std::string::npos);

    fixture.Recipe(kTwoLayerRecipe);
    draft = fixture.Load();
    draft.Row(0).name = "Bad Name";
    CHECK(draft.Validate().find("not valid") != std::string::npos);

    draft = fixture.Load();
    draft.Row(1).name = "base";
    CHECK(draft.Validate().find("share") != std::string::npos);

    draft = fixture.Load();
    draft.MutableRules(0).height = TerrainMaterialHeightRule{};
    CHECK(draft.Validate().find("first layer") != std::string::npos);

    draft = fixture.Load();
    draft.Row(0).material.clear();
    CHECK(draft.Validate().find("needs a surface material") != std::string::npos);

    draft = fixture.Load();
    draft.Row(0).material = "materials/missing.material.json";
    CHECK(draft.Validate().find("does not exist") != std::string::npos);
}

TEST_CASE("The plan finds renames, deletions and additions by row identity", "[terrain_editor][materiallayers]")
{
    DraftFixture fixture("plan");
    fixture.Recipe(kTwoLayerRecipe).Bound("materials/sand.material.json");

    TerrainMaterialLayersDraft draft = fixture.Load();
    draft.Row(0).name = "sand_base";
    draft.RemoveLayer(1);
    REQUIRE(draft.AddLayer());

    const TerrainMaterialLayersPlan plan = draft.MakePlan();
    REQUIRE(plan.renames.size() == 1);
    CHECK(plan.renames.front() == std::pair<std::string, std::string>("base", "sand_base"));
    CHECK(plan.deletions == std::vector<std::string>{ "rock" });
    CHECK(plan.additions == std::vector<std::string>{ "layer_2" });
    CHECK_FALSE(plan.Empty());
}

TEST_CASE("Writing replaces only the recipe materials and writes the layered material",
    "[terrain_editor][materiallayers]")
{
    DraftFixture fixture("write");
    fixture.Recipe(kTwoLayerRecipe).Bound("materials/sand.material.json");

    TerrainMaterialLayersDraft draft = fixture.Load();
    draft.Row(1).name = "stone";
    draft.Row(1).material = "materials/rock.material.json";

    const TerrainMaterialLayersWriteResult written = mir::WriteTerrainMaterialLayersFiles(draft);
    INFO(written.error);
    REQUIRE(written.error.empty());
    CHECK(written.layeredMaterial == "materials/terrain/hills.terrainmaterial.json");

    const nlohmann::json recipe = nlohmann::json::parse(ReadFile(fixture.RecipePath()));
    CHECK(recipe.at("parameters").at("keepMe") == 7);
    CHECK(recipe.at("other").at("x") == 1);
    REQUIRE(recipe.at("materials").at("layers").size() == 2);
    CHECK(recipe.at("materials").at("layers")[0].at("name") == "base");
    CHECK(recipe.at("materials").at("layers")[1].at("name") == "stone");
    CHECK(recipe.at("materials").at("layers")[1].at("rules").at("height").at("max") == 10.0);

    const nlohmann::json layered = nlohmann::json::parse(ReadFile(fixture.AssetPath(written.layeredMaterial)));
    CHECK(layered.at("version") == 1);
    CHECK(layered.at("sourceRecipe") == "terrain/hills.terrain.json");
    CHECK(layered.at("blend") == "height");
    CHECK(layered.at("heightContrast") == Catch::Approx(0.1f));
    REQUIRE(layered.at("layers").size() == 2);
    CHECK(layered.at("layers")[0].at("material") == "materials/sand.material.json");
    CHECK(layered.at("layers")[1].at("name") == "stone");
    CHECK(layered.at("layers")[1].at("material") == "materials/rock.material.json");
}

TEST_CASE("A single layer writes no layered material file", "[terrain_editor][materiallayers]")
{
    DraftFixture fixture("singlewrite");
    fixture.Recipe(R"({"version":1,"materials":{"layers":[{"name":"base"}]}})")
        .Bound("materials/sand.material.json");

    const TerrainMaterialLayersDraft draft = fixture.Load();
    const TerrainMaterialLayersWriteResult written = mir::WriteTerrainMaterialLayersFiles(draft);
    INFO(written.error);
    REQUIRE(written.error.empty());
    CHECK(written.layeredMaterial.empty());
    CHECK_FALSE(std::filesystem::exists(fixture.AssetPath(draft.LayeredMaterialPath())));

    const nlohmann::json recipe = nlohmann::json::parse(ReadFile(fixture.RecipePath()));
    REQUIRE(recipe.at("materials").at("layers").size() == 1);
    CHECK(recipe.at("materials").at("layers")[0].at("name") == "base");
}

TEST_CASE("Writing keeps the bound layered material's blend and contrast", "[terrain_editor][materiallayers]")
{
    DraftFixture fixture("boundwrite");
    fixture.Recipe(kTwoLayerRecipe)
        .Material("materials/custom.terrainmaterial.json", kBoundLayers)
        .Bound("materials/custom.terrainmaterial.json");

    TerrainMaterialLayersDraft draft = fixture.Load();
    REQUIRE(draft.AddLayer());
    REQUIRE(draft.AddLayer());
    CHECK_FALSE(draft.CanAddLayer());
    CHECK_FALSE(draft.AddLayer());

    const TerrainMaterialLayersWriteResult written = mir::WriteTerrainMaterialLayersFiles(draft);
    INFO(written.error);
    REQUIRE(written.error.empty());
    CHECK(written.layeredMaterial == "materials/custom.terrainmaterial.json");

    const nlohmann::json layered = nlohmann::json::parse(ReadFile(fixture.AssetPath(written.layeredMaterial)));
    CHECK(layered.at("blend") == "alpha");
    CHECK_FALSE(layered.contains("sourceRecipe"));
    CHECK(layered.at("heightContrast") == Catch::Approx(0.25f));
    REQUIRE(layered.at("layers").size() == 4);
    CHECK(layered.at("layers")[1].at("name") == "rock");
    CHECK(layered.at("layers")[2].at("name") == "layer_2");
    CHECK(layered.at("layers")[2].at("material") == "materials/sand.material.json");
    CHECK(layered.at("layers")[3].at("name") == "layer_3");
}

TEST_CASE("Generated material paths distinguish recipe directories and preserve Unicode",
    "[terrain_editor][materiallayers]")
{
    namespace fs = std::filesystem;
    DraftFixture fixture("nested");
    const fs::path first = fixture.mAssets / u8"terrain/north/\U0001F30B.terrain.json";
    const fs::path second = fixture.mAssets / u8"terrain/south/\U0001F30B.terrain.json";
    WriteFile(first, kTwoLayerRecipe);
    WriteFile(second, kTwoLayerRecipe);
    TerrainMaterialLayersDraft draft;
    std::string error;
    REQUIRE(draft.Load({fixture.mAssets, first, "materials/sand.material.json"}, error));
    auto north = mir::WriteTerrainMaterialLayersFiles(draft);
    INFO(north.error);
    REQUIRE(north.error.empty());
    const fs::path northPath = fixture.mAssets / fs::u8path(north.layeredMaterial);
    const auto before = ReadFile(northPath);
    CHECK(nlohmann::json::parse(before).at("sourceRecipe") ==
          mir::GenericPathUtf8(first.lexically_relative(fixture.mAssets)));

    REQUIRE(draft.Load({fixture.mAssets, second, "materials/sand.material.json"}, error));
    draft.Row(1).name = "clay";
    auto south = mir::WriteTerrainMaterialLayersFiles(draft);
    REQUIRE(south.error.empty());
    CHECK(north.layeredMaterial != south.layeredMaterial);
    CHECK(ReadFile(northPath) == before);
}

TEST_CASE("Generated materials can be reused by their recipe and foreign assets are preserved",
    "[terrain_editor][materiallayers]")
{
    DraftFixture fixture("ownership");
    fixture.Recipe(kTwoLayerRecipe).Bound("materials/sand.material.json");
    TerrainMaterialLayersDraft draft = fixture.Load();
    const auto material = draft.LayeredMaterialPath();
    SECTION("the same recipe can re-add its second layer")
    {
        REQUIRE(mir::WriteTerrainMaterialLayersFiles(draft).error.empty());
        draft = fixture.Bound(material).Load();
        draft.RemoveLayer(1);
        REQUIRE(mir::WriteTerrainMaterialLayersFiles(draft).error.empty());
        draft = fixture.Bound("materials/sand.material.json").Load();
        REQUIRE(draft.AddLayer());
        const auto written = mir::WriteTerrainMaterialLayersFiles(draft);
        REQUIRE(written.error.empty());
        CHECK(written.layeredMaterial == material);
    }
    SECTION("a generated path is occupied by a hand-authored asset")
    {
        fixture.Material(material, kBoundLayers);
        draft.Row(1).name = "clay";
        CHECK_FALSE(mir::WriteTerrainMaterialLayersFiles(draft).error.empty());
        CHECK(ReadFile(fixture.RecipePath()) == kTwoLayerRecipe);
        CHECK(ReadFile(fixture.AssetPath(material)) == kBoundLayers);
    }
    SECTION("a bound generated asset records a different owner")
    {
        auto foreign = nlohmann::json::parse(kBoundLayers);
        foreign["sourceRecipe"] = "terrain/another.terrain.json";
        const auto before = foreign.dump();
        fixture.Material(material, before);
        draft = fixture.Bound(material).Load();
        draft.Row(1).name = "clay";
        CHECK_FALSE(mir::WriteTerrainMaterialLayersFiles(draft).error.empty());
        CHECK(ReadFile(fixture.RecipePath()) == kTwoLayerRecipe);
        CHECK(ReadFile(fixture.AssetPath(material)) == before);
    }
}

#ifdef _WIN32
TEST_CASE("A locked layered material rolls the recipe back after a rename",
    "[terrain_editor][materiallayers]")
{
    DraftFixture fixture("rollback");
    const std::string material = "materials/custom.terrainmaterial.json";
    fixture.Recipe(kTwoLayerRecipe).Material(material, kBoundLayers).Bound(material);
    TerrainMaterialLayersDraft draft = fixture.Load();
    draft.Row(1).name = "clay";
    {
        const std::unique_ptr<FILE, decltype(&fclose)> held(
            _wfsopen(fixture.AssetPath(material).c_str(), L"rb", _SH_DENYNO), fclose);
        REQUIRE(held);
        const auto failed = mir::WriteTerrainMaterialLayersFiles(draft);
        REQUIRE_FALSE(failed.error.empty());
        CHECK(failed.error.find("Cannot replace") != std::string::npos);
        CHECK(ReadFile(fixture.RecipePath()) == kTwoLayerRecipe);
        CHECK(ReadFile(fixture.AssetPath(material)) == kBoundLayers);
    }
    CHECK(mir::WriteTerrainMaterialLayersFiles(draft).error.empty());
}
#endif

TEST_CASE("Channel migration renames and deletes painted weights on every layer",
    "[terrain_editor][materiallayers][migration]")
{
    const std::filesystem::path root = MakeScratchDir("migration");
    TerrainLayerDocument document(root / "hills.terrain.layers");
    document.Load();
    REQUIRE(document.Editable());

    TerrainEditLayer stackLayer;
    stackLayer.info = TerrainEditLayerInfo{ "edit", 0, 1, true, TerrainEditBlend::Additive };
    document.Append(stackLayer);
    const uint64_t changes = document.ChangeCount();

    TerrainEditLayer *layer = document.MutableLayer(1);
    REQUIRE(layer != nullptr);

    TerrainEditTile channels;
    channels.tile = TileCoord{ 0, 0 };
    channels.intervals = 1;
    channels.weights.push_back({ "base", { 0.25f, 0.5f, 0.75f, 1.0f } });
    channels.weights.push_back({ "rock", { 1.0f, 1.0f, 1.0f, 1.0f } });
    layer->tiles.emplace(channels.tile, channels);

    TerrainEditTile heights = ChannelTile(TileCoord{ 1, 0 }, "base", { 0.1f, 0.2f, 0.3f, 0.4f });
    heights.values.assign(4, 2.0f);
    layer->tiles.emplace(heights.tile, heights);

    layer->tiles.emplace(TileCoord{ 2, 0 }, ChannelTile(TileCoord{ 2, 0 }, "deleted", { 1.0f, 1.0f, 1.0f, 1.0f }));

    TerrainMaterialLayersPlan plan;
    plan.renames.emplace_back("base", "sand_base");
    plan.deletions.push_back("deleted");
    const TerrainMaterialChannelMigration migrated = mir::MigrateTerrainMaterialChannels(document, plan);
    CHECK(migrated.renamed == 2);
    CHECK(migrated.deleted == 1);
    CHECK(migrated.droppedTiles == 1);

    const TerrainEditLayer &edited = document.Layers().front();
    REQUIRE(edited.tiles.size() == 2);
    const TerrainEditTile &migratedChannels = edited.tiles.at(TileCoord{ 0, 0 });
    REQUIRE(migratedChannels.weights.size() == 2);
    CHECK(migratedChannels.weights[0].layer == "rock");
    CHECK(migratedChannels.weights[1].layer == "sand_base");
    CHECK(migratedChannels.weights[1].values == std::vector<float>{ 0.25f, 0.5f, 0.75f, 1.0f });

    const TerrainEditTile &migratedHeights = edited.tiles.at(TileCoord{ 1, 0 });
    CHECK(migratedHeights.values == std::vector<float>(4, 2.0f));
    REQUIRE(migratedHeights.weights.size() == 1);
    CHECK(migratedHeights.weights.front().layer == "sand_base");
    CHECK(document.ChangeCount() > changes);
    CHECK(document.MaterialGeneration() == 1);
}

TEST_CASE("Paint points a recipe without material layers at the Material layers section",
    "[terrain_editor][materiallayers]")
{
    const mir::TerrainBrushAvailability missing = mir::EvaluateTerrainPaintAvailability(
        true, true, true, true, true, false, "no cache", true, false);
    CHECK_FALSE(missing.enabled);
    CHECK(missing.message.find("Material layers") != std::string::npos);
    CHECK(missing.message.find("Build the game") == std::string::npos);

    const mir::TerrainBrushAvailability unbaked = mir::EvaluateTerrainPaintAvailability(
        true, true, true, true, true, false, "no cache", true, true);
    CHECK(unbaked.message.find("Build the game") != std::string::npos);
}

TEST_CASE("A brush step from before a material migration refuses to undo", "[terrain_editor][materiallayers]")
{
    BrushEditing editing;
    TerrainLayerDocument &document = editing.mDocument;
    REQUIRE(document.Editable());

    TerrainEditLayer layer;
    layer.info = TerrainEditLayerInfo{ "edit", 0, 1, true, TerrainEditBlend::Additive };
    document.Append(layer);

    const TerrainEditTile painted = ChannelTile(TileCoord{ 0, 0 }, "base", { 1.0f, 0.5f });
    TerrainEditTileStates before;
    before[painted.tile] = std::nullopt;
    TerrainEditTileStates after;
    after[painted.tile] = painted;

    editing.Run(std::make_unique<mir::TerrainBrushCommand>(document, 1, before, after));
    REQUIRE(document.Layers().front().tiles.size() == 1);

    editing.Undo();
    CHECK(document.Layers().front().tiles.empty());
    editing.Redo();
    REQUIRE(document.Layers().front().tiles.size() == 1);

    document.BumpMaterialGeneration();
    editing.Undo();
    CHECK(document.Layers().front().tiles.size() == 1);
    CHECK(editing.mHistory.CanUndo());
}

/**
 * @file TerrainBrushCommands.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain_editor/TerrainLayerCommands' brush stroke command.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain_editor/TerrainEditOverlay.h>
#include <terrain_editor/TerrainHeightBrush.h>
#include <terrain_editor/TerrainLayerCommands.h>
#include <terrain_editor/TerrainLayerDocument.h>

#include <editor/history/History.h>
#include <editor/history/SceneOps.h>

#include <core/ecs/Entity.h>
#include <core/ecs/World.h>

#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TileHeights.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using mir::AddTerrainLayerCommand;
    using mir::CapturedSubtree;
    using mir::Entity;
    using mir::History;
    using mir::kNullEntity;
    using mir::kSceneDocument;
    using mir::TerrainBaseHeightsErrorCode;
    using mir::TerrainBaseHeightsResult;
    using mir::TerrainBaseTile;
    using mir::TerrainBaseTileProvider;
    using mir::TerrainBrushCommand;
    using mir::TerrainBrushSettings;
    using mir::TerrainBrushStroke;
    using mir::TerrainBrushTileSource;
    using mir::TerrainEditBlend;
    using mir::TerrainEditLayer;
    using mir::TerrainEditOverlaySession;
    using mir::TerrainEditTileStates;
    using mir::TerrainLayerDocument;
    using mir::TerrainLayout;
    using mir::TerrainLocalPosition;
    using mir::TerrainTileOrder;
    using mir::TileCoord;
    using mir::TileHeights;
    using mir::World;
    using mir::WorldVec3;

    class NoScene final : public mir::ISceneOps
    {
    public:
        Entity CreateEntity() override { return kNullEntity; }
        Entity Duplicate(Entity) override { return kNullEntity; }
        Entity CopyAsOrdinaryObject(Entity) override { return kNullEntity; }
        CapturedSubtree Capture(Entity) override { return {}; }
        std::vector<Entity> Restore(const CapturedSubtree &, Entity, Entity) override { return {}; }
        bool Destroy(Entity) override { return false; }
        Entity PlacePrefab(std::string_view, Entity, const WorldVec3 &) override { return kNullEntity; }
    };

    std::filesystem::path MakeScratchDir()
    {
        static std::random_device random;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "mir_terrain_editor_tests" / ("brush_" + std::to_string(random()));
        std::filesystem::remove_all(dir);
        return dir;
    }

    TerrainLayout MakeLayout()
    {
        const std::optional<TerrainLayout> layout =
            TerrainLayout::Create(1.0, 8, mir::TerrainLocalBounds{ 0.0, 0.0, 8.0, 8.0 });
        REQUIRE(layout);
        return *layout;
    }

    TerrainBaseTile MakeBaseTile(TileCoord coord, int32_t intervals)
    {
        std::optional<TileHeights> tile = TileHeights::Create(coord, intervals);
        REQUIRE(tile);
        for (int32_t k = 0; k <= intervals; ++k)
        {
            for (int32_t i = 0; i <= intervals; ++i)
            {
                const float x = static_cast<float>(coord.x * intervals + i);
                const float z = static_cast<float>(coord.z * intervals + k);
                tile->Set(i, k, 1.0f + 0.25f * x - 0.5f * z);
            }
        }
        return std::make_shared<const TileHeights>(std::move(*tile));
    }

    struct Editing
    {
        Editing() : mDir(MakeScratchDir()), mDocument(mDir / "hills.terrain.layers")
        {
            mDocument.Load();
        }

        ~Editing()
        {
            std::error_code ignored;
            std::filesystem::remove_all(mDir, ignored);
        }

        TerrainLayerDocument &Document() { return mDocument; }
        History &Steps() { return mHistory; }

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
        World mWorld;
        NoScene mScene;
        History mHistory;
    };
}

TEST_CASE("One brush stroke is one History step and undo removes the tiles it created",
    "[terrain_editor][brush]")
{
    Editing editing;
    TerrainLayerDocument &document = editing.Document();
    REQUIRE(document.Editable());
    editing.Run(std::make_unique<AddTerrainLayerCommand>(document, TerrainEditBlend::Additive));
    REQUIRE(document.Layers().size() == 1);
    REQUIRE(document.Save());
    CHECK_FALSE(document.Unsaved());

    const uint32_t sequence = document.Layers().front().info.sequence;
    TerrainEditLayer *layer = document.MutableLayer(sequence);
    REQUIRE(layer != nullptr);
    const std::vector<TerrainEditLayer> beforeStack = document.Layers();
    const std::size_t steps = editing.Steps().StepCount();
    const uint64_t changesBeforeDabs = document.ChangeCount();

    const TerrainLayout layout = MakeLayout();
    TerrainBrushTileSource source{ layout, [layout](TileCoord) { return std::optional<TerrainLayout>(layout); },
        [](TileCoord coord) -> TerrainBaseHeightsResult<TerrainBaseTile> {
            return { MakeBaseTile(coord, 8), {} };
        } };

    TerrainBrushStroke stroke(*layer, document.Layers(), source, 7.0,
        [&document]() { document.MarkChanged(); });
    TerrainBrushSettings settings;
    settings.brush = mir::TerrainBrush::RaiseLower;
    settings.radius = 2.5;
    settings.strength = 0.5;
    settings.falloff = 0.5;
    const std::optional<TerrainLocalPosition> center =
        TerrainLocalPosition::FromMeters(glm::dvec3(4.0, 0.0, 4.0));
    REQUIRE(center);
    for (int dab = 0; dab < 3; ++dab)
        REQUIRE(stroke.Dab(*center, settings, 0.1).Ok());

    CHECK_FALSE(stroke.Empty());
    CHECK_FALSE(layer->tiles.empty());
    const uint64_t changesAfterDabs = document.ChangeCount();
    CHECK(changesAfterDabs > changesBeforeDabs);
    const std::vector<TerrainEditLayer> afterStack = document.Layers();

    TerrainEditOverlaySession overlays;
    const TerrainEditTileStates changed = stroke.Before();
    for (const auto &[tile, before] : changed)
    {
        (void)before;
        overlays.MarkEdited(tile);
    }
    const TerrainBaseTileProvider provider = [](TileCoord coord) -> TerrainBaseHeightsResult<TerrainBaseTile> {
        return { MakeBaseTile(coord, 8), {} };
    };
    std::string reason;
    const std::shared_ptr<const mir::TerrainOverlay> snapshot =
        overlays.Build(mir::AssetId{ 0x99 }, document.Layers(), provider, reason);
    REQUIRE(snapshot);
    CHECK(snapshot->Tiles().size() == overlays.EditedTiles().size());
    const uint64_t strokeRevision = snapshot->Revision();

    editing.Run(std::make_unique<TerrainBrushCommand>(document, sequence, stroke.Before(), stroke.After()));
    CHECK(editing.Steps().StepCount() == steps + 1);
    const uint64_t changesAfterRun = document.ChangeCount();
    CHECK(changesAfterRun > changesAfterDabs);
    CHECK(document.Layers() == afterStack);
    CHECK(document.Unsaved());
    CHECK_FALSE(editing.Steps().IsUnsaved(kSceneDocument));

    editing.Undo();
    const uint64_t changesAfterUndo = document.ChangeCount();
    CHECK(changesAfterUndo > changesAfterRun);
    CHECK(document.Layers() == beforeStack);
    CHECK(document.MutableLayer(sequence)->tiles.empty());
    CHECK_FALSE(document.Unsaved());
    CHECK_FALSE(editing.Steps().IsUnsaved(kSceneDocument));

    const std::shared_ptr<const mir::TerrainOverlay> undone =
        overlays.Build(mir::AssetId{ 0x99 }, document.Layers(), provider, reason);
    REQUIRE(undone);
    CHECK(undone->Revision() > strokeRevision);

    editing.Redo();
    CHECK(document.ChangeCount() > changesAfterUndo);
    CHECK(document.Layers() == afterStack);
    CHECK(document.Unsaved());
    CHECK_FALSE(editing.Steps().IsUnsaved(kSceneDocument));
}

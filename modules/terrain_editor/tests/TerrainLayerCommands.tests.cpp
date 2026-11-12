/**
 * @file TerrainLayerCommands.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain_editor/TerrainLayerCommands.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainLayerCommands.h>
#include <terrain_editor/TerrainLayerDocument.h>

#include <editor/history/History.h>
#include <editor/history/SceneOps.h>

#include <core/ecs/Entity.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using mir::AddTerrainLayerCommand;
    using mir::CapturedSubtree;
    using mir::CommandContext;
    using mir::DeleteTerrainLayerCommand;
    using mir::Entity;
    using mir::History;
    using mir::ICommand;
    using mir::kNullEntity;
    using mir::kSceneDocument;
    using mir::MoveTerrainLayerCommand;
    using mir::RenameTerrainLayerCommand;
    using mir::SetTerrainLayerEnabledCommand;
    using mir::SetTerrainLayerPriorityCommand;
    using mir::TerrainEditBlend;
    using mir::TerrainEditLayer;
    using mir::TerrainLayerDocument;
    using mir::TerrainLayerMove;
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

    std::filesystem::path MakeScratchDir(std::string_view tag)
    {
        static std::random_device random;
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "mir_terrain_editor_tests" /
            (std::string(tag) + "_" + std::to_string(random()));
        std::filesystem::remove_all(dir);
        return dir;
    }

    struct Editing
    {
        explicit Editing(std::string_view tag)
            : mDir(MakeScratchDir(tag)), mDocument(mDir / "hills.terrain.layers")
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

        void Run(std::unique_ptr<ICommand> command)
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

        uint32_t SequenceAt(std::size_t index) const { return mDocument.Layers()[index].info.sequence; }

        World mWorld;
        NoScene mScene;
        History mHistory;
        std::filesystem::path mDir;
        TerrainLayerDocument mDocument;
    };

    int32_t PriorityOf(const TerrainLayerDocument &document, uint32_t sequence)
    {
        return document.Find(sequence)->info.priority;
    }
} // namespace

TEST_CASE("Edit Layer commands undo and redo to the same stacks", "[terrain_editor][editlayers]")
{
    Editing editing("roundtrip");
    TerrainLayerDocument &document = editing.Document();
    REQUIRE(document.Editable());
    CHECK_FALSE(document.Unsaved());

    std::vector<std::vector<TerrainEditLayer>> states{ document.Layers() };
    const auto run = [&editing, &document, &states](std::unique_ptr<ICommand> command) {
        editing.Run(std::move(command));
        states.push_back(document.Layers());
    };

    run(std::make_unique<AddTerrainLayerCommand>(document, TerrainEditBlend::Additive));
    REQUIRE(document.Layers().size() == 1);
    const uint32_t first = editing.SequenceAt(0);
    CHECK(document.Layers()[0].info.name == "Layer 1");
    CHECK(document.Layers()[0].info.heightBlend == TerrainEditBlend::Additive);

    run(std::make_unique<AddTerrainLayerCommand>(document, TerrainEditBlend::AlphaBlend));
    REQUIRE(document.Layers().size() == 2);
    const uint32_t second = editing.SequenceAt(1);
    CHECK(document.Layers()[1].info.name == "Layer 2");
    CHECK(document.Layers()[1].info.heightBlend == TerrainEditBlend::AlphaBlend);
    CHECK(document.Layers()[1].info.priority > document.Layers()[0].info.priority);

    run(std::make_unique<RenameTerrainLayerCommand>(document, first, "Ridge"));
    CHECK(document.Find(first)->info.name == "Ridge");

    run(std::make_unique<SetTerrainLayerPriorityCommand>(document, second, 9));
    CHECK(PriorityOf(document, second) == 9);

    run(std::make_unique<SetTerrainLayerEnabledCommand>(document, first, false));
    CHECK_FALSE(document.Find(first)->info.enabled);

    run(std::make_unique<MoveTerrainLayerCommand>(document, first, TerrainLayerMove::Up));
    CHECK(document.CompositingOrder().back() == document.IndexOf(first));

    run(std::make_unique<DeleteTerrainLayerCommand>(document, first));
    CHECK(document.Find(first) == nullptr);
    REQUIRE(document.Layers().size() == 1);

    REQUIRE(states.size() == 8);
    for (std::size_t i = states.size() - 1; i > 0; --i)
    {
        editing.Undo();
        CHECK(document.Layers() == states[i - 1]);
    }
    for (std::size_t i = 1; i < states.size(); ++i)
    {
        editing.Redo();
        CHECK(document.Layers() == states[i]);
    }
}

TEST_CASE("Edit Layer names stay unique and a refused command records no step", "[terrain_editor][editlayers]")
{
    Editing editing("refusals");
    TerrainLayerDocument &document = editing.Document();
    editing.Run(std::make_unique<AddTerrainLayerCommand>(document, TerrainEditBlend::Additive));
    editing.Run(std::make_unique<AddTerrainLayerCommand>(document, TerrainEditBlend::Additive));

    const uint32_t first = editing.SequenceAt(0);
    const uint32_t second = editing.SequenceAt(1);
    const std::size_t steps = editing.Steps().StepCount();

    CHECK(document.CanRename(second, "Path"));
    CHECK_FALSE(document.CanRename(second, ""));
    CHECK_FALSE(document.CanRename(second, "Layer 1"));
    CHECK_FALSE(document.CanRename(99, "Path"));

    editing.Run(std::make_unique<RenameTerrainLayerCommand>(document, second, ""));
    CHECK(editing.Steps().StepCount() == steps);
    editing.Run(std::make_unique<RenameTerrainLayerCommand>(document, second, "Layer 1"));
    CHECK(editing.Steps().StepCount() == steps);
    CHECK(document.Find(second)->info.name == "Layer 2");

    editing.Run(std::make_unique<MoveTerrainLayerCommand>(document, first, TerrainLayerMove::Down));
    CHECK(editing.Steps().StepCount() == steps);
    editing.Run(std::make_unique<MoveTerrainLayerCommand>(document, second, TerrainLayerMove::Up));
    CHECK(editing.Steps().StepCount() == steps);
    editing.Run(std::make_unique<DeleteTerrainLayerCommand>(document, 99));
    CHECK(editing.Steps().StepCount() == steps);
    editing.Run(std::make_unique<SetTerrainLayerPriorityCommand>(document, 99, 3));
    CHECK(editing.Steps().StepCount() == steps);
}

TEST_CASE("An added Edit Layer never takes back a used sequence", "[terrain_editor][editlayers]")
{
    Editing editing("sequences");
    TerrainLayerDocument &document = editing.Document();

    editing.Run(std::make_unique<AddTerrainLayerCommand>(document, TerrainEditBlend::Additive));
    const uint32_t first = editing.SequenceAt(0);
    editing.Run(std::make_unique<DeleteTerrainLayerCommand>(document, first));
    editing.Run(std::make_unique<AddTerrainLayerCommand>(document, TerrainEditBlend::Additive));

    REQUIRE(document.Layers().size() == 1);
    CHECK(editing.SequenceAt(0) == first + 1);
    CHECK(document.Layers()[0].info.name == "Layer 1");

    const uint32_t top = editing.SequenceAt(0);
    editing.Undo();
    editing.Run(std::make_unique<AddTerrainLayerCommand>(document, TerrainEditBlend::Additive));
    CHECK(editing.SequenceAt(0) == top + 1);
}

TEST_CASE("The unsaved flag follows saves, edits and undo back to the saved point",
    "[terrain_editor][editlayers]")
{
    Editing editing("unsaved");
    TerrainLayerDocument &document = editing.Document();
    REQUIRE(document.Editable());
    CHECK_FALSE(document.Unsaved());

    editing.Run(std::make_unique<AddTerrainLayerCommand>(document, TerrainEditBlend::Additive));
    CHECK(document.Unsaved());

    REQUIRE(document.Save());
    CHECK_FALSE(document.Unsaved());

    editing.Run(std::make_unique<AddTerrainLayerCommand>(document, TerrainEditBlend::Additive));
    CHECK(document.Unsaved());

    editing.Undo();
    CHECK_FALSE(document.Unsaved());

    editing.Undo();
    CHECK(document.Unsaved());
}

TEST_CASE("Edit Layer commands never mark the scene document", "[terrain_editor][editlayers]")
{
    Editing editing("scene-document");
    TerrainLayerDocument &document = editing.Document();

    const uint32_t layer = [&editing, &document]() {
        editing.Run(std::make_unique<AddTerrainLayerCommand>(document, TerrainEditBlend::Additive));
        return editing.SequenceAt(0);
    }();
    editing.Run(std::make_unique<RenameTerrainLayerCommand>(document, layer, "Ridge"));
    editing.Run(std::make_unique<SetTerrainLayerPriorityCommand>(document, layer, 4));
    editing.Run(std::make_unique<SetTerrainLayerEnabledCommand>(document, layer, false));
    editing.Undo();
    editing.Redo();

    CHECK(editing.Steps().StepCount() == 4);
    CHECK_FALSE(editing.Steps().IsUnsaved(kSceneDocument));
    CHECK_FALSE(editing.Steps().IsUnsaved());
}

TEST_CASE("Edit Layer priorities swap with the compositing neighbour, ties step past it",
    "[terrain_editor][editlayers]")
{
    Editing editing("priorities");
    TerrainLayerDocument &document = editing.Document();
    editing.Run(std::make_unique<AddTerrainLayerCommand>(document, TerrainEditBlend::Additive));
    editing.Run(std::make_unique<AddTerrainLayerCommand>(document, TerrainEditBlend::Additive));

    const uint32_t first = editing.SequenceAt(0);
    const uint32_t second = editing.SequenceAt(1);
    REQUIRE(PriorityOf(document, first) < PriorityOf(document, second));

    editing.Run(std::make_unique<MoveTerrainLayerCommand>(document, first, TerrainLayerMove::Up));
    CHECK(PriorityOf(document, first) == PriorityOf(document, second) + 1);
    CHECK(document.CompositingOrder().back() == document.IndexOf(first));
    editing.Undo();
    CHECK(PriorityOf(document, first) < PriorityOf(document, second));
    editing.Redo();
    CHECK(document.CompositingOrder().back() == document.IndexOf(first));

    editing.Run(std::make_unique<SetTerrainLayerPriorityCommand>(document, first, 4));
    editing.Run(std::make_unique<SetTerrainLayerPriorityCommand>(document, second, 4));
    REQUIRE(PriorityOf(document, first) == PriorityOf(document, second));
    editing.Run(std::make_unique<MoveTerrainLayerCommand>(document, first, TerrainLayerMove::Up));
    CHECK(PriorityOf(document, first) > PriorityOf(document, second));
    CHECK(document.CompositingOrder().back() == document.IndexOf(first));
    editing.Undo();
    CHECK(PriorityOf(document, first) == 4);
    CHECK(PriorityOf(document, second) == 4);

    editing.Run(std::make_unique<MoveTerrainLayerCommand>(document, second, TerrainLayerMove::Down));
    CHECK(PriorityOf(document, second) < PriorityOf(document, first));
    CHECK(document.CompositingOrder().front() == document.IndexOf(second));
    editing.Undo();
    CHECK(PriorityOf(document, second) == 4);
    CHECK(PriorityOf(document, first) == 4);
}

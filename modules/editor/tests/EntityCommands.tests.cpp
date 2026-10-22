/**
 * @file EntityCommands.tests.cpp
 * @author Dong Ta
 * @brief Create, delete, duplicate and reparent: do, undo and redo, compared through saved scene files.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "EditingFixture.h"

#include <editor/history/EntityCommands.h>
#include <editor/panels/HierarchyPanel.h>

#include <core/ecs/EntityPresetRegistry.h>

namespace
{
    using mir::EntityAction;
    using mir::EntityActionKind;
    using mir::EntityPresetDesc;
    using mir::MakeCreateCommand;
    using mir::MakeDeleteCommand;
    using mir::MakeDuplicateCommand;
    using mir::MakeReParentCommand;

    void Thing(World &world, Entity entity)
    {
        world.AddComponent<mir::Name>(entity, mir::MakeName("Thing"));
    }

    bool Never(const World &)
    {
        return false;
    }
}

TEST_CASE("Delete, undo and redo round-trip the scene files", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);

    Entity target = kNullEntity;
    SECTION("a middle child") { target = s.y; }
    SECTION("a last child") { target = s.z; }
    SECTION("a root") { target = s.b; }
    SECTION("a parent") { target = s.a; }

    const Files before = e.Saved();
    e.Run(MakeDeleteCommand(target));
    const Files deleted = e.Saved();
    CHECK(deleted != before);

    e.Undo();
    CHECK(Differences(e.Saved(), before) == std::vector<std::string>{});

    e.Redo();
    CHECK(Differences(e.Saved(), deleted) == std::vector<std::string>{});

    e.Undo();
    CHECK(Differences(e.Saved(), before) == std::vector<std::string>{});
}

TEST_CASE("Undoing a delete points references from outside back at it", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);
    const StableId y = e.IdOf(s.y);

    e.Run(MakeDeleteCommand(s.y));
    e.Undo();

    CHECK(e.Find(y) != s.y);
    CHECK(TargetOf(e.mWorld, s.b) == e.Find(y));
}

TEST_CASE("Two deletes linked by a reference undo back to the same scene", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);
    const Files before = e.Saved();

    SECTION("the target first")
    {
        e.Run(MakeDeleteCommand(s.x));
        e.Run(MakeDeleteCommand(s.y));
    }
    SECTION("the referrer first")
    {
        e.Run(MakeDeleteCommand(s.y));
        e.Run(MakeDeleteCommand(s.x));
    }
    const Files deleted = e.Saved();

    e.Undo();
    e.Undo();
    CHECK(Differences(e.Saved(), before) == std::vector<std::string>{});

    e.Redo();
    e.Redo();
    CHECK(Differences(e.Saved(), deleted) == std::vector<std::string>{});

    e.Undo();
    e.Undo();
    CHECK(Differences(e.Saved(), before) == std::vector<std::string>{});
}

TEST_CASE("Create, undo and redo round-trip the scene files", "[editor][history]")
{
    Editing e;
    BuildSample(e);

    static const EntityPresetDesc thing{ "Test/Thing", &Thing };
    const EntityPresetDesc *preset = nullptr;
    SECTION("a plain entity") { preset = nullptr; }
    SECTION("from a preset") { preset = &thing; }

    const Files before = e.Saved();
    e.Run(MakeCreateCommand(preset));
    const Files created = e.Saved();
    CHECK(created != before);
    REQUIRE(e.Selected().size() == 1);

    e.Undo();
    CHECK(Differences(WithoutNextId(e.Saved()), WithoutNextId(before)) == std::vector<std::string>{});

    e.Redo();
    CHECK(Differences(e.Saved(), created) == std::vector<std::string>{});
    CHECK(e.mHistory.UndoName() == (preset != nullptr ? "Create Thing" : "Create Entity"));
}

TEST_CASE("Duplicate, undo and redo round-trip the scene files", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);

    const Files before = e.Saved();
    e.Run(MakeDuplicateCommand(s.y));
    const Files duplicated = e.Saved();
    CHECK(duplicated != before);
    REQUIRE(e.Selected().size() == 1);
    CHECK(e.Selected().front() != s.y);

    e.Undo();
    CHECK(Differences(WithoutNextId(e.Saved()), WithoutNextId(before)) == std::vector<std::string>{});
    CHECK(e.Selected().empty());

    e.Redo();
    CHECK(Differences(e.Saved(), duplicated) == std::vector<std::string>{});
}

TEST_CASE("A reparent is one step and undoes in place", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);

    std::vector<Entity> targets;
    Entity parent = kNullEntity;
    Entity before = kNullEntity;
    SECTION("two entities under another parent")
    {
        targets = { s.x, s.z };
        parent = s.b;
    }
    SECTION("a reorder under the same parent")
    {
        targets = { s.z };
        parent = s.a;
        before = s.x;
    }
    SECTION("to the end of the same parent")
    {
        targets = { s.x };
        parent = s.a;
    }
    SECTION("a subtree to the roots")
    {
        targets = { s.y, s.w };
    }
    SECTION("two entities before a root")
    {
        targets = { s.x, s.z };
        before = s.b;
    }

    const Files original = e.Saved();
    e.Run(MakeReParentCommand(targets, parent, before));
    const Files moved = e.Saved();

    CHECK(e.mHistory.StepCount() == 1);
    CHECK(moved != original);

    e.Undo();
    CHECK(Differences(e.Saved(), original) == std::vector<std::string>{});

    e.Redo();
    CHECK(Differences(e.Saved(), moved) == std::vector<std::string>{});

    e.Undo();
    CHECK(Differences(e.Saved(), original) == std::vector<std::string>{});
}

TEST_CASE("A drop that moves nothing records no step", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);

    std::vector<Entity> targets;
    Entity parent = kNullEntity;
    Entity before = kNullEntity;
    SECTION("before its own next sibling")
    {
        targets = { s.x };
        parent = s.a;
        before = s.y;
    }
    SECTION("to the end it is already at")
    {
        targets = { s.z };
        parent = s.a;
    }
    SECTION("two neighbors before the one after them")
    {
        targets = { s.x, s.y };
        parent = s.a;
        before = s.z;
    }
    SECTION("a root before its own next root")
    {
        targets = { s.b };
        before = s.a;
    }
    SECTION("the last root to the end of the roots")
    {
        targets = { s.a };
    }

    const Files original = e.Saved();
    e.Run(MakeReParentCommand(targets, parent, before));

    CHECK(e.mHistory.StepCount() == 0);
    CHECK(Differences(e.Saved(), original) == std::vector<std::string>{});
}

TEST_CASE("Delete selects the neighbor, and undo selects the entity again", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);
    const StableId x = e.IdOf(s.x);
    e.Selected() = { s.x };

    e.Run(MakeDeleteCommand(s.x));
    CHECK(e.Selected() == std::vector<Entity>{ s.y });

    e.Undo();
    CHECK(e.Selected() == std::vector<Entity>{ e.Find(x) });

    e.Redo();
    CHECK(e.Selected() == std::vector<Entity>{ s.y });
}

TEST_CASE("A command that cannot run records no step", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);
    const Entity dead = e.Add("dead");
    e.mWorld.DestroyEntity(dead);

    static const EntityPresetDesc never{ "Test/Never", &Thing, &Never };

    SECTION("delete") { e.Run(MakeDeleteCommand(dead)); }
    SECTION("duplicate") { e.Run(MakeDuplicateCommand(dead)); }
    SECTION("a preset that refuses") { e.Run(MakeCreateCommand(&never)); }
    SECTION("a parent under its own child") { e.Run(MakeReParentCommand({ s.a }, s.x, kNullEntity)); }

    CHECK(e.mHistory.StepCount() == 0);
}

TEST_CASE("MakeEntityCommand turns each EntityAction into a command", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);

    CHECK(mir::MakeEntityCommand(EntityAction{}) == nullptr);

    e.Run(mir::MakeEntityCommand(EntityAction{ EntityActionKind::Delete, nullptr, s.z }));
    CHECK(e.mHistory.UndoName() == "Delete z");

    e.Run(mir::MakeEntityCommand(EntityAction{ EntityActionKind::Reparent, nullptr, kNullEntity, { s.x, s.y }, s.b }));
    CHECK(e.mHistory.UndoName() == "Move 2 Entities");

    e.Run(mir::MakeEntityCommand(EntityAction{ EntityActionKind::Duplicate, nullptr, s.b }));
    CHECK(e.mHistory.UndoName() == "Duplicate b");
}

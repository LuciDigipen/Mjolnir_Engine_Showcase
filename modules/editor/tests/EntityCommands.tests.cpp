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
#include <scene/SceneInstancePatch.h>
#include <particles/ComponentRegistration.h>
#include <particles/components/ParticleSystem.h>

namespace
{
    using mir::EntityAction;
    using mir::EntityActionKind;
    using mir::EntityPresetDesc;
    using mir::FindInstanceFieldPatch;
    using mir::MakeCopyAsOrdinaryObjectCommand;
    using mir::MakeCreateCommand;
    using mir::MakeDeleteCommand;
    using mir::MakeDuplicateCommand;
    using mir::MakePlacePrefabCommand;
    using mir::MakeReParentCommand;
    using mir::SceneNodeAddress;
    using mir::ScenePrefabInstance;
    using mir::SceneInstancePatch;
    using mir::SceneFieldPatch;

    void Thing(World &world, Entity entity)
    {
        world.AddComponent<mir::Name>(entity, mir::MakeName("Thing"));
    }

    bool Never(const World &)
    {
        return false;
    }

    // a and its children as the members of one instance; w stays an ordinary child under y
    StableId MakeInstance(Editing &e, const Sample &s)
    {
        const StableId root = e.IdOf(s.a);
        const StableId y = e.IdOf(s.y);

        SetTarget(e.mWorld, s.a, s.y);

        ScenePrefabInstance instance;
        instance.mPrefab = "11111111-2222-4333-8444-555555555555";
        instance.mRoot = root;
        instance.mBindings[SceneNodeAddress{ 1 }] = root;
        instance.mBindings[SceneNodeAddress{ 2 }] = e.IdOf(s.x);
        instance.mBindings[SceneNodeAddress{ 3 }] = y;
        instance.mBindings[SceneNodeAddress{ 4 }] = e.IdOf(s.z);

        SceneInstancePatch patch;
        patch.mTarget = SceneNodeAddress{ 1 };
        patch.mFields.push_back(SceneFieldPatch{ "EditorHistoryTestRef", "target", nlohmann::json(y) });
        instance.mPatches.push_back(std::move(patch));

        e.mScene.mInstances[root] = std::move(instance);
        return root;
    }

    bool BindsMember(const LoadedScene &scene, StableId member)
    {
        for (const auto &[root, instance] : scene.mInstances)
        {
            (void)root;
            for (const auto &[address, id] : instance.mBindings)
            {
                (void)address;
                if (id == member)
                    return true;
            }
        }
        return false;
    }

    std::vector<Entity> ChildrenOf(World &world, Entity parent)
    {
        std::vector<Entity> children;
        mir::ForEachChild(world, parent, [&](Entity child) { children.push_back(child); });
        return children;
    }
}

TEST_CASE("Placing a particle system creates an undoable scene entity", "[editor][particles]")
{
    Editing e;
    mir::RegisterParticleComponents();
    e.Run(mir::MakePlaceParticleSystemCommand("assets/effects/exhaust.particle.json",
        kNullEntity, mir::WorldVec3(2, 3, 4)));
    REQUIRE(e.mScene.mEntities.size() == 1);
    const Entity placed = e.mScene.mEntities.begin()->second;
    const auto *component = e.mWorld.GetComponent<mir::ParticleSystem>(placed);
    REQUIRE(component != nullptr);
    CHECK(std::string(component->systemName) == "effects/exhaust.particle.json");
    REQUIRE(e.mWorld.GetComponent<Transform>(placed) != nullptr);
    CHECK(e.mWorld.GetComponent<Transform>(placed)->Position() == mir::WorldVec3(2, 3, 4));
    e.Undo();
    CHECK(e.mScene.mEntities.empty());
    e.Redo();
    REQUIRE(e.mScene.mEntities.size() == 1);
    CHECK(e.mWorld.GetComponent<mir::ParticleSystem>(e.mScene.mEntities.begin()->second) != nullptr);
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

TEST_CASE("Placing a Prefab is one step that keeps its identity through undo and redo", "[editor][history]")
{
    Editing e;
    const mir::WorldVec3 position(2.0, 0.0, 3.0);

    e.Run(MakePlacePrefabCommand("assets/prefabs/Chair.prefab", kNullEntity, position));
    REQUIRE(e.mHistory.StepCount() == 1);
    REQUIRE(e.Selected().size() == 1);

    const Entity root = e.Selected().front();
    const StableId rootId = e.IdOf(root);
    REQUIRE(rootId != kNullStableId);
    REQUIRE(e.mScene.mInstances.contains(rootId));
    const ScenePrefabInstance record = e.mScene.mInstances.at(rootId);
    CHECK(record.mPrefab == "assets/prefabs/Chair.prefab");
    CHECK(e.mWorld.GetComponent<Transform>(root)->Position() == position);
    const Files placed = e.Saved();

    e.Undo();
    CHECK_FALSE(e.mWorld.IsAlive(root));
    CHECK_FALSE(e.mScene.mInstances.contains(rootId));

    e.Redo();
    REQUIRE(e.mScene.mEntities.contains(rootId));
    CHECK(e.mWorld.IsAlive(e.mScene.mEntities.at(rootId)));
    REQUIRE(e.mScene.mInstances.contains(rootId));
    CHECK(e.mScene.mInstances.at(rootId).mPrefab == record.mPrefab);
    CHECK(e.mScene.mInstances.at(rootId).mBindings == record.mBindings);
    CHECK(Differences(e.Saved(), placed) == std::vector<std::string>{});
}

TEST_CASE("A Prefab that cannot be placed records no step", "[editor][history]")
{
    Editing e;
    e.mOps.mPlaceFails = true;

    e.Run(MakePlacePrefabCommand("assets/prefabs/Missing.prefab", kNullEntity, mir::WorldVec3(0.0)));
    CHECK(e.mHistory.StepCount() == 0);
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

TEST_CASE("Duplicating an instance root keeps the record through undo and redo", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);
    const StableId sourceRoot = MakeInstance(e, s);
    const StableId sourceY = e.IdOf(s.y);

    const Files before = e.Saved();
    e.Run(MakeDuplicateCommand(s.a));
    const Files duplicated = e.Saved();

    CHECK(e.mHistory.StepCount() == 1);
    REQUIRE(e.Selected().size() == 1);
    const Entity copy = e.Selected().front();
    const StableId copyRoot = e.IdOf(copy);
    REQUIRE(copyRoot != kNullStableId);
    REQUIRE(e.mScene.mInstances.contains(copyRoot));

    const ScenePrefabInstance &copyRecord = e.mScene.mInstances.at(copyRoot);
    CHECK(copyRecord.mPrefab == e.mScene.mInstances.at(sourceRoot).mPrefab);
    CHECK(copyRecord.mRoot == copyRoot);
    CHECK(copyRecord.mBindings.at(SceneNodeAddress{ 2 }) != e.mScene.mInstances.at(sourceRoot).mBindings.at(
        SceneNodeAddress{ 2 }));
    CHECK(copyRecord.mBindings.at(SceneNodeAddress{ 3 }) != sourceY);
    CHECK(copyRecord.mBindings.size() == 4);

    const auto patch = FindInstanceFieldPatch(copyRecord, SceneNodeAddress{ 1 }, "EditorHistoryTestRef", "target");
    REQUIRE(patch != nullptr);
    CHECK(patch->mValue == nlohmann::json(copyRecord.mBindings.at(SceneNodeAddress{ 3 })));
    CHECK(TargetOf(e.mWorld, copy) == e.Find(copyRecord.mBindings.at(SceneNodeAddress{ 3 })));
    CHECK(TargetOf(e.mWorld, s.a) == s.y);

    e.Undo();
    CHECK(e.mHistory.StepCount() == 1);
    CHECK(e.mScene.mInstances.size() == 1);
    CHECK(e.mScene.mInstances.contains(sourceRoot));
    CHECK(Differences(WithoutNextId(e.Saved()), WithoutNextId(before)) == std::vector<std::string>{});
    CHECK(e.Selected().empty());

    e.Redo();
    CHECK(e.mScene.mInstances.size() == 2);
    REQUIRE(e.mScene.mInstances.contains(copyRoot));
    CHECK(Differences(e.Saved(), duplicated) == std::vector<std::string>{});
    REQUIRE(e.Selected().size() == 1);
    CHECK(e.IdOf(e.Selected().front()) == copyRoot);
}

TEST_CASE("Duplicating an instance member records no step", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);
    const std::size_t entities = e.mScene.mEntities.size();
    MakeInstance(e, s);

    e.Run(MakeDuplicateCommand(s.y));

    CHECK(e.mHistory.StepCount() == 0);
    CHECK(e.mScene.mEntities.size() == entities);
    CHECK(e.mScene.mInstances.size() == 1);
}

TEST_CASE("Copy as Ordinary Object is one step that undo and redo restore", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);
    const StableId sourceRoot = MakeInstance(e, s);
    const StableId sourceY = e.IdOf(s.y);
    const StableId sourceW = e.IdOf(s.w);
    const std::size_t instances = e.mScene.mInstances.size();

    const Files before = e.Saved();
    e.Run(MakeCopyAsOrdinaryObjectCommand(s.y));
    const Files copied = e.Saved();

    CHECK(e.mHistory.StepCount() == 1);
    REQUIRE(e.Selected().size() == 1);
    const Entity copy = e.Selected().front();
    const StableId copyId = e.IdOf(copy);
    REQUIRE(copyId != kNullStableId);
    CHECK(copyId != sourceY);

    CHECK(e.mScene.mInstances.size() == instances);
    CHECK_FALSE(BindsMember(e.mScene, copyId));
    CHECK(mir::ParentOf(e.mWorld, copy) == mir::ParentOf(e.mWorld, s.a));
    CHECK(mir::ResolveWorld(e.mWorld, copy).position.Meters() == mir::ResolveWorld(e.mWorld, s.y).position.Meters());

    const std::vector<Entity> copiedChildren = ChildrenOf(e.mWorld, copy);
    REQUIRE(copiedChildren.size() == 1);
    CHECK(TargetOf(e.mWorld, copy) == copiedChildren.front());
    CHECK(TargetOf(e.mWorld, copiedChildren.front()) == s.x);

    CHECK(TargetOf(e.mWorld, s.y) == s.w);
    CHECK(mir::ParentOf(e.mWorld, s.w) == s.y);
    CHECK(e.mScene.mInstances.contains(sourceRoot));
    CHECK(e.mScene.mInstances.at(sourceRoot).mBindings.at(SceneNodeAddress{ 3 }) == sourceY);
    CHECK(sourceW != copyId);

    e.Undo();
    CHECK(e.Selected().empty());
    CHECK(Differences(WithoutNextId(e.Saved()), WithoutNextId(before)) == std::vector<std::string>{});

    e.Redo();
    CHECK(Differences(e.Saved(), copied) == std::vector<std::string>{});
    REQUIRE(e.Selected().size() == 1);
    CHECK(e.IdOf(e.Selected().front()) == copyId);
}

/**
 * @file EntityCommands.tests.cpp
 * @author Dong Ta
 * @brief Create, delete, duplicate and reparent: do, undo and redo, compared through saved scene files.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/Selection.h>
#include <editor/history/EntityCommands.h>
#include <editor/history/History.h>
#include <editor/history/SceneOps.h>
#include <editor/panels/HierarchyPanel.h>

#include <scene/EntitySnapshot.h>
#include <scene/SceneAsset.h>
#include <scene/SceneIO.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EntityPresetRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/Transform.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using mir::CapturedSubtree;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::Entity;
    using mir::EntityAction;
    using mir::EntityActionKind;
    using mir::EntityPresetDesc;
    using mir::FieldKind;
    using mir::History;
    using mir::ICommand;
    using mir::kNullEntity;
    using mir::kNullStableId;
    using mir::LoadedScene;
    using mir::MakeCreateCommand;
    using mir::MakeDeleteCommand;
    using mir::MakeDuplicateCommand;
    using mir::MakeReParentCommand;
    using mir::RuntimeFieldDecl;
    using mir::Selection;
    using mir::StableId;
    using mir::Transform;
    using mir::World;

    using Files = std::map<std::string, std::string>;

    //the calls App makes, over a scene the test owns
    class TestSceneOps final : public mir::ISceneOps
    {
    public:
        TestSceneOps(World &world, LoadedScene &scene) : mWorld(world), mScene(scene) {}

        Entity CreateEntity() override { return mir::CreateSceneEntity(mWorld, mScene); }
        Entity Duplicate(Entity source) override { return mir::DuplicateSceneEntity(mWorld, mScene, source); }

        CapturedSubtree Capture(Entity root) override
        {
            CapturedSubtree subtree;
            subtree.mMembers = mir::SnapshotMembers(mWorld, root);
            subtree.mData = mir::CaptureEntity(mWorld, root).mData;

            std::unordered_map<uint64_t, StableId> ids;
            for (const auto &[id, entity] : mScene.mEntities)
                ids[mir::PackEntity(entity)] = id;
            for (const Entity member : subtree.mMembers)
            {
                const auto it = ids.find(mir::PackEntity(member));
                subtree.mStableIds.push_back(it != ids.end() ? it->second : kNullStableId);
            }
            return subtree;
        }

        std::vector<Entity> Restore(const CapturedSubtree &subtree, Entity parent, Entity before) override
        {
            return mir::RestoreEntityAt(mWorld, mir::EntitySnapshot{ subtree.mData }, mScene, subtree.mStableIds,
                parent, before);
        }

        bool Destroy(Entity root) override { return mir::DestroySceneEntity(mWorld, mScene, root); }

    private:
        World &mWorld;
        LoadedScene &mScene;
    };

    const ComponentOps &RefOps()
    {
        static const RuntimeFieldDecl fields[] = { { "target", FieldKind::EntityRef } };
        return ComponentRegistry::Instance().RegisterRuntime("EditorHistoryTestRef", fields);
    }

    void SetTarget(World &world, Entity referrer, Entity target)
    {
        const ComponentOps &ops = RefOps();
        if (!ops.Has(world, referrer))
            ops.AddDefault(world, referrer);
        ops.FindField("target")->Write(ops.GetComponent(world, referrer), &target);
    }

    Entity TargetOf(World &world, Entity referrer)
    {
        const ComponentOps &ops = RefOps();
        Entity target = kNullEntity;
        ops.FindField("target")->Read(ops.GetComponent(world, referrer), &target);
        return target;
    }

    Files ReadTree(const std::filesystem::path &root)
    {
        Files files;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
                continue;

            std::ifstream file(entry.path(), std::ios::binary);
            files[std::filesystem::relative(entry.path(), root).generic_string()] =
                std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        return files;
    }

    //each file that differs, with both versions
    std::vector<std::string> Differences(const Files &now, const Files &was)
    {
        std::vector<std::string> out;
        const auto describe = [](const Files &files, const std::string &path)
            {
                const auto it = files.find(path);
                return it != files.end() ? it->second : std::string("(missing)");
            };

        Files all = now;
        all.insert(was.begin(), was.end());
        for (const auto &[path, unused] : all)
        {
            if (describe(now, path) != describe(was, path))
                out.push_back(path + "\n  was: " + describe(was, path) + "\n  now: " + describe(now, path));
        }
        return out;
    }

    //undo keeps the StableId high-water mark, so an id is never handed out twice
    Files WithoutNextId(Files files)
    {
        std::string &manifest = files.at("scene.json");
        manifest = std::regex_replace(manifest, std::regex(R"("nextId": \d+)"), R"("nextId": 0)");
        return files;
    }

    struct Editing
    {
        World mWorld;
        LoadedScene mScene = mir::NewScene("test");
        TestSceneOps mOps{ mWorld, mScene };
        History mHistory;

        Editing()
        {
            mir::RegisterCoreComponents();
            mWorld.EmplaceResource<Selection>();
        }

        void Run(std::unique_ptr<ICommand> command)
        {
            mHistory.Submit(std::move(command));
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

        void Flush() { mHistory.Flush(mWorld, mOps); }

        Entity Add(const char *name, Entity parent = kNullEntity)
        {
            const Entity entity = mir::CreateSceneEntity(mWorld, mScene, parent);
            mWorld.AddComponent<mir::Name>(entity, mir::MakeName(name));
            return entity;
        }

        StableId IdOf(Entity entity) const
        {
            for (const auto &[id, candidate] : mScene.mEntities)
            {
                if (candidate == entity)
                    return id;
            }
            return kNullStableId;
        }

        Entity Find(StableId id) const { return mScene.mEntities.at(id); }

        std::vector<Entity> &Selected() { return mWorld.Resource<Selection>().mSelected; }

        //every file a save writes now
        Files Saved()
        {
            static const std::string run = std::to_string(std::random_device{}());
            static int count = 0;
            const std::filesystem::path dir =
                std::filesystem::temp_directory_path() / "mir_editor_tests" / (run + "_" + std::to_string(count++));

            //what TransformPropagateSystem does each frame, before the user can save
            mWorld.ForEachAliveEntity([this](Entity entity) { mir::ResolveWorld(mWorld, entity); });

            std::filesystem::remove_all(dir);
            REQUIRE(mir::SaveScene(mWorld, dir, mScene));
            Files files = ReadTree(dir);
            std::filesystem::remove_all(dir);
            return files;
        }
    };

    //roots [b, a], a's children [x, y, z], w under y.
    //y -> w stays inside y, w -> x leaves it, b -> y comes into it
    struct Sample
    {
        Entity a, b, x, y, z, w;
    };

    Sample BuildSample(Editing &e)
    {
        Sample s;
        s.a = e.Add("a");
        s.b = e.Add("b");
        s.x = e.Add("x", s.a);
        s.y = e.Add("y", s.a);
        s.z = e.Add("z", s.a);
        s.w = e.Add("w", s.y);

        Transform transform{ glm::vec3(1.5f, -2.25f, 0.1f) };
        transform.SetRotation(glm::angleAxis(0.7f, glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f))));
        transform.SetScale(glm::vec3(0.3f, 2.0f, 1.0f));
        for (const Entity entity : { s.a, s.x, s.y, s.z, s.w })
            mir::AddTransform(e.mWorld, entity, transform);

        Transform other{ glm::vec3(-4.0f, 0.5f, 2.0f) };
        other.SetRotation(glm::angleAxis(1.2f, glm::vec3(0.0f, 1.0f, 0.0f)));
        mir::AddTransform(e.mWorld, s.b, other);

        SetTarget(e.mWorld, s.y, s.w);
        SetTarget(e.mWorld, s.w, s.x);
        SetTarget(e.mWorld, s.b, s.y);

        REQUIRE(mir::SetParent(e.mWorld, s.b, kNullEntity, s.a));
        return s;
    }

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

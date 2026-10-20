/**
 * @file EditingFixture.h
 * @author Dong Ta
 * @brief A World, a scene, history and real scene ops for command tests, compared through saved scene files.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/Selection.h>
#include <editor/history/History.h>
#include <editor/history/SceneOps.h>

#include <scene/EntitySnapshot.h>
#include <scene/SceneAsset.h>
#include <scene/SceneIO.h>

#include <core/ecs/ComponentRegistry.h>
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
    using mir::FieldKind;
    using mir::History;
    using mir::ICommand;
    using mir::kNullEntity;
    using mir::kNullStableId;
    using mir::LoadedScene;
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
}

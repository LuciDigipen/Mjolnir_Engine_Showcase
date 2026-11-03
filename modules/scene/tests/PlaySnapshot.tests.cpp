/**
 * @file PlaySnapshot.tests.cpp
 * @author Sumin Park
 * @brief World capture, copy and restore tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <scene/PlaySnapshot.h>
#include <scene/SceneIO.h>
#include <scene/SceneInstancePatch.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using mir::AddTransform;
    using mir::CaptureWorld;
    using mir::CreateSceneEntity;
    using mir::Entity;
    using mir::LoadedScene;
    using mir::NewScene;
    using mir::PlaySnapshot;
    using mir::RestoreWorld;
    using mir::DiscardSnapshot;
    using mir::Transform;
    using mir::World;
}

TEST_CASE("RestoreWorld reverts a Transform mutated after CaptureWorld", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity entity = CreateSceneEntity(world, scene);
    AddTransform(world, entity, Transform{ glm::vec3(1.0f, 2.0f, 3.0f) });

    PlaySnapshot snapshot = CaptureWorld(world, scene);

    // "Play" mutates it.
    Transform *transform = world.GetComponent<Transform>(entity);
    REQUIRE(transform != nullptr);
    transform->SetPosition(glm::vec3(99.0f, 99.0f, 99.0f));

    const LoadedScene restored = RestoreWorld(world, snapshot);
    DiscardSnapshot(snapshot);

    REQUIRE(restored.mEntities.size() == 1);
    const Entity restoredEntity = restored.mEntities.begin()->second;

    const Transform *restoredTransform = world.GetComponent<Transform>(restoredEntity);
    REQUIRE(restoredTransform != nullptr);
    CHECK(restoredTransform->Position() == mir::WorldVec3(1.0, 2.0, 3.0));
}

TEST_CASE("CopyWorld seeds Play from unsaved edits and leaves authoring entities intact", "[scene]")
{
    mir::RegisterCoreComponents();

    World authoring;
    LoadedScene scene = NewScene("unsaved");
    const Entity authored = CreateSceneEntity(authoring, scene);
    AddTransform(authoring, authored, Transform{ glm::vec3(3.0f, 4.0f, 5.0f) });
    scene.mDirty = true;

    World runtime;
    LoadedScene played = mir::CopyWorld(authoring, scene, runtime);
    REQUIRE(played.mEntities.size() == 1);
    CHECK(played.mDirty);
    CHECK(played.mName == scene.mName);
    CHECK(played.mNextId == scene.mNextId);
    CHECK_FALSE(std::filesystem::exists(std::filesystem::temp_directory_path() /
        ("mjolnir_play_snapshot_" + std::to_string(mir::CurrentProcessId()))));

    const Entity running = played.mEntities.begin()->second;
    runtime.GetComponent<Transform>(running)->SetPosition(glm::vec3(99.0f));
    runtime.DestroyEntity(running);
    CHECK(authoring.IsAlive(authored));
    CHECK(authoring.GetComponent<Transform>(authored)->Position() == mir::WorldVec3(3.0, 4.0, 5.0));
    CHECK(scene.mDirty);
}

TEST_CASE("RestoreWorld reverts a far root and its child exactly", "[scene]")
{
    mir::RegisterCoreComponents();

    const mir::WorldVec3 rootPosition = mir::WorldVec3(150000.625, -20.0, -75000.25) + mir::WorldVec3(0.001, -0.002, 0.003);
    const mir::WorldVec3 childLocal{0.001, 0.0, -0.002};

    World world;
    LoadedScene scene = NewScene("test");
    Entity root = CreateSceneEntity(world, scene);
    Entity child = CreateSceneEntity(world, scene, root);
    AddTransform(world, root, Transform{ rootPosition });
    AddTransform(world, child, Transform{ childLocal }, root);
    const mir::WorldVec3 childWorld = mir::ResolveWorld(world, child).position.Meters();

    const PlaySnapshot snapshot = CaptureWorld(world, scene);

    // "Play" moves both by an amount a float at this distance cannot hold
    REQUIRE(world.GetComponent<Transform>(root)->Translate(mir::WorldVec3(0.004)));
    REQUIRE(world.GetComponent<Transform>(child)->Translate(mir::WorldVec3(0.004)));

    const LoadedScene restored = RestoreWorld(world, snapshot);
    REQUIRE(restored.mEntities.size() == 2);

    Entity restoredRoot{};
    Entity restoredChild{};
    for (const auto &[id, entity] : restored.mEntities)
        (mir::ParentOf(world, entity).IsNull() ? restoredRoot : restoredChild) = entity;
    REQUIRE(mir::ParentOf(world, restoredChild) == restoredRoot);

    CHECK(world.GetComponent<Transform>(restoredRoot)->Position() == rootPosition);
    CHECK(world.GetComponent<Transform>(restoredChild)->Position() == childLocal);
    CHECK(mir::ResolveWorld(world, restoredChild).position.Meters() == childWorld);
}

TEST_CASE("RestoreWorld destroys an entity Play spawned that CaptureWorld never saw", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity original = CreateSceneEntity(world, scene);
    AddTransform(world, original);

    PlaySnapshot snapshot = CaptureWorld(world, scene);

    // "Play" spawns something the edit-time scene never tracked.
    Entity spawned = world.CreateEntity();
    AddTransform(world, spawned);
    REQUIRE(world.IsAlive(spawned));

    RestoreWorld(world, snapshot);
    DiscardSnapshot(snapshot);

    CHECK_FALSE(world.IsAlive(spawned));
    CHECK_FALSE(world.IsAlive(original)); // restore gets a new handle, not the old one
}

TEST_CASE("The snapshot's directory is this process's own, and DiscardSnapshot removes it", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    AddTransform(world, CreateSceneEntity(world, scene));

    PlaySnapshot snapshot = CaptureWorld(world, scene);
    const std::filesystem::path dir = snapshot.mDir;
    CHECK(dir.filename().string() == "mjolnir_play_snapshot_" + std::to_string(mir::CurrentProcessId()));
    REQUIRE(std::filesystem::exists(dir));

    DiscardSnapshot(snapshot);
    CHECK_FALSE(std::filesystem::exists(dir));
    CHECK(snapshot.mDir.empty());

    // twice is harmless
    DiscardSnapshot(snapshot);
}

namespace
{
    float MarkerOf(World &world, Entity entity) { return static_cast<float>(world.GetComponent<Transform>(entity)->Position().x); }

    Entity MakeMarked(World &world, LoadedScene &scene, float marker, Entity parent = mir::kNullEntity)
    {
        const Entity entity = CreateSceneEntity(world, scene, parent);
        AddTransform(world, entity, Transform{ glm::vec3(marker, 0.0f, 0.0f) });
        return entity;
    }
}

TEST_CASE("RestoreWorld keeps root and child order", "[scene][order]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    const Entity a = MakeMarked(world, scene, 1.0f);
    MakeMarked(world, scene, 2.0f);
    const Entity c = MakeMarked(world, scene, 3.0f);
    const Entity x = MakeMarked(world, scene, 4.0f, a);
    const Entity y = MakeMarked(world, scene, 5.0f, a);

    REQUIRE(mir::SetParent(world, c, mir::kNullEntity, a));
    REQUIRE(mir::SetParent(world, y, a, x));

    PlaySnapshot snapshot = CaptureWorld(world, scene);
    RestoreWorld(world, snapshot);
    DiscardSnapshot(snapshot);

    const mir::HierarchyIndex &index = world.HierarchyIndex();
    std::vector<float> roots;
    for (const Entity root : index.Roots())
        roots.push_back(MarkerOf(world, root));
    REQUIRE(roots == std::vector<float>{ 3.0f, 1.0f, 2.0f });

    std::vector<float> children;
    for (const Entity child : index.ChildrenOf(index.Roots()[1]))
        children.push_back(MarkerOf(world, child));
    CHECK(children == std::vector<float>{ 5.0f, 4.0f });
}

namespace
{
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::FieldKind;
    using mir::kNullStableId;
    using mir::RuntimeFieldDecl;
    using mir::SceneFieldPatch;
    using mir::SceneInstancePatch;
    using mir::SceneNodeAddress;
    using mir::ScenePrefabInstance;
    using mir::ScenePrefabResolver;
    using mir::StableId;

    const char *kPrefabUuid = "11111111-2222-4333-8444-555555555555";

    const ComponentOps &MarkOps()
    {
        static const RuntimeFieldDecl fields[] = { { "value", FieldKind::Float } };
        return ComponentRegistry::Instance().RegisterRuntime("PlaySnapshotTestMark", fields);
    }

    struct FakeSceneResolver final : ScenePrefabResolver
    {
        std::map<SceneNodeAddress, Entity> Materialize(World &world, const ScenePrefabInstance &) override
        {
            const Entity root = world.CreateEntity();
            AddTransform(world, root, Transform{ glm::vec3(1.0f, 2.0f, 3.0f) });
            const Entity child = world.CreateEntity();
            AddTransform(world, child, Transform{ glm::vec3(0.5f, 0.0f, 0.0f) }, root);

            MarkOps().AddDefault(world, root);
            float value = 1.0f;
            MarkOps().FindField("value")->Write(MarkOps().GetComponent(world, root), &value);
            return { { SceneNodeAddress{ 1 }, root }, { SceneNodeAddress{ 2 }, child } };
        }

        bool TemplateNodes(const ScenePrefabInstance &, std::set<SceneNodeAddress> &outNodes) const override
        {
            outNodes = { SceneNodeAddress{ 1 }, SceneNodeAddress{ 2 } };
            return true;
        }

        bool TemplateEntityRef(const ScenePrefabInstance &, const SceneNodeAddress &, std::string_view,
            std::string_view, std::optional<SceneNodeAddress> &) const override
        {
            return false;
        }
    };

    StableId PlaceInstance(World &world, LoadedScene &scene, ScenePrefabResolver &resolver)
    {
        ScenePrefabInstance seed;
        seed.mPrefab = kPrefabUuid;
        const std::map<SceneNodeAddress, Entity> members = resolver.Materialize(world, seed);

        ScenePrefabInstance instance;
        instance.mPrefab = kPrefabUuid;
        for (const auto &[address, entity] : members)
        {
            const StableId id = mir::AllocateStableId(scene);
            scene.mEntities[id] = entity;
            instance.mBindings[address] = id;
            if (address == SceneNodeAddress{ 1 })
                instance.mRoot = id;
        }
        if (instance.mRoot == kNullStableId)
            return kNullStableId;

        SceneInstancePatch patch;
        patch.mTarget = SceneNodeAddress{ 1 };
        patch.mFields.push_back(SceneFieldPatch{ "PlaySnapshotTestMark", "value", 4.0f });
        instance.mPatches.push_back(std::move(patch));

        const StableId root = instance.mRoot;
        scene.mInstances[root] = std::move(instance);
        return root;
    }

    struct TempSceneDir
    {
        std::filesystem::path mPath;

        explicit TempSceneDir(std::string_view name)
            : mPath(std::filesystem::temp_directory_path() / "mir_play_snapshot_tests" / name)
        {
            std::filesystem::remove_all(mPath);
        }

        ~TempSceneDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(mPath, ec);
        }

        TempSceneDir(const TempSceneDir &) = delete;
        TempSceneDir &operator=(const TempSceneDir &) = delete;
    };

    std::string ReadText(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::map<std::string, std::string> SnapshotFiles(const std::filesystem::path &root)
    {
        std::map<std::string, std::string> files;
        for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
                continue;
            files[entry.path().lexically_relative(root).generic_string()] = ReadText(entry.path());
        }
        return files;
    }

    std::vector<Entity> UntrackedAlive(World &world, const LoadedScene &scene)
    {
        std::set<uint64_t> tracked;
        for (const auto &[id, entity] : scene.mEntities)
            tracked.insert(mir::PackEntity(entity));

        std::vector<Entity> untracked;
        world.ForEachAliveEntity([&](Entity entity)
            {
                if (!tracked.contains(mir::PackEntity(entity)))
                    untracked.push_back(entity);
            });
        return untracked;
    }
}

TEST_CASE("RestoreWorld keeps the scene's name, ids and high-water mark", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("edit");
    const Entity kept = CreateSceneEntity(world, scene);
    AddTransform(world, kept, Transform{ glm::vec3(1.0f, 0.0f, 0.0f) });
    const Entity removed = CreateSceneEntity(world, scene);
    AddTransform(world, removed);
    REQUIRE(mir::DestroySceneEntity(world, scene, removed));

    const Entity untracked = world.CreateEntity();
    AddTransform(world, untracked, Transform{ glm::vec3(7.0f, 0.0f, 0.0f) });

    const mir::StableId nextId = scene.mNextId;
    PlaySnapshot snapshot = CaptureWorld(world, scene);

    world.GetComponent<Transform>(kept)->SetPosition(glm::vec3(99.0f, 0.0f, 0.0f));
    world.GetComponent<Transform>(untracked)->SetPosition(glm::vec3(98.0f, 0.0f, 0.0f));
    AddTransform(world, world.CreateEntity());

    const LoadedScene restored = RestoreWorld(world, snapshot);
    DiscardSnapshot(snapshot);

    CHECK(restored.mName == "edit");
    CHECK(restored.mNextId == nextId);
    REQUIRE(restored.mEntities.size() == 1);
    REQUIRE(restored.mEntities.contains(1));

    const Transform *restoredTransform = world.GetComponent<Transform>(restored.mEntities.at(1));
    REQUIRE(restoredTransform != nullptr);
    CHECK(restoredTransform->Position() == mir::WorldVec3(1.0, 0.0, 0.0));

    const std::vector<Entity> untrackedAlive = UntrackedAlive(world, restored);
    REQUIRE(untrackedAlive.size() == 1);
    const Transform *untrackedTransform = world.GetComponent<Transform>(untrackedAlive.front());
    REQUIRE(untrackedTransform != nullptr);
    CHECK(untrackedTransform->Position() == mir::WorldVec3(7.0, 0.0, 0.0));
}

TEST_CASE("RestoreWorld keeps the scene's dirty flag", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    AddTransform(world, CreateSceneEntity(world, scene));

    SECTION("dirty scene") { scene.mDirty = true; }
    SECTION("clean scene") { scene.mDirty = false; }

    PlaySnapshot snapshot = CaptureWorld(world, scene);
    const LoadedScene restored = RestoreWorld(world, snapshot);
    DiscardSnapshot(snapshot);

    CHECK(restored.mDirty == scene.mDirty);
}

TEST_CASE("Play, Stop and Save leave an unchanged scene's files identical", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    MarkOps();

    World authoredWorld;
    LoadedScene authored = NewScene("edit");
    const Entity ordinary = CreateSceneEntity(authoredWorld, authored);
    AddTransform(authoredWorld, ordinary, Transform{ glm::vec3(9.0f, 8.0f, 7.0f) });
    const Entity dropped = CreateSceneEntity(authoredWorld, authored);
    AddTransform(authoredWorld, dropped);
    REQUIRE(mir::DestroySceneEntity(authoredWorld, authored, dropped));

    FakeSceneResolver resolver;
    const mir::StableId root = PlaceInstance(authoredWorld, authored, resolver);
    REQUIRE(root != mir::kNullStableId);

    TempSceneDir original("play_stop_save_original");
    REQUIRE(mir::SaveScene(authoredWorld, original.mPath, authored, &resolver));

    World world;
    LoadedScene scene = mir::LoadScene(world, original.mPath, &resolver);

    const Entity untracked = world.CreateEntity();
    AddTransform(world, untracked, Transform{ glm::vec3(6.0f, 5.0f, 4.0f) });

    const std::map<mir::StableId, Entity> originalEntities = scene.mEntities;
    const mir::ScenePrefabInstance originalInstance = scene.mInstances.at(root);
    const mir::StableId originalNextId = scene.mNextId;
    REQUIRE_FALSE(originalInstance.mPlacement.empty());

    PlaySnapshot snapshot = CaptureWorld(world, scene, &resolver);

    world.GetComponent<Transform>(scene.mEntities.at(1))->SetPosition(glm::vec3(10.0f, 10.0f, 10.0f));
    world.GetComponent<Transform>(scene.mEntities.at(root))->SetPosition(glm::vec3(20.0f, 20.0f, 20.0f));
    world.GetComponent<Transform>(untracked)->SetPosition(glm::vec3(30.0f, 30.0f, 30.0f));
    AddTransform(world, world.CreateEntity());
    world.DestroyEntity(scene.mEntities.at(1));

    const LoadedScene restored = RestoreWorld(world, snapshot, &resolver);
    DiscardSnapshot(snapshot);

    CHECK(restored.mName == "edit");
    CHECK(restored.mNextId == originalNextId);
    CHECK_FALSE(restored.mDirty);
    REQUIRE(restored.mEntities.size() == originalEntities.size());
    for (const auto &entry : originalEntities)
        CHECK(restored.mEntities.contains(entry.first));

    REQUIRE(restored.mInstances.size() == 1);
    const mir::ScenePrefabInstance &restoredInstance = restored.mInstances.at(root);
    CHECK(restoredInstance.mPrefab == originalInstance.mPrefab);
    CHECK(restoredInstance.mRoot == originalInstance.mRoot);
    CHECK(restoredInstance.mParent == originalInstance.mParent);
    CHECK(restoredInstance.mSiblingIndex == originalInstance.mSiblingIndex);
    CHECK(restoredInstance.mPlacement == originalInstance.mPlacement);
    CHECK(restoredInstance.mBindings == originalInstance.mBindings);
    REQUIRE(restoredInstance.mPatches.size() == originalInstance.mPatches.size());
    REQUIRE(restoredInstance.mPatches.at(0).mFields.size() == 1);
    CHECK(restoredInstance.mPatches.at(0).mFields.at(0).mValue ==
        originalInstance.mPatches.at(0).mFields.at(0).mValue);

    const std::vector<Entity> untrackedAlive = UntrackedAlive(world, restored);
    REQUIRE(untrackedAlive.size() == 1);
    const Transform *untrackedTransform = world.GetComponent<Transform>(untrackedAlive.front());
    REQUIRE(untrackedTransform != nullptr);
    CHECK(untrackedTransform->Position() == mir::WorldVec3(6.0, 5.0, 4.0));

    TempSceneDir after("play_stop_save_after");
    REQUIRE(mir::SaveScene(world, after.mPath, restored, &resolver));
    CHECK(SnapshotFiles(original.mPath) == SnapshotFiles(after.mPath));
}

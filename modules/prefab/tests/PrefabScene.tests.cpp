/**
 * @file PrefabScene.tests.cpp
 * @author Jackey Cen
 * @brief Tests for prefab/PrefabSceneInstance.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/PrefabSceneInstance.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <core/math/WorldReal.h>
#include <prefab/PrefabFormat.h>
#include <prefab/PrefabInstantiate.h>
#include <serial/JsonFile.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using nlohmann::json;
    using mir::CompiledPrefab;
    using mir::CompiledPrefabCache;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::CreateSceneEntity;
    using mir::Entity;
    using mir::FieldKind;
    using mir::HasPrefabErrors;
    using mir::kNullStableId;
    using mir::LoadedScene;
    using mir::NewScene;
    using mir::PlacePrefabInstance;
    using mir::PrefabComponentInstance;
    using mir::PrefabDocument;
    using mir::PrefabParseResult;
    using mir::PrefabRegistry;
    using mir::PrefabSceneResolver;
    using mir::RuntimeFieldDecl;
    using mir::SceneNodeAddress;
    using mir::ScenePrefabInstance;
    using mir::StableId;
    using mir::Transform;
    using mir::World;
    using mir::WorldVec3;

    const char *kUuid = "11111111-2222-4333-8444-555555555555";

    const ComponentOps &LinkOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "target", FieldKind::EntityRef },
            { "other", FieldKind::EntityRef },
            { "speed", FieldKind::Float },
        };
        return ComponentRegistry::Instance().RegisterRuntime("PrefabSceneTestLink", fields);
    }

    const ComponentOps &TagOps()
    {
        return ComponentRegistry::Instance().RegisterRuntime("PrefabSceneTestTag", {});
    }

    const std::string &ProcessToken()
    {
        static const std::string token = []
            {
                std::random_device rd;
                std::ostringstream out;
                out << std::hex << ((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
                return out.str();
            }();
        return token;
    }

    struct TempDir
    {
        std::filesystem::path mPath;

        TempDir()
        {
            static std::atomic<int> counter{ 0 };
            mPath = std::filesystem::temp_directory_path() /
                ("mir_prefab_scene_test_" + ProcessToken() + "_" + std::to_string(counter.fetch_add(1)));
            std::filesystem::create_directories(mPath);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(mPath, ec);
        }

        TempDir(const TempDir &) = delete;
        TempDir &operator=(const TempDir &) = delete;
    };

    PrefabDocument ParseDocument(const json &root)
    {
        const PrefabParseResult parsed = mir::ParsePrefab(root);
        REQUIRE_FALSE(HasPrefabErrors(parsed.mDiagnostics));
        REQUIRE(parsed.mDocument.has_value());
        return *parsed.mDocument;
    }

    json SceneDocumentJson()
    {
        json root;
        root["formatVersion"] = 1;
        root["prefabId"] = kUuid;
        root["nextNodeId"] = "4";
        root["rootNode"] = "1";
        root["nodes"] = json::array({
            json{
                { "id", "1" },
                { "kind", "entity" },
                { "components", json::array({
                    json{ { "type", "Transform" }, { "fields", json{
                        { "position", json::array({ 1.0, 2.0, 3.0 }) },
                    } } },
                    json{ { "type", "PrefabSceneTestLink" }, { "fields", json{
                        { "target", json::array({ "3" }) },
                        { "speed", 2.0 },
                    } } },
                }) },
            },
            json{
                { "id", "2" },
                { "kind", "entity" },
                { "parent", "1" },
                { "components", json::array({
                    json{ { "type", "Transform" }, { "fields", json{
                        { "position", json::array({ 0.0, 4.0, 0.0 }) },
                    } } },
                }) },
            },
            json{
                { "id", "3" },
                { "kind", "entity" },
                { "parent", "1" },
                { "components", json::array({
                    json{ { "type", "Transform" }, { "fields", json{
                        { "position", json::array({ 0.0, 0.0, 5.0 }) },
                    } } },
                }) },
            },
        });
        return root;
    }

    Entity FieldEntity(World &world, Entity entity, const ComponentOps &ops, std::string_view field)
    {
        void *component = ops.GetComponent(world, entity);
        REQUIRE(component != nullptr);
        const mir::FieldDesc *desc = ops.FindField(field);
        REQUIRE(desc != nullptr);
        Entity value;
        desc->Read(component, &value);
        return value;
    }

    void SetFieldEntity(World &world, Entity entity, const ComponentOps &ops, std::string_view field, Entity value)
    {
        void *component = ops.GetComponent(world, entity);
        REQUIRE(component != nullptr);
        const mir::FieldDesc *desc = ops.FindField(field);
        REQUIRE(desc != nullptr);
        REQUIRE(desc->Write(component, &value));
    }

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

    Entity MemberOf(const LoadedScene &scene, StableId root, const SceneNodeAddress &address)
    {
        return scene.mEntities.at(scene.mInstances.at(root).mBindings.at(address));
    }
}

TEST_CASE("PlacePrefabInstance registers every node under a fresh StableId", "[prefab][scene]")
{
    mir::RegisterCoreComponents();
    LinkOps();
    TagOps();

    const PrefabDocument document = ParseDocument(SceneDocumentJson());
    PrefabRegistry registry;
    REQUIRE(registry.SetDocument(document));
    CompiledPrefabCache cache;
    REQUIRE_FALSE(HasPrefabErrors(cache.Compile(document)));
    const std::shared_ptr<const CompiledPrefab> prefab = cache.Find(document.mId);
    REQUIRE(prefab != nullptr);

    World world;
    LoadedScene scene = NewScene("test");
    const Entity parent = CreateSceneEntity(world, scene);
    mir::AddTransform(world, parent, Transform{ WorldVec3(9.0, 8.0, 7.0) });

    const StableId root = PlacePrefabInstance(world, scene, *prefab, parent);

    REQUIRE(root != kNullStableId);
    REQUIRE(scene.mInstances.size() == 1);
    const ScenePrefabInstance &instance = scene.mInstances.at(root);
    CHECK(instance.mPrefab == mir::ToString(document.mId));
    CHECK(instance.mRoot == root);
    CHECK(instance.mParent == 1);
    CHECK(instance.mSiblingIndex == 0);
    CHECK(instance.mBindings.size() == prefab->Nodes().size());
    CHECK(instance.mBindings.at(SceneNodeAddress{ 1 }) == root);
    CHECK(scene.mEntities.size() == prefab->Nodes().size() + 1);
    CHECK(scene.mDirty);

    const Entity rootEntity = scene.mEntities.at(root);
    CHECK(mir::ParentOf(world, rootEntity) == parent);
    REQUIRE(world.GetComponent<Transform>(rootEntity) != nullptr);
    CHECK(world.GetComponent<Transform>(rootEntity)->Position() == WorldVec3(1.0, 2.0, 3.0));
    CHECK(FieldEntity(world, rootEntity, LinkOps(), "target") == MemberOf(scene, root, SceneNodeAddress{ 3 }));
}

TEST_CASE("TemplateEntityRef reports the default target and a null default", "[prefab][scene]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    const PrefabDocument document = ParseDocument(SceneDocumentJson());
    PrefabRegistry registry;
    REQUIRE(registry.SetDocument(document));
    CompiledPrefabCache cache;
    REQUIRE_FALSE(HasPrefabErrors(cache.Compile(document)));
    PrefabSceneResolver resolver(registry, cache);

    ScenePrefabInstance instance;
    instance.mPrefab = kUuid;

    std::optional<SceneNodeAddress> target;
    CHECK(resolver.TemplateEntityRef(instance, SceneNodeAddress{ 1 }, "PrefabSceneTestLink", "target", target));
    CHECK(target == SceneNodeAddress{ 3 });

    CHECK(resolver.TemplateEntityRef(instance, SceneNodeAddress{ 1 }, "PrefabSceneTestLink", "other", target));
    CHECK_FALSE(target.has_value());

    CHECK_FALSE(resolver.TemplateEntityRef(instance, SceneNodeAddress{ 2 }, "PrefabSceneTestLink", "target", target));
    CHECK_FALSE(resolver.TemplateEntityRef(instance, SceneNodeAddress{ 1 }, "PrefabSceneTestLink", "speed", target));
    CHECK_FALSE(resolver.TemplateEntityRef(instance, SceneNodeAddress{ 1 }, "PrefabSceneTestMissing", "target", target));
}

TEST_CASE("Prefab scene instances round-trip ids and references", "[prefab][scene]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    const PrefabDocument document = ParseDocument(SceneDocumentJson());
    PrefabRegistry registry;
    REQUIRE(registry.SetDocument(document));
    CompiledPrefabCache cache;
    REQUIRE_FALSE(HasPrefabErrors(cache.Compile(document)));
    const std::shared_ptr<const CompiledPrefab> prefab = cache.Find(document.mId);
    REQUIRE(prefab != nullptr);
    PrefabSceneResolver resolver(registry, cache);

    World world;
    LoadedScene scene = NewScene("test");
    const Entity ordinary = CreateSceneEntity(world, scene);
    mir::AddTransform(world, ordinary, Transform{ WorldVec3(9.0, 8.0, 7.0) });
    LinkOps().AddDefault(world, ordinary);

    const StableId rootA = PlacePrefabInstance(world, scene, *prefab);
    const StableId rootB = PlacePrefabInstance(world, scene, *prefab);
    REQUIRE(rootA != kNullStableId);
    REQUIRE(rootB != kNullStableId);

    REQUIRE(mir::SetInstanceEntityRef(world, scene, rootA, SceneNodeAddress{ 1 }, "PrefabSceneTestLink", "target", 1));
    REQUIRE(mir::SetInstanceEntityRef(world, scene, rootA, SceneNodeAddress{ 1 }, "PrefabSceneTestLink", "other",
        kNullStableId));

    const Entity memberA = MemberOf(scene, rootA, SceneNodeAddress{ 2 });
    SetFieldEntity(world, ordinary, LinkOps(), "target", memberA);

    const StableId nextId = scene.mNextId;
    const std::map<SceneNodeAddress, StableId> bindingsA = scene.mInstances.at(rootA).mBindings;
    const std::map<SceneNodeAddress, StableId> bindingsB = scene.mInstances.at(rootB).mBindings;

    TempDir first;
    REQUIRE(mir::SaveScene(world, first.mPath, scene, &resolver));

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, first.mPath, &resolver);

    CHECK(loaded.mNextId == nextId);
    CHECK_FALSE(loaded.mDirty);
    REQUIRE(loaded.mInstances.size() == 2);
    CHECK(loaded.mInstances.at(rootA).mBindings == bindingsA);
    CHECK(loaded.mInstances.at(rootB).mBindings == bindingsB);

    const Entity loadedRootA = loaded.mEntities.at(rootA);
    const Entity loadedRootB = loaded.mEntities.at(rootB);
    const Entity loadedOrdinary = loaded.mEntities.at(1);

    CHECK(FieldEntity(loadedWorld, loadedRootA, LinkOps(), "target") == loadedOrdinary);
    CHECK(FieldEntity(loadedWorld, loadedRootA, LinkOps(), "other").IsNull());
    CHECK(FieldEntity(loadedWorld, loadedOrdinary, LinkOps(), "target") == MemberOf(loaded, rootA, SceneNodeAddress{ 2 }));
    CHECK(FieldEntity(loadedWorld, loadedRootB, LinkOps(), "target") == MemberOf(loaded, rootB, SceneNodeAddress{ 3 }));
    CHECK(FieldEntity(loadedWorld, loadedRootB, LinkOps(), "target") !=
        MemberOf(loaded, rootA, SceneNodeAddress{ 3 }));

    TempDir second;
    REQUIRE(mir::SaveScene(loadedWorld, second.mPath, loaded, &resolver));
    CHECK(SnapshotFiles(first.mPath) == SnapshotFiles(second.mPath));
}

TEST_CASE("An instance whose template root has no Transform round-trips", "[prefab][scene]")
{
    mir::RegisterCoreComponents();

    const PrefabDocument document = mir::NewPrefabDocument();
    PrefabRegistry registry;
    REQUIRE(registry.SetDocument(document));
    CompiledPrefabCache cache;
    REQUIRE_FALSE(HasPrefabErrors(cache.Compile(document)));
    const std::shared_ptr<const CompiledPrefab> prefab = cache.Find(document.mId);
    REQUIRE(prefab != nullptr);
    PrefabSceneResolver resolver(registry, cache);

    World world;
    LoadedScene scene = NewScene("test");
    const StableId root = PlacePrefabInstance(world, scene, *prefab);
    REQUIRE(root != kNullStableId);
    CHECK(world.GetComponent<Transform>(scene.mEntities.at(root)) == nullptr);
    CHECK(scene.mInstances.at(root).mPlacement.empty());

    TempDir first;
    REQUIRE(mir::SaveScene(world, first.mPath, scene, &resolver));

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, first.mPath, &resolver);
    CHECK(loadedWorld.GetComponent<Transform>(loaded.mEntities.at(root)) == nullptr);

    TempDir second;
    REQUIRE(mir::SaveScene(loadedWorld, second.mPath, loaded, &resolver));
    CHECK(SnapshotFiles(first.mPath) == SnapshotFiles(second.mPath));
}

TEST_CASE("A scene StableId cannot validate as a prefab EntityRef", "[prefab][scene]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    PrefabDocument document = mir::NewPrefabDocument();
    document.mNodes[0].mComponents.push_back(
        PrefabComponentInstance{ "PrefabSceneTestLink", json{ { "target", StableId{ 7 } } } });

    CHECK(HasPrefabErrors(mir::ValidatePrefab(document)));

    TempDir dir;
    CHECK_FALSE(mir::WritePrefabFile(dir.mPath / "invalid.prefab", document));
}

TEST_CASE("LoadScene keeps an instance whose prefab the resolver cannot find", "[prefab][scene]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    World world;
    LoadedScene scene = NewScene("test");
    ScenePrefabInstance instance;
    instance.mPrefab = kUuid;
    instance.mRoot = 1;
    instance.mBindings[SceneNodeAddress{ 1 }] = 1;
    scene.mInstances[1] = instance;
    scene.mNextId = 2;

    TempDir dir;
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    PrefabRegistry registry;
    CompiledPrefabCache cache;
    PrefabSceneResolver resolver(registry, cache);

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, &resolver);

    CHECK(loaded.mEntities.empty());
    REQUIRE(loaded.mInstances.size() == 1);
    CHECK(loaded.mInstances.at(1).mBindings == instance.mBindings);
    CHECK_FALSE(loaded.mDirty);
}

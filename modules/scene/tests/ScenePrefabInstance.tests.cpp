/**
 * @file ScenePrefabInstance.tests.cpp
 * @author Jackey Cen
 * @brief Tests for scene/ScenePrefabInstance.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <scene/SceneIO.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <core/math/WorldReal.h>
#include <serial/JsonFile.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using nlohmann::json;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::CreateSceneEntity;
    using mir::Entity;
    using mir::FieldKind;
    using mir::kNullEntity;
    using mir::kNullStableId;
    using mir::LoadedScene;
    using mir::NewScene;
    using mir::RuntimeFieldDecl;
    using mir::SceneNodeAddress;
    using mir::ScenePrefabInstance;
    using mir::ScenePrefabResolver;
    using mir::SetParent;
    using mir::StableId;
    using mir::Transform;
    using mir::World;
    using mir::WorldVec3;

    const char *kPrefabUuid = "11111111-2222-4333-8444-555555555555";

    struct TempSceneDir
    {
        std::filesystem::path mPath;

        explicit TempSceneDir(std::string_view name)
            : mPath(std::filesystem::temp_directory_path() / "mir_scene_instance_tests" / name)
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

    const ComponentOps &LinkOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "target", FieldKind::EntityRef },
            { "other", FieldKind::EntityRef },
            { "speed", FieldKind::Float },
        };
        return ComponentRegistry::Instance().RegisterRuntime("ScenePrefabTestLink", fields);
    }

    const ComponentOps &RefOps()
    {
        static const RuntimeFieldDecl fields[] = { { "target", FieldKind::EntityRef } };
        return ComponentRegistry::Instance().RegisterRuntime("ScenePrefabTestRef", fields);
    }

    const ComponentOps &TagOps()
    {
        return ComponentRegistry::Instance().RegisterRuntime("ScenePrefabTestTag", {});
    }

    struct FakeSceneResolver final : ScenePrefabResolver
    {
        bool mAvailable = true;
        bool mExtraNode = false;
        StableId mUnavailableRoot = kNullStableId;

        std::map<SceneNodeAddress, Entity> Materialize(World &world, const ScenePrefabInstance &instance) override
        {
            if (!mAvailable || (mUnavailableRoot != kNullStableId && instance.mRoot == mUnavailableRoot))
                return {};

            const Entity root = world.CreateEntity();
            const Entity first = world.CreateEntity();
            const Entity second = world.CreateEntity();

            mir::AddTransform(world, root, Transform{ WorldVec3(1.0, 2.0, 3.0) });
            mir::AddTransform(world, first, Transform{ WorldVec3(0.0, 4.0, 0.0) }, root);
            mir::AddTransform(world, second, Transform{ WorldVec3(0.0, 0.0, 5.0) }, root);

            const ComponentOps &link = LinkOps();
            link.AddDefault(world, root);
            void *component = link.GetComponent(world, root);
            float speed = 1.5f;
            link.FindField("speed")->Write(component, &speed);
            link.FindField("target")->Write(component, &second);

            std::map<SceneNodeAddress, Entity> members;
            members[SceneNodeAddress{ 1 }] = root;
            members[SceneNodeAddress{ 2 }] = first;
            members[SceneNodeAddress{ 3 }] = second;
            if (mExtraNode)
                members[SceneNodeAddress{ 4 }] = world.CreateEntity();
            return members;
        }

        bool TemplateEntityRef(const ScenePrefabInstance &, const SceneNodeAddress &node,
            std::string_view component, std::string_view field,
            std::optional<SceneNodeAddress> &outTarget) const override
        {
            if (node != SceneNodeAddress{ 1 } || component != "ScenePrefabTestLink")
                return false;

            if (field == "target")
            {
                outTarget = SceneNodeAddress{ 3 };
                return true;
            }
            if (field == "other")
            {
                outTarget.reset();
                return true;
            }
            return false;
        }
    };

    StableId PlaceInstance(World &world, LoadedScene &scene, FakeSceneResolver &resolver)
    {
        ScenePrefabInstance seed;
        seed.mPrefab = kPrefabUuid;

        const std::map<SceneNodeAddress, Entity> members = resolver.Materialize(world, seed);
        if (members.empty())
            return kNullStableId;

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

        const StableId root = instance.mRoot;
        scene.mInstances[root] = std::move(instance);
        return root;
    }

    Entity EntityField(World &world, Entity entity, const ComponentOps &ops, std::string_view field)
    {
        void *component = ops.GetComponent(world, entity);
        REQUIRE(component != nullptr);
        Entity value;
        const mir::FieldDesc *desc = ops.FindField(field);
        REQUIRE(desc != nullptr);
        desc->Read(component, &value);
        return value;
    }

    void SetEntityField(World &world, Entity entity, const ComponentOps &ops, std::string_view field, Entity value)
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

    void WriteText(const std::filesystem::path &path, std::string_view text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << text;
    }
}

TEST_CASE("SaveScene writes one instance record and no member entity files", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    World world;
    LoadedScene scene = NewScene("test");
    const Entity ordinary = CreateSceneEntity(world, scene);
    mir::AddTransform(world, ordinary, Transform{ WorldVec3(9.0, 8.0, 7.0) });

    FakeSceneResolver resolver;
    const StableId root = PlaceInstance(world, scene, resolver);
    REQUIRE(root != kNullStableId);

    TempSceneDir dir("instance_record");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene, &resolver));

    const std::filesystem::path instancePath = dir.mPath / "instances" / (std::to_string(root) + ".json");
    REQUIRE(std::filesystem::exists(instancePath));

    const json instance = *mir::ReadJsonFile(instancePath);
    CHECK(instance.at("root") == root);
    CHECK(instance.at("prefab") == kPrefabUuid);
    CHECK(instance.at("bindings").size() == 3);

    const std::string text = ReadText(instancePath);
    CHECK(text.find("\"entity\"") == std::string::npos);
    CHECK(text.find("speed") == std::string::npos);
    CHECK(text.find("ScenePrefabTestLink") == std::string::npos);

    CHECK(std::filesystem::exists(dir.mPath / "entities" / "1.json"));
    for (const auto &[address, id] : scene.mInstances.at(root).mBindings)
        CHECK_FALSE(std::filesystem::exists(dir.mPath / "entities" / (std::to_string(id) + ".json")));
}

TEST_CASE("A scene without instances keeps its manifest unchanged", "[scene][prefab]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    const Entity entity = CreateSceneEntity(world, scene);
    mir::AddTransform(world, entity, Transform{ WorldVec3(1.0, 2.0, 3.0) });

    TempSceneDir dir("no_instances");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    const std::string expected =
        "{\n"
        "  \"entities\": [\n"
        "    1\n"
        "  ],\n"
        "  \"name\": \"test\",\n"
        "  \"nextId\": 2\n"
        "}";
    CHECK(ReadText(dir.mPath / "scene.json") == expected);
    CHECK_FALSE(std::filesystem::exists(dir.mPath / "instances"));
}

TEST_CASE("LoadScene preserves member StableIds, placement and the next id", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    World world;
    LoadedScene scene = NewScene("test");
    const Entity ordinary = CreateSceneEntity(world, scene);
    mir::AddTransform(world, ordinary, Transform{ WorldVec3(9.0, 8.0, 7.0) });

    FakeSceneResolver resolver;
    const StableId root = PlaceInstance(world, scene, resolver);
    REQUIRE(root != kNullStableId);

    const Entity rootEntity = scene.mEntities.at(root);
    REQUIRE(SetParent(world, rootEntity, ordinary, kNullEntity, false));
    const WorldVec3 rootPosition = world.GetComponent<Transform>(rootEntity)->Position();
    const StableId nextId = scene.mNextId;
    const std::map<SceneNodeAddress, StableId> bindings = scene.mInstances.at(root).mBindings;

    TempSceneDir dir("instance_reload");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene, &resolver));

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, &resolver);

    CHECK(loaded.mNextId == nextId);
    CHECK_FALSE(loaded.mDirty);
    REQUIRE(loaded.mEntities.size() == bindings.size() + 1);
    for (const auto &[address, id] : bindings)
        CHECK(loaded.mEntities.contains(id));

    const Entity loadedRoot = loaded.mEntities.at(root);
    CHECK(mir::ParentOf(loadedWorld, loadedRoot) == loaded.mEntities.at(1));
    REQUIRE(loadedWorld.GetComponent<Transform>(loadedRoot) != nullptr);
    CHECK(loadedWorld.GetComponent<Transform>(loadedRoot)->Position() == rootPosition);
}

TEST_CASE("LoadScene keeps a far instance placement exact", "[scene][prefab]")
{
    if (!mir::kLargeWorld)
        SKIP("exact far positions need a double-precision world");

    mir::RegisterCoreComponents();
    LinkOps();

    World world;
    LoadedScene scene = NewScene("test");
    FakeSceneResolver resolver;
    const StableId root = PlaceInstance(world, scene, resolver);
    REQUIRE(root != kNullStableId);

    const WorldVec3 far(1000000000.5, -2000000000.25, 3000000000.125);
    REQUIRE(world.GetComponent<Transform>(scene.mEntities.at(root))->SetPosition(far));

    TempSceneDir dir("far_instance");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene, &resolver));

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, &resolver);
    const Transform *transform = loadedWorld.GetComponent<Transform>(loaded.mEntities.at(root));
    REQUIRE(transform != nullptr);
    CHECK(transform->Position() == far);
}

TEST_CASE("LoadScene counts StableIds that only appear in bindings", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    TempSceneDir dir("binding_high_water");
    WriteText(dir.mPath / "scene.json", R"({"name":"test","nextId":1,"entities":[],"instances":[1]})");
    WriteText(dir.mPath / "instances" / "1.json", R"({
        "root": 1,
        "prefab": "11111111-2222-4333-8444-555555555555",
        "parent": 0,
        "siblingIndex": 0,
        "placement": {},
        "bindings": [
            {"address": ["1"], "id": 1},
            {"address": ["2"], "id": 2},
            {"address": ["3"], "id": 3},
            {"address": ["4"], "id": 50}
        ]
    })");

    World world;
    FakeSceneResolver resolver;
    LoadedScene loaded = mir::LoadScene(world, dir.mPath, &resolver);

    REQUIRE(loaded.mInstances.size() == 1);
    CHECK(loaded.mInstances.at(1).mBindings.at(SceneNodeAddress{ 4 }) == 50);
    CHECK(loaded.mNextId == 51);
    CHECK(mir::AllocateStableId(loaded) == 51);
}

TEST_CASE("LoadScene gives an unbound template node a fresh StableId", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    TempSceneDir dir("unbound_node");
    WriteText(dir.mPath / "scene.json", R"({"name":"test","nextId":1,"entities":[],"instances":[1]})");
    WriteText(dir.mPath / "instances" / "1.json", R"({
        "root": 1,
        "prefab": "11111111-2222-4333-8444-555555555555",
        "parent": 0,
        "siblingIndex": 0,
        "placement": {},
        "bindings": [
            {"address": ["1"], "id": 1},
            {"address": ["2"], "id": 2},
            {"address": ["3"], "id": 3}
        ]
    })");

    World world;
    FakeSceneResolver resolver;
    resolver.mExtraNode = true;
    const LoadedScene loaded = mir::LoadScene(world, dir.mPath, &resolver);

    REQUIRE(loaded.mInstances.size() == 1);
    const ScenePrefabInstance &instance = loaded.mInstances.at(1);
    REQUIRE(instance.mBindings.size() == 4);
    const StableId fresh = instance.mBindings.at(SceneNodeAddress{ 4 });
    CHECK(fresh == 4);
    CHECK(loaded.mEntities.contains(fresh));
    CHECK(loaded.mNextId == 5);
    CHECK(loaded.mDirty);
}

TEST_CASE("LoadScene without a resolver keeps the instance record unchanged", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    World world;
    LoadedScene scene = NewScene("test");
    FakeSceneResolver resolver;
    const StableId root = PlaceInstance(world, scene, resolver);
    REQUIRE(root != kNullStableId);
    const std::map<SceneNodeAddress, StableId> bindings = scene.mInstances.at(root).mBindings;

    TempSceneDir dir("no_resolver");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene, &resolver));

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);

    CHECK(loaded.mEntities.empty());
    REQUIRE(loaded.mInstances.size() == 1);
    const ScenePrefabInstance &instance = loaded.mInstances.at(root);
    CHECK(instance.mPrefab == kPrefabUuid);
    CHECK(instance.mBindings == bindings);
    CHECK_FALSE(instance.mPlacement.empty());
    CHECK_FALSE(loaded.mDirty);
}

TEST_CASE("LoadScene keeps a record whose prefab is unavailable", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    World world;
    LoadedScene scene = NewScene("test");
    FakeSceneResolver resolver;
    const StableId root = PlaceInstance(world, scene, resolver);
    REQUIRE(root != kNullStableId);

    TempSceneDir dir("unavailable_prefab");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene, &resolver));

    FakeSceneResolver unavailable;
    unavailable.mAvailable = false;

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, &unavailable);

    CHECK(loaded.mEntities.empty());
    REQUIRE(loaded.mInstances.size() == 1);
    CHECK(loaded.mInstances.at(root).mBindings.size() == 3);
    CHECK_FALSE(loaded.mDirty);
}

TEST_CASE("LoadScene skips a malformed instance file without throwing", "[scene][prefab]")
{
    mir::RegisterCoreComponents();

    TempSceneDir dir("malformed_instance");
    WriteText(dir.mPath / "scene.json", R"({"name":"test","nextId":3,"entities":[2],"instances":[1]})");
    WriteText(dir.mPath / "instances" / "1.json", "{ not json");
    WriteText(dir.mPath / "entities" / "2.json",
        R"({"id":2,"parent":0,"components":[{"type":"Transform","fields":{"position":[1.0,2.0,3.0]}}]})");

    World world;
    FakeSceneResolver resolver;
    const LoadedScene loaded = mir::LoadScene(world, dir.mPath, &resolver);

    CHECK(loaded.mInstances.empty());
    REQUIRE(loaded.mEntities.size() == 1);
    const Transform *transform = world.GetComponent<Transform>(loaded.mEntities.at(2));
    REQUIRE(transform != nullptr);
    CHECK(transform->Position() == WorldVec3(1.0, 2.0, 3.0));
}

TEST_CASE("An explicit null reference override survives save and load", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    World world;
    LoadedScene scene = NewScene("test");
    FakeSceneResolver resolver;
    const StableId root = PlaceInstance(world, scene, resolver);
    REQUIRE(root != kNullStableId);

    TempSceneDir dir("explicit_null");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene, &resolver));

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, &resolver);
    const Entity loadedRoot = loaded.mEntities.at(root);
    CHECK(EntityField(loadedWorld, loadedRoot, LinkOps(), "target") ==
        loaded.mEntities.at(loaded.mInstances.at(root).mBindings.at(SceneNodeAddress{ 3 })));

    REQUIRE(mir::SetInstanceEntityRef(loadedWorld, loaded, root, SceneNodeAddress{ 1 }, "ScenePrefabTestLink",
        "target", kNullStableId));
    CHECK(EntityField(loadedWorld, loadedRoot, LinkOps(), "target").IsNull());
    CHECK(loaded.mDirty);

    loaded.mDirty = false;
    CHECK_FALSE(mir::SetInstanceEntityRef(loadedWorld, loaded, root, SceneNodeAddress{ 1 }, "ScenePrefabTestLink",
        "target", kNullStableId));
    CHECK_FALSE(loaded.mDirty);

    TempSceneDir reloadedDir("explicit_null_reloaded");
    REQUIRE(mir::SaveScene(loadedWorld, reloadedDir.mPath, loaded, &resolver));

    const json file = *mir::ReadJsonFile(reloadedDir.mPath / "instances" / (std::to_string(root) + ".json"));
    REQUIRE(file.at("patches").size() == 1);
    const json &fields = file.at("patches").at(0).at("fields");
    REQUIRE(fields.size() == 1);
    CHECK(fields.at(0).at("component") == "ScenePrefabTestLink");
    CHECK(fields.at(0).at("field") == "target");
    CHECK(fields.at(0).at("value") == kNullStableId);

    World secondWorld;
    const LoadedScene second = mir::LoadScene(secondWorld, reloadedDir.mPath, &resolver);
    CHECK(EntityField(secondWorld, second.mEntities.at(root), LinkOps(), "target").IsNull());
}

TEST_CASE("An equal reference override survives save and load", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    World world;
    LoadedScene scene = NewScene("test");
    FakeSceneResolver resolver;
    const StableId root = PlaceInstance(world, scene, resolver);
    REQUIRE(root != kNullStableId);

    const StableId target = scene.mInstances.at(root).mBindings.at(SceneNodeAddress{ 3 });
    REQUIRE(mir::SetInstanceEntityRef(world, scene, root, SceneNodeAddress{ 1 }, "ScenePrefabTestLink", "target",
        target));

    TempSceneDir first("equal_reference_first");
    REQUIRE(mir::SaveScene(world, first.mPath, scene, &resolver));

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, first.mPath, &resolver);
    const ScenePrefabInstance &instance = loaded.mInstances.at(root);
    REQUIRE(instance.mPatches.size() == 1);
    REQUIRE(instance.mPatches.at(0).mFields.size() == 1);
    CHECK(instance.mPatches.at(0).mFields.at(0).mComponent == "ScenePrefabTestLink");
    CHECK(instance.mPatches.at(0).mFields.at(0).mField == "target");
    CHECK(instance.mPatches.at(0).mFields.at(0).mValue == target);
    CHECK(EntityField(loadedWorld, loaded.mEntities.at(root), LinkOps(), "target") == loaded.mEntities.at(target));

    TempSceneDir second("equal_reference_second");
    REQUIRE(mir::SaveScene(loadedWorld, second.mPath, loaded, &resolver));
    CHECK(SnapshotFiles(first.mPath) == SnapshotFiles(second.mPath));
}

TEST_CASE("EntityRefs resolve only after ordinary entities and members exist", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();
    RefOps();

    World world;
    LoadedScene scene = NewScene("test");
    const Entity ordinary = CreateSceneEntity(world, scene);
    mir::AddTransform(world, ordinary, Transform{});
    RefOps().AddDefault(world, ordinary);

    FakeSceneResolver resolver;
    const StableId rootA = PlaceInstance(world, scene, resolver);
    const StableId rootB = PlaceInstance(world, scene, resolver);
    REQUIRE(rootA != kNullStableId);
    REQUIRE(rootB != kNullStableId);

    const Entity memberB = scene.mEntities.at(scene.mInstances.at(rootB).mBindings.at(SceneNodeAddress{ 2 }));
    SetEntityField(world, ordinary, RefOps(), "target", memberB);

    SceneNodeAddress rootAddress{ 1 };
    SetEntityField(world, scene.mEntities.at(rootA), LinkOps(), "target", ordinary);
    mir::SceneInstancePatch patch;
    patch.mTarget = rootAddress;
    patch.mFields.push_back(mir::SceneFieldPatch{ "ScenePrefabTestLink", "target", StableId{ 1 } });
    scene.mInstances.at(rootA).mPatches.push_back(std::move(patch));

    TempSceneDir dir("cross_references");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene, &resolver));

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, &resolver);

    const Entity loadedOrdinary = loaded.mEntities.at(1);
    const Entity loadedRootA = loaded.mEntities.at(rootA);
    const Entity loadedRootB = loaded.mEntities.at(rootB);

    CHECK(EntityField(loadedWorld, loadedOrdinary, RefOps(), "target") ==
        loaded.mEntities.at(loaded.mInstances.at(rootB).mBindings.at(SceneNodeAddress{ 2 })));
    CHECK(EntityField(loadedWorld, loadedRootA, LinkOps(), "target") == loadedOrdinary);
    CHECK(EntityField(loadedWorld, loadedRootB, LinkOps(), "target") ==
        loaded.mEntities.at(loaded.mInstances.at(rootB).mBindings.at(SceneNodeAddress{ 3 })));
    CHECK(EntityField(loadedWorld, loadedRootB, LinkOps(), "target") !=
        loaded.mEntities.at(loaded.mInstances.at(rootA).mBindings.at(SceneNodeAddress{ 3 })));
}

TEST_CASE("DestroySceneEntity drops an instance record and marks the scene dirty", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    World world;
    LoadedScene scene = NewScene("test");
    const Entity keeper = CreateSceneEntity(world, scene);

    FakeSceneResolver resolver;
    const StableId root = PlaceInstance(world, scene, resolver);
    REQUIRE(root != kNullStableId);
    REQUIRE(scene.mInstances.size() == 1);

    REQUIRE(mir::DestroySceneEntity(world, scene, keeper));
    CHECK(scene.mInstances.size() == 1);
    CHECK_FALSE(scene.mDirty);

    REQUIRE(mir::DestroySceneEntity(world, scene, scene.mEntities.at(root)));
    CHECK(scene.mInstances.empty());
    CHECK(scene.mDirty);
}

TEST_CASE("LoadScene applies added and removed component patches", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();
    TagOps();

    World world;
    LoadedScene scene = NewScene("test");
    FakeSceneResolver resolver;
    const StableId root = PlaceInstance(world, scene, resolver);
    REQUIRE(root != kNullStableId);

    mir::SceneInstancePatch patch;
    patch.mTarget = SceneNodeAddress{ 1 };
    patch.mAddedComponents.push_back(mir::SceneComponentRecord{ "ScenePrefabTestTag", json::object() });
    patch.mRemovedComponents.push_back("ScenePrefabTestLink");
    scene.mInstances.at(root).mPatches.push_back(std::move(patch));

    TempSceneDir dir("component_patches");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene, &resolver));

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, &resolver);
    const Entity loadedRoot = loaded.mEntities.at(root);

    CHECK(TagOps().Has(loadedWorld, loadedRoot));
    CHECK_FALSE(LinkOps().Has(loadedWorld, loadedRoot));
}

TEST_CASE("An unchanged instance scene round-trips to identical files", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    World world;
    LoadedScene scene = NewScene("test");
    const Entity ordinary = CreateSceneEntity(world, scene);
    mir::AddTransform(world, ordinary, Transform{ WorldVec3(4.0, 5.0, 6.0) });

    FakeSceneResolver resolver;
    const StableId root = PlaceInstance(world, scene, resolver);
    REQUIRE(root != kNullStableId);

    TempSceneDir first("round_trip_first");
    REQUIRE(mir::SaveScene(world, first.mPath, scene, &resolver));

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, first.mPath, &resolver);

    TempSceneDir second("round_trip_second");
    REQUIRE(mir::SaveScene(loadedWorld, second.mPath, loaded, &resolver));

    CHECK(SnapshotFiles(first.mPath) == SnapshotFiles(second.mPath));
}

TEST_CASE("Unavailable members retain incoming reference identities across saves", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();
    RefOps();

    World world;
    LoadedScene scene = NewScene("test");
    const Entity ordinary = CreateSceneEntity(world, scene);
    RefOps().AddDefault(world, ordinary);
    FakeSceneResolver resolver;
    const StableId rootA = PlaceInstance(world, scene, resolver);
    const StableId rootB = PlaceInstance(world, scene, resolver);
    const StableId target = scene.mInstances.at(rootB).mBindings.at(SceneNodeAddress{ 2 });
    REQUIRE(mir::SetInstanceEntityRef(world, scene, rootA, SceneNodeAddress{ 1 },
        "ScenePrefabTestLink", "target", target));
    REQUIRE(mir::SetInstanceEntityRef(world, scene, rootA, SceneNodeAddress{ 1 },
        "ScenePrefabTestLink", "other", target));
    SetEntityField(world, ordinary, RefOps(), "target", scene.mEntities.at(target));

    TempSceneDir first("missing_reference_first");
    REQUIRE(mir::SaveScene(world, first.mPath, scene, &resolver));
    FakeSceneResolver unavailable;
    unavailable.mUnavailableRoot = rootB;
    World partialWorld;
    LoadedScene partial = mir::LoadScene(partialWorld, first.mPath, &unavailable);
    REQUIRE(partial.mEntities.contains(rootA));
    CHECK_FALSE(partial.mEntities.contains(target));
    CHECK(EntityField(partialWorld, partial.mEntities.at(rootA), LinkOps(), "target").IsNull());
    CHECK(EntityField(partialWorld, partial.mEntities.at(rootA), LinkOps(), "other").IsNull());
    CHECK(EntityField(partialWorld, partial.mEntities.at(1), RefOps(), "target").IsNull());
    CHECK(partial.mUnresolvedEntityRefs.size() == 3);

    StableId expectedTarget = target;
    SECTION("An unchanged null handle keeps its saved target")
    {
        CHECK_FALSE(partial.mDirty);
    }
    SECTION("An explicit clear replaces an unresolved reference")
    {
        REQUIRE(mir::SetInstanceEntityRef(partialWorld, partial, rootA, SceneNodeAddress{ 1 },
            "ScenePrefabTestLink", "target", kNullStableId));
        REQUIRE(mir::SetInstanceEntityRef(partialWorld, partial, rootA, SceneNodeAddress{ 1 },
            "ScenePrefabTestLink", "other", kNullStableId));
        CHECK(partial.mUnresolvedEntityRefs.size() == 1);
        expectedTarget = kNullStableId;
    }
    SECTION("Retargeting to a live entity replaces the unresolved identity")
    {
        REQUIRE(mir::SetInstanceEntityRef(partialWorld, partial, rootA, SceneNodeAddress{ 1 },
            "ScenePrefabTestLink", "target", StableId{ 1 }));
        REQUIRE(mir::SetInstanceEntityRef(partialWorld, partial, rootA, SceneNodeAddress{ 1 },
            "ScenePrefabTestLink", "other", StableId{ 1 }));
        CHECK(partial.mUnresolvedEntityRefs.size() == 1);
        expectedTarget = 1;
    }

    TempSceneDir second("missing_reference_second");
    REQUIRE(mir::SaveScene(partialWorld, second.mPath, partial, &unavailable));
    // Saving without a resolver must also preserve ordinary incoming references.
    World stillPartialWorld;
    const LoadedScene stillPartial = mir::LoadScene(stillPartialWorld, second.mPath, &unavailable);
    TempSceneDir third("missing_reference_third");
    REQUIRE(mir::SaveScene(stillPartialWorld, third.mPath, stillPartial));

    World restoredWorld;
    const LoadedScene restored = mir::LoadScene(restoredWorld, third.mPath, &resolver);
    REQUIRE(restored.mEntities.contains(target));
    const Entity expected = expectedTarget == kNullStableId ? kNullEntity : restored.mEntities.at(expectedTarget);
    CHECK(EntityField(restoredWorld, restored.mEntities.at(rootA), LinkOps(), "target") == expected);
    CHECK(EntityField(restoredWorld, restored.mEntities.at(rootA), LinkOps(), "other") == expected);
    CHECK(EntityField(restoredWorld, restored.mEntities.at(1), RefOps(), "target") == restored.mEntities.at(target));
    CHECK(restored.mUnresolvedEntityRefs.empty());
}

TEST_CASE("Field edits override the initial values of instance-added components", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();
    RefOps();

    World world;
    LoadedScene scene = NewScene("test");
    CreateSceneEntity(world, scene);
    CreateSceneEntity(world, scene);
    FakeSceneResolver resolver;
    const StableId root = PlaceInstance(world, scene, resolver);
    mir::SceneInstancePatch patch;
    patch.mTarget = SceneNodeAddress{ 1 };
    patch.mAddedComponents.push_back(mir::SceneComponentRecord{
        "ScenePrefabTestRef", json{ { "target", StableId{ 1 } } } });
    scene.mInstances.at(root).mPatches.push_back(std::move(patch));

    TempSceneDir first("added_reference_first");
    REQUIRE(mir::SaveScene(world, first.mPath, scene, &resolver));
    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, first.mPath, &resolver);
    CHECK(EntityField(loadedWorld, loaded.mEntities.at(root), RefOps(), "target") == loaded.mEntities.at(1));
    REQUIRE(mir::SetInstanceEntityRef(loadedWorld, loaded, root, SceneNodeAddress{ 1 },
        "ScenePrefabTestRef", "target", StableId{ 2 }));

    TempSceneDir second("added_reference_second");
    REQUIRE(mir::SaveScene(loadedWorld, second.mPath, loaded, &resolver));
    World reloadedWorld;
    const LoadedScene reloaded = mir::LoadScene(reloadedWorld, second.mPath, &resolver);
    CHECK(EntityField(reloadedWorld, reloaded.mEntities.at(root), RefOps(), "target") == reloaded.mEntities.at(2));
}

TEST_CASE("External children retain their positions among prefab members", "[scene][prefab][order]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    World world;
    LoadedScene scene = NewScene("test");
    FakeSceneResolver resolver;
    const StableId root = PlaceInstance(world, scene, resolver);
    const auto bindings = scene.mInstances.at(root).mBindings;
    const Entity rootEntity = scene.mEntities.at(root);
    const Entity first = scene.mEntities.at(bindings.at(SceneNodeAddress{ 2 }));
    const Entity second = scene.mEntities.at(bindings.at(SceneNodeAddress{ 3 }));
    const StableId ordinaryId = scene.mNextId;
    const Entity ordinary = CreateSceneEntity(world, scene);
    REQUIRE(SetParent(world, ordinary, rootEntity, first, false));
    const StableId nestedRoot = PlaceInstance(world, scene, resolver);
    REQUIRE(SetParent(world, scene.mEntities.at(nestedRoot), rootEntity, second, false));

    TempSceneDir dir("mixed_member_order");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene, &resolver));
    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, &resolver);
    std::vector<Entity> children;
    mir::ForEachChild(loadedWorld, loaded.mEntities.at(root), [&](Entity entity) { children.push_back(entity); });
    const std::vector<Entity> expected{
        loaded.mEntities.at(ordinaryId), loaded.mEntities.at(bindings.at(SceneNodeAddress{ 2 })),
        loaded.mEntities.at(nestedRoot), loaded.mEntities.at(bindings.at(SceneNodeAddress{ 3 })),
    };
    CHECK(children == expected);
}

TEST_CASE("Instance bindings take precedence over stale member entity files", "[scene][prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();

    World world;
    LoadedScene scene = NewScene("test");
    FakeSceneResolver resolver;
    const StableId root = PlaceInstance(world, scene, resolver);
    TempSceneDir dir("stale_member_files");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene, &resolver));
    for (const auto &[address, id] : scene.mInstances.at(root).mBindings)
    {
        REQUIRE(mir::WriteJsonFile(dir.mPath / "entities" / (std::to_string(id) + ".json"),
            json{ { "id", id }, { "parent", 0 }, { "components", json::array() } }));
    }

    SECTION("An available instance materializes once")
    {
        World loadedWorld;
        const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, &resolver);
        REQUIRE(loaded.mEntities.size() == 3);
        const Entity loadedRoot = loaded.mEntities.at(root);
        CHECK(LinkOps().Has(loadedWorld, loadedRoot));
        for (const SceneNodeAddress &address : { SceneNodeAddress{ 2 }, SceneNodeAddress{ 3 } })
        {
            const StableId member = scene.mInstances.at(root).mBindings.at(address);
            CHECK(mir::ParentOf(loadedWorld, loaded.mEntities.at(member)) == loadedRoot);
        }
    }
    SECTION("An unavailable instance does not load flattened stand-ins")
    {
        World loadedWorld;
        const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);
        CHECK(loaded.mEntities.empty());
        CHECK(loaded.mInstances.size() == 1);
    }
}

/**
 * @file PrefabCapture.tests.cpp
 * @author Jackey Cen
 * @brief Tests for prefab/PrefabCapture.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/CompiledPrefab.h>
#include <prefab/PrefabCapture.h>
#include <prefab/PrefabFormat.h>
#include <prefab/PrefabSceneInstance.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/Transform.h>
#include <core/ecs/components/WorldTransform.h>
#include <scene/SceneAsset.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using nlohmann::json;
    using mir::CapturePrefab;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::Entity;
    using mir::FieldKind;
    using mir::GeneratePrefabId;
    using mir::HasPrefabErrors;
    using mir::kNullStableId;
    using mir::LoadedScene;
    using mir::NewPrefabDocument;
    using mir::NewScene;
    using mir::PrefabCaptureResult;
    using mir::PrefabComponentInstance;
    using mir::PrefabDocument;
    using mir::PrefabNode;
    using mir::PrefabRegistry;
    using mir::RuntimeFieldDecl;
    using mir::SceneNodeAddress;
    using mir::ScenePrefabInstance;
    using mir::StableId;
    using mir::ToString;
    using mir::Transform;
    using mir::UniquePrefabPath;
    using mir::ValidatePrefab;
    using mir::World;

    const ComponentOps &RefOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "target", FieldKind::EntityRef },
            { "spare", FieldKind::EntityRef },
        };
        return ComponentRegistry::Instance().RegisterRuntime("PrefabCaptureTestRef", fields);
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
                ("mir_prefab_capture_test_" + ProcessToken() + "_" + std::to_string(counter.fetch_add(1)));
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

    void SetRef(World &world, Entity entity, std::string_view field, Entity target)
    {
        const ComponentOps &ops = RefOps();
        if (!ops.Has(world, entity))
            ops.AddDefault(world, entity);
        ops.FindField(field)->Write(ops.GetComponent(world, entity), &target);
    }

    void Name(World &world, Entity entity, const char *name)
    {
        world.AddComponent<mir::Name>(entity, mir::MakeName(name));
    }

    const PrefabComponentInstance *FindComponent(const PrefabNode &node, std::string_view type)
    {
        for (const PrefabComponentInstance &component : node.mComponents)
        {
            if (component.mType == type)
                return &component;
        }
        return nullptr;
    }

    StableId IdOf(const LoadedScene &scene, Entity entity)
    {
        for (const auto &[id, candidate] : scene.mEntities)
        {
            if (candidate == entity)
                return id;
        }
        return kNullStableId;
    }

    struct Fixture
    {
        World mWorld;
        LoadedScene mScene = NewScene("test");
        Entity mRoot = mir::kNullEntity;
        Entity mChild = mir::kNullEntity;
        Entity mGrandchild = mir::kNullEntity;
        Entity mOutside = mir::kNullEntity;

        Fixture()
        {
            mir::RegisterCoreComponents();
            RefOps();

            mRoot = mir::CreateSceneEntity(mWorld, mScene);
            mChild = mir::CreateSceneEntity(mWorld, mScene, mRoot);
            mGrandchild = mir::CreateSceneEntity(mWorld, mScene, mChild);
            mOutside = mir::CreateSceneEntity(mWorld, mScene);

            Name(mWorld, mRoot, "Root");
            Name(mWorld, mChild, "Child");
            Name(mWorld, mGrandchild, "Grandchild");
            Name(mWorld, mOutside, "Outside");

            Transform root(glm::vec3(1.0f, 2.0f, 3.0f),
                glm::angleAxis(0.4f, glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(2.0f, 3.0f, 4.0f));
            mir::AddTransform(mWorld, mRoot, root);
            mir::AddTransform(mWorld, mChild, Transform(glm::vec3(4.0f, 5.0f, 6.0f)));
            mir::AddTransform(mWorld, mGrandchild, Transform(glm::vec3(7.0f, 8.0f, 9.0f)));

            SetRef(mWorld, mChild, "target", mGrandchild);
            SetRef(mWorld, mGrandchild, "target", mir::kNullEntity);
        }
    };
}

TEST_CASE("Capture keeps the hierarchy and internal references", "[prefab][capture]")
{
    Fixture fixture;

    const PrefabCaptureResult captured = CapturePrefab(fixture.mWorld, fixture.mScene, fixture.mRoot);
    REQUIRE(captured.mDocument.has_value());
    CHECK(captured.mRefusal.empty());

    const PrefabDocument &document = *captured.mDocument;
    CHECK_FALSE(document.mId.IsNull());
    REQUIRE(document.mNodes.size() == 3);
    CHECK(document.mRootNode == 1);
    CHECK(document.mNodes[0].mId == 1);
    CHECK(document.mNodes[1].mId == 2);
    CHECK(document.mNodes[2].mId == 3);
    CHECK(document.mNodes[1].mParent == 1);
    CHECK(document.mNodes[2].mParent == 2);

    const PrefabComponentInstance *childRef = FindComponent(document.mNodes[1], "PrefabCaptureTestRef");
    REQUIRE(childRef != nullptr);
    CHECK(childRef->mFields.at("target") == json::array({ "3" }));

    const PrefabComponentInstance *grandchildRef = FindComponent(document.mNodes[2], "PrefabCaptureTestRef");
    REQUIRE(grandchildRef != nullptr);
    CHECK(grandchildRef->mFields.at("target").is_null());
    CHECK(grandchildRef->mFields.at("spare").is_null());

    const PrefabComponentInstance *rootTransform = FindComponent(document.mNodes[0], "Transform");
    REQUIRE(rootTransform != nullptr);
    CHECK(rootTransform->mFields.at("position") == json::array({ 0.0, 0.0, 0.0 }));
    CHECK(rootTransform->mFields.at("rotation") == json::array({ 0.0, 0.0, 0.0, 1.0 }));
    CHECK(rootTransform->mFields.at("scale") == json::array({ 2.0, 3.0, 4.0 }));

    const PrefabComponentInstance *childTransform = FindComponent(document.mNodes[1], "Transform");
    REQUIRE(childTransform != nullptr);
    CHECK(childTransform->mFields.at("position") == json::array({ 4.0, 5.0, 6.0 }));

    REQUIRE(fixture.mWorld.Has<mir::WorldTransform>(fixture.mRoot));
    const PrefabComponentInstance *rootWorldTransform = FindComponent(document.mNodes[0], "WorldTransform");
    REQUIRE(rootWorldTransform != nullptr);
    CHECK(rootWorldTransform->mFields.empty());
    CHECK(FindComponent(document.mNodes[1], "WorldTransform") != nullptr);
    CHECK_FALSE(HasPrefabErrors(ValidatePrefab(document)));
    const mir::PrefabCompileResult compiled = mir::CompilePrefab(document);
    REQUIRE(compiled.mPrefab != nullptr);

    const StableId capturedRoot = mir::PlacePrefabInstance(fixture.mWorld, fixture.mScene, *compiled.mPrefab);
    REQUIRE(capturedRoot != kNullStableId);
    const ScenePrefabInstance &placed = fixture.mScene.mInstances.at(capturedRoot);
    std::size_t transforms = 0;
    for (const auto &[address, id] : placed.mBindings)
    {
        (void)address;
        const Entity member = fixture.mScene.mEntities.at(id);
        CHECK(fixture.mWorld.Has<Transform>(member) == fixture.mWorld.Has<mir::WorldTransform>(member));
        if (fixture.mWorld.Has<Transform>(member))
            ++transforms;
    }
    CHECK(transforms == 3);
}

TEST_CASE("Capture refuses a reference outside the subtree", "[prefab][capture]")
{
    Fixture fixture;
    SetRef(fixture.mWorld, fixture.mRoot, "target", fixture.mOutside);
    const StableId nextId = fixture.mScene.mNextId;

    const PrefabCaptureResult captured = CapturePrefab(fixture.mWorld, fixture.mScene, fixture.mRoot);
    CHECK_FALSE(captured.mDocument.has_value());
    CHECK(captured.mRefusal.find("Root") != std::string::npos);
    CHECK(captured.mRefusal.find("PrefabCaptureTestRef.target") != std::string::npos);
    CHECK(fixture.mScene.mNextId == nextId);
    CHECK(fixture.mWorld.IsAlive(fixture.mOutside));
}

TEST_CASE("Capture refuses a Prefab instance member", "[prefab][capture]")
{
    Fixture fixture;

    ScenePrefabInstance instance;
    instance.mPrefab = mir::ToString(mir::GeneratePrefabId());
    instance.mRoot = IdOf(fixture.mScene, fixture.mRoot);
    instance.mBindings[mir::SceneNodeAddress{ 1 }] = instance.mRoot;
    instance.mBindings[mir::SceneNodeAddress{ 2 }] = IdOf(fixture.mScene, fixture.mChild);
    fixture.mScene.mInstances[instance.mRoot] = instance;

    const PrefabCaptureResult captured = CapturePrefab(fixture.mWorld, fixture.mScene, fixture.mRoot);
    CHECK_FALSE(captured.mDocument.has_value());
    CHECK(captured.mRefusal.find("Root") != std::string::npos);
}

TEST_CASE("An intended file name is sanitized and made unique", "[prefab][capture]")
{
    TempDir directory;
    mir::PrefabRegistry registry;

    std::filesystem::create_directories(directory.mPath);
    std::ofstream(directory.mPath / "Chair.prefab") << "{}";

    const PrefabDocument second = NewPrefabDocument();
    REQUIRE(registry.SetDocument(second, directory.mPath / "Chair 1.prefab"));

    const std::filesystem::path path = UniquePrefabPath(directory.mPath, "Chair", registry);
    CHECK(path.filename() == "Chair 2.prefab");
    CHECK(mir::PrefabNameForFile("Desk/Side:Table?") == "Desk_Side_Table_");
}

TEST_CASE("Capture refuses unavailable references instead of persisting a null handle", "[prefab][capture]")
{
    Fixture fixture;
    Entity owner = fixture.mRoot;
    SECTION("The selected root owns the reference") {}
    SECTION("A descendant owns the reference") { owner = fixture.mGrandchild; }
    SetRef(fixture.mWorld, owner, "target", mir::kNullEntity);
    fixture.mScene.mUnresolvedEntityRefs[{ IdOf(fixture.mScene, owner), "PrefabCaptureTestRef", "target" }] = 9999;
    const auto markers = fixture.mScene.mUnresolvedEntityRefs;
    const auto entities = fixture.mScene.mEntities;
    const auto nextId = fixture.mScene.mNextId;
    const auto captured = CapturePrefab(fixture.mWorld, fixture.mScene, fixture.mRoot);
    CHECK_FALSE(captured.mDocument.has_value());
    CHECK(captured.mRefusal.find("unresolved") != std::string::npos);
    CHECK(fixture.mScene.mUnresolvedEntityRefs == markers);
    CHECK(fixture.mScene.mEntities == entities);
    CHECK(fixture.mScene.mNextId == nextId);
}

TEST_CASE("Capture distinguishes unavailable references from resolved and unrelated fields", "[prefab][capture]")
{
    Fixture fixture;
    SECTION("An outside owner has an unresolved reference")
    {
        fixture.mScene.mUnresolvedEntityRefs[{ IdOf(fixture.mScene, fixture.mOutside),
            "PrefabCaptureTestRef", "target" }] = 9999;
    }
    SECTION("A live internal handle takes precedence over its stale marker")
    {
        fixture.mScene.mUnresolvedEntityRefs[{ IdOf(fixture.mScene, fixture.mChild),
            "PrefabCaptureTestRef", "target" }] = 9999;
    }
    const auto captured = CapturePrefab(fixture.mWorld, fixture.mScene, fixture.mRoot);
    REQUIRE(captured.mDocument.has_value());
    CHECK(FindComponent(captured.mDocument->mNodes[1], "PrefabCaptureTestRef")->mFields.at("target") ==
        json::array({ "3" }));
}

/**
 * @file PrefabPreview.tests.cpp
 * @author Jackey Cen
 * @brief Tests for prefab/PrefabCoordinator.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/PrefabAuthoring.h>
#include <prefab/PrefabCoordinator.h>
#include <prefab/PrefabFormat.h>
#include <prefab/PrefabSceneInstance.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/Transform.h>
#include <scene/ComponentJson.h>
#include <scene/SceneAsset.h>
#include <scene/SceneIO.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using nlohmann::json;
    using mir::CompiledPrefab;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::Entity;
    using mir::FieldDesc;
    using mir::FieldKind;
    using mir::HasPrefabErrors;
    using mir::kNullEntity;
    using mir::kNullStableId;
    using mir::LoadedScene;
    using mir::NewScene;
    using mir::ParentOf;
    using mir::PlacePrefabInstance;
    using mir::PrefabAuthoredEntry;
    using mir::PrefabAuthoredKey;
    using mir::PrefabAuthoring;
    using mir::PrefabComponentInstance;
    using mir::PrefabComponentState;
    using mir::PrefabCoordinator;
    using mir::PrefabDocument;
    using mir::PrefabFieldState;
    using mir::PrefabNode;
    using mir::PrefabNodeId;
    using mir::PrefabSystem;
    using mir::RuntimeFieldDecl;
    using mir::SceneNodeAddress;
    using mir::StableId;
    using mir::SystemContext;
    using mir::World;

    const ComponentOps &DataOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "amount", FieldKind::Float },
            { "bonus", FieldKind::Float },
            { "target", FieldKind::EntityRef },
        };
        return ComponentRegistry::Instance().RegisterRuntime("PrefabPreviewTestData", fields);
    }

    PrefabDocument Document()
    {
        PrefabDocument document = mir::NewPrefabDocument();
        document.mNodes[0].mComponents.push_back(PrefabComponentInstance{
            "PrefabPreviewTestData",
            json{ { "amount", 2.0 }, { "bonus", 3.0 }, { "target", json::array({ "2" }) } } });
        document.mNodes[0].mComponents.push_back(PrefabComponentInstance{ "Transform", json::object() });

        PrefabNode second;
        second.mId = 2;
        second.mParent = 1;
        second.mComponents.push_back(PrefabComponentInstance{
            "PrefabPreviewTestData",
            json{ { "amount", 9.0 }, { "bonus", 4.0 }, { "target", nullptr } } });
        second.mComponents.push_back(PrefabComponentInstance{ "Transform", json::object() });

        PrefabNode third;
        third.mId = 3;
        third.mParent = 1;
        third.mComponents.push_back(PrefabComponentInstance{ "Transform", json::object() });

        PrefabNode fourth;
        fourth.mId = 4;
        fourth.mParent = 3;
        fourth.mComponents.push_back(PrefabComponentInstance{ "Transform", json::object() });

        document.mNodes.push_back(std::move(second));
        document.mNodes.push_back(std::move(third));
        document.mNodes.push_back(std::move(fourth));
        document.mNextNodeId = 5;
        return document;
    }

    float FloatField(World &world, Entity entity, const ComponentOps &ops, std::string_view field)
    {
        void *component = ops.GetComponent(world, entity);
        REQUIRE(component != nullptr);
        const FieldDesc *desc = ops.FindField(field);
        REQUIRE(desc != nullptr);
        float value = 0.0f;
        desc->Read(component, &value);
        return value;
    }

    struct Fixture
    {
        World mMainWorld;
        World mPreviewWorld;
        LoadedScene mMainScene = NewScene("main");
        LoadedScene mPreviewScene = NewScene("preview");
        PrefabCoordinator *mCoordinator = nullptr;
        std::unique_ptr<PrefabAuthoring> mAuthoring;
        mir::CommandBuffer mCommands;
        PrefabDocument mDocument;
        StableId mPreviewRoot = kNullStableId;

        Fixture()
        {
            mir::RegisterCoreComponents();
            DataOps();

            mMainWorld.EmplaceResource<mir::EngineModeState>();
            mCoordinator = &mMainWorld.EmplaceResource<PrefabCoordinator>();

            mDocument = Document();
            REQUIRE(mCoordinator->Registry().SetDocument(mDocument));
            REQUIRE_FALSE(HasPrefabErrors(
                mCoordinator->Cache().Compile(mDocument, mir::RegistryLookup(mCoordinator->Registry()))));

            const std::shared_ptr<const CompiledPrefab> prefab = mCoordinator->Cache().Find(mDocument.mId);
            REQUIRE(prefab != nullptr);
            mPreviewRoot = PlacePrefabInstance(mPreviewWorld, mPreviewScene, *prefab);
            REQUIRE(mPreviewRoot != kNullStableId);
            REQUIRE(mCoordinator->BindPreview(mDocument.mId, mPreviewWorld, mPreviewScene));
            mCoordinator->Bind(mMainScene);

            mAuthoring = std::make_unique<PrefabAuthoring>(mMainWorld, *mCoordinator);
            mAuthoring->SetActiveDocument(mDocument.mId);
        }

        Entity PreviewEntity(const SceneNodeAddress &address) const
        {
            return mPreviewScene.mEntities.at(mPreviewScene.mInstances.at(mPreviewRoot).mBindings.at(address));
        }

        void Commit()
        {
            PrefabSystem system;
            SystemContext context{ mMainWorld, mCommands };
            system.OnUpdate(context);
        }
    };
}

TEST_CASE("A committed preview edit updates its world and keeps surviving entities", "[prefab][preview]")
{
    Fixture fixture;
    const Entity second = fixture.PreviewEntity(SceneNodeAddress{ 2 });
    std::size_t mainTransforms = 0;
    fixture.mMainWorld.GetOrCreateQuery<const mir::Transform>().ForEach(
        [&mainTransforms](Entity, const mir::Transform &) { ++mainTransforms; });
    CHECK(mainTransforms == 0);

    PrefabAuthoredKey key;
    key.mPrefab = fixture.mDocument.mId;
    key.mAddress = { 2 };
    key.mComponent = "PrefabPreviewTestData";
    key.mField = "amount";
    const std::vector<PrefabAuthoredEntry> edit{ PrefabAuthoredEntry{ key, std::string("42.0") } };
    REQUIRE(fixture.mAuthoring->Write(edit).mApplied);

    fixture.Commit();

    CHECK(fixture.PreviewEntity(SceneNodeAddress{ 2 }) == second);
    CHECK(FloatField(fixture.mPreviewWorld, second, DataOps(), "amount") == 42.0f);
    CHECK(fixture.mMainScene.mEntities.empty());
    CHECK_FALSE(fixture.mCoordinator->ConsumeSceneMaintenance());
}

TEST_CASE("A preview node inserted in the document commits into the preview", "[prefab][preview]")
{
    Fixture fixture;
    const Entity second = fixture.PreviewEntity(SceneNodeAddress{ 2 });

    const mir::PrefabAuthoredAction created = fixture.mAuthoring->CreateNode(1, 0);
    REQUIRE(created.mRefusal.empty());
    REQUIRE(fixture.mAuthoring->Write(created.mEntries).mApplied);
    fixture.Commit();

    const PrefabDocument *document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    REQUIRE(document->mNodes.size() == 5);
    const PrefabNodeId inserted = document->mNodes[1].mId;
    CHECK(document->mNodes[1].mParent == 1);
    CHECK(fixture.PreviewEntity(SceneNodeAddress{ 2 }) == second);

    const Entity entity = fixture.PreviewEntity(SceneNodeAddress{ inserted });
    CHECK(ParentOf(fixture.mPreviewWorld, entity) == fixture.PreviewEntity(SceneNodeAddress{ 1 }));
    CHECK(fixture.mPreviewWorld.Has<mir::Transform>(entity));
    CHECK(fixture.mPreviewWorld.Has<mir::Name>(entity));
}

TEST_CASE("Preview entities return document keys and field states", "[prefab][preview]")
{
    Fixture fixture;
    const Entity second = fixture.PreviewEntity(SceneNodeAddress{ 2 });

    const std::optional<PrefabAuthoredKey> key =
        fixture.mAuthoring->FieldKey(second, "PrefabPreviewTestData", "amount");
    REQUIRE(key.has_value());
    REQUIRE(key->mPrefab.has_value());
    CHECK(*key->mPrefab == fixture.mDocument.mId);
    CHECK(key->mAddress == SceneNodeAddress{ 2 });
    CHECK(fixture.mAuthoring->FieldState(*key) == PrefabFieldState::Inherited);
    CHECK(fixture.mAuthoring->ComponentState(second, "PrefabPreviewTestData") == PrefabComponentState::Inherited);
    CHECK(fixture.mAuthoring->RemovedComponents(second).empty());
    CHECK(fixture.mAuthoring->WritesComponentDirectly(second, "PrefabPreviewTestData"));
    CHECK(fixture.mAuthoring->NodeIdOf(second) == 2);

    const Entity root = fixture.PreviewEntity(SceneNodeAddress{ 1 });
    CHECK(fixture.mAuthoring->FieldKey(root, "Transform", "position").has_value());

    const Entity third = fixture.PreviewEntity(SceneNodeAddress{ 3 });
    const std::string encoded = fixture.mAuthoring->EncodeField(FieldKind::EntityRef, &third);
    CHECK(json::parse(encoded) == json::array({ "3" }));
}

TEST_CASE("Preview node delete and reparent entries cover the document", "[prefab][preview]")
{
    Fixture fixture;

    const StableId thirdId = fixture.mPreviewScene.mInstances.at(fixture.mPreviewRoot).mBindings.at({ 3 });
    const StableId fourthId = fixture.mPreviewScene.mInstances.at(fixture.mPreviewRoot).mBindings.at({ 4 });
    const std::array<PrefabNodeId, 1> third{ 3 };
    const mir::PrefabAuthoredAction deleted = fixture.mAuthoring->DeleteNodes(third);
    REQUIRE(deleted.mRefusal.empty());
    REQUIRE(deleted.mEntries.size() == 2);
    CHECK(deleted.mEntries[0].mKey.mAddress == SceneNodeAddress{ 3 });
    CHECK(deleted.mEntries[1].mKey.mAddress == SceneNodeAddress{ 4 });
    REQUIRE(fixture.mAuthoring->Write(deleted.mEntries).mApplied);
    fixture.Commit();

    CHECK_FALSE(fixture.mPreviewScene.mEntities.contains(thirdId));
    CHECK_FALSE(fixture.mPreviewScene.mEntities.contains(fourthId));

    const mir::PrefabAuthoredAction reverted = fixture.mAuthoring->DeleteNodes(third);
    CHECK(reverted.mRefusal == "the node is missing");

    const std::array<PrefabNodeId, 1> root{ 1 };
    const mir::PrefabAuthoredAction rootDelete = fixture.mAuthoring->DeleteNodes(root);
    CHECK_FALSE(rootDelete.mRefusal.empty());

    Fixture reparent;
    const Entity node = reparent.PreviewEntity(SceneNodeAddress{ 4 });
    const Entity parent = reparent.PreviewEntity(SceneNodeAddress{ 2 });
    const mir::PrefabAuthoredAction moved = reparent.mAuthoring->ReparentNode(4, 2, 0);
    REQUIRE(moved.mRefusal.empty());
    REQUIRE(reparent.mAuthoring->Write(moved.mEntries).mApplied);
    reparent.Commit();
    CHECK(ParentOf(reparent.mPreviewWorld, node) == parent);

    const mir::PrefabAuthoredAction refused = reparent.mAuthoring->ReparentNode(2, 4, 0);
    CHECK_FALSE(refused.mRefusal.empty());
}

TEST_CASE("Reparenting a preview node keeps its world pose", "[prefab][preview]")
{
    Fixture fixture;

    const auto transformKey = [&](PrefabNodeId node, std::string_view field)
        {
            return PrefabAuthoredKey{ fixture.mDocument.mId, SceneNodeAddress{ node }, "Transform", std::string(field) };
        };
    const mir::WorldVec3 secondPosition(10.0, 0.0, 0.0);
    const glm::quat secondRotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 secondScale(2.0f);
    const mir::WorldVec3 thirdPosition(0.0, 5.0, 0.0);
    const mir::WorldVec3 fourthPosition(1.0, 0.0, 3.0);
    const std::vector<PrefabAuthoredEntry> placed{
        { transformKey(2, "position"), mir::FieldToJson(FieldKind::WorldPosition, &secondPosition).dump() },
        { transformKey(2, "rotation"), mir::FieldToJson(FieldKind::Quat, &secondRotation).dump() },
        { transformKey(2, "scale"), mir::FieldToJson(FieldKind::Vec3, &secondScale).dump() },
        { transformKey(3, "position"), mir::FieldToJson(FieldKind::WorldPosition, &thirdPosition).dump() },
        { transformKey(4, "position"), mir::FieldToJson(FieldKind::WorldPosition, &fourthPosition).dump() },
    };
    REQUIRE(fixture.mAuthoring->Write(placed).mApplied);
    fixture.Commit();

    const Entity node = fixture.PreviewEntity(SceneNodeAddress{ 4 });
    const mir::WorldPose before = mir::ResolveWorld(fixture.mPreviewWorld, node);

    const mir::PrefabAuthoredAction moved = fixture.mAuthoring->ReparentNode(4, 2, 0);
    REQUIRE(moved.mRefusal.empty());
    REQUIRE(fixture.mAuthoring->Write(moved.mEntries).mApplied);
    fixture.Commit();
    REQUIRE(ParentOf(fixture.mPreviewWorld, node) == fixture.PreviewEntity(SceneNodeAddress{ 2 }));

    const mir::WorldPose after = mir::ResolveWorld(fixture.mPreviewWorld, node);
    CHECK(glm::length(glm::dvec3(after.position.Meters() - before.position.Meters())) < 1e-4);
    for (int axis = 0; axis < 3; ++axis)
        CHECK(glm::length(after.linear[axis] - before.linear[axis]) < 1e-4f);
}

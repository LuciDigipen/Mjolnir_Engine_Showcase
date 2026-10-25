/**
 * @file PrefabAuthoring.tests.cpp
 * @author Jackey Cen
 * @brief Tests for prefab/PrefabAuthoring.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/PrefabAuthoring.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <prefab/PrefabCoordinator.h>
#include <prefab/PrefabFormat.h>
#include <prefab/PrefabSceneInstance.h>
#include <scene/SceneInstancePatch.h>
#include <scene/SceneIO.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using nlohmann::json;
    using mir::CompiledPrefab;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::CreateSceneEntity;
    using mir::Entity;
    using mir::FieldDesc;
    using mir::FieldKind;
    using mir::FindInstanceFieldPatch;
    using mir::HasPrefabErrors;
    using mir::kNullStableId;
    using mir::LoadedScene;
    using mir::NewScene;
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
    using mir::PrefabSystem;
    using mir::RuntimeFieldDecl;
    using mir::SceneNodeAddress;
    using mir::ScenePrefabInstance;
    using mir::SetInstanceEntityRef;
    using mir::SetInstanceFieldOverride;
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
        return ComponentRegistry::Instance().RegisterRuntime("PrefabAuthoringTestData", fields);
    }

    const ComponentOps &ExtraOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "value", FieldKind::Float },
            { "link", FieldKind::EntityRef },
        };
        return ComponentRegistry::Instance().RegisterRuntime("PrefabAuthoringTestExtra", fields);
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
                ("mir_prefab_authoring_test_" + ProcessToken() + "_" + std::to_string(counter.fetch_add(1)));
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

    PrefabDocument Document()
    {
        PrefabDocument document = mir::NewPrefabDocument();

        document.mNodes[0].mComponents.push_back(PrefabComponentInstance{
            "PrefabAuthoringTestData",
            json{ { "amount", 2.0 }, { "bonus", 3.0 }, { "target", json::array({ "2" }) } } });
        document.mNodes[0].mComponents.push_back(PrefabComponentInstance{ "Transform", json::object() });

        PrefabNode second;
        second.mId = 2;
        second.mParent = 1;
        second.mComponents.push_back(PrefabComponentInstance{
            "PrefabAuthoringTestData",
            json{ { "amount", 9.0 }, { "bonus", 4.0 }, { "target", nullptr } } });
        second.mComponents.push_back(PrefabComponentInstance{
            "PrefabAuthoringTestExtra", json{ { "value", 1.0 }, { "link", nullptr } } });
        second.mComponents.push_back(PrefabComponentInstance{ "Transform", json::object() });

        PrefabNode third;
        third.mId = 3;
        third.mParent = 1;
        third.mComponents.push_back(PrefabComponentInstance{
            "PrefabAuthoringTestData",
            json{ { "amount", 5.0 }, { "bonus", 6.0 }, { "target", nullptr } } });

        PrefabNode fourth;
        fourth.mId = 4;
        fourth.mParent = 3;
        fourth.mComponents.push_back(PrefabComponentInstance{
            "PrefabAuthoringTestData",
            json{ { "amount", 7.0 }, { "bonus", 8.0 }, { "target", nullptr } } });

        document.mNodes.push_back(std::move(second));
        document.mNodes.push_back(std::move(third));
        document.mNodes.push_back(std::move(fourth));
        document.mNextNodeId = 5;
        return document;
    }

    PrefabAuthoredKey SceneKey(StableId root, const SceneNodeAddress &node, std::string component,
        std::string field = {})
    {
        PrefabAuthoredKey key;
        key.mAddress.push_back(root);
        key.mAddress.insert(key.mAddress.end(), node.begin(), node.end());
        key.mComponent = std::move(component);
        key.mField = std::move(field);
        return key;
    }

    PrefabAuthoredKey DocumentKey(mir::PrefabId id, const SceneNodeAddress &node, std::string component = {},
        std::string field = {})
    {
        PrefabAuthoredKey key;
        key.mPrefab = id;
        key.mAddress = node;
        key.mComponent = std::move(component);
        key.mField = std::move(field);
        return key;
    }

    std::optional<std::string> ReadOne(PrefabAuthoring &authoring, const PrefabAuthoredKey &key)
    {
        const std::optional<std::vector<PrefabAuthoredEntry>> entries = authoring.Read({ &key, 1 });
        REQUIRE(entries.has_value());
        REQUIRE(entries->size() == 1);
        return entries->front().mValue;
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

    Entity EntityField(World &world, Entity entity, const ComponentOps &ops, std::string_view field)
    {
        void *component = ops.GetComponent(world, entity);
        REQUIRE(component != nullptr);
        const FieldDesc *desc = ops.FindField(field);
        REQUIRE(desc != nullptr);
        Entity value = mir::kNullEntity;
        desc->Read(component, &value);
        return value;
    }

    void SetEntityField(World &world, Entity entity, const ComponentOps &ops, std::string_view field, Entity value)
    {
        void *component = ops.GetComponent(world, entity);
        REQUIRE(component != nullptr);
        const FieldDesc *desc = ops.FindField(field);
        REQUIRE(desc != nullptr);
        REQUIRE(desc->Write(component, &value));
    }

    struct Fixture
    {
        World mWorld;
        LoadedScene mScene;
        PrefabCoordinator *mCoordinator = nullptr;
        std::unique_ptr<PrefabAuthoring> mAuthoring;
        PrefabDocument mDocument;
        mir::CommandBuffer mCommands;
        StableId mRoot = kNullStableId;

        Fixture()
        {
            mir::RegisterCoreComponents();
            DataOps();
            ExtraOps();

            mWorld.EmplaceResource<mir::EngineModeState>();
            mCoordinator = &mWorld.EmplaceResource<PrefabCoordinator>();

            mDocument = Document();
            REQUIRE(mCoordinator->Registry().SetDocument(mDocument));
            REQUIRE_FALSE(HasPrefabErrors(mCoordinator->Cache().Compile(mDocument)));

            mScene = NewScene("test");
            const std::shared_ptr<const CompiledPrefab> prefab = mCoordinator->Cache().Find(mDocument.mId);
            REQUIRE(prefab != nullptr);
            mRoot = PlacePrefabInstance(mWorld, mScene, *prefab);
            REQUIRE(mRoot != kNullStableId);
            mCoordinator->Bind(mScene);

            mAuthoring = std::make_unique<PrefabAuthoring>(mWorld, *mCoordinator);
        }

        StableId Binding(const SceneNodeAddress &address) const
        {
            return mScene.mInstances.at(mRoot).mBindings.at(address);
        }

        Entity Member(const SceneNodeAddress &address) const
        {
            return mScene.mEntities.at(Binding(address));
        }

        void Commit()
        {
            PrefabSystem system;
            SystemContext context{ mWorld, mCommands };
            system.OnUpdate(context);
        }
    };

    std::map<std::string, std::string> SnapshotFiles(const std::filesystem::path &root)
    {
        std::map<std::string, std::string> files;
        for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
                continue;
            std::ifstream file(entry.path(), std::ios::binary);
            files[entry.path().lexically_relative(root).generic_string()] =
                std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        return files;
    }
}

TEST_CASE("Document-only writes work before a scene is bound", "[prefab][authoring]")
{
    mir::RegisterCoreComponents();
    DataOps();
    ExtraOps();
    World world;
    PrefabCoordinator coordinator;
    PrefabDocument document = Document();
    REQUIRE(coordinator.SetDocument(document));
    REQUIRE(coordinator.Scene() == nullptr);
    PrefabAuthoring authoring(world, coordinator);

    const PrefabAuthoredKey amount =
        DocumentKey(document.mId, SceneNodeAddress{ 2 }, "PrefabAuthoringTestData", "amount");
    const std::vector<PrefabAuthoredEntry> changed{ { amount, std::string("12.0") } };
    const mir::PrefabAuthoredWrite write = authoring.Write(changed);
    REQUIRE(write.mApplied);
    CHECK_FALSE(write.mSceneMaintained);
    CHECK(ReadOne(authoring, amount) == "12.0");

    const PrefabAuthoredKey leaf = DocumentKey(document.mId, SceneNodeAddress{ 4 });
    const std::vector<PrefabAuthoredEntry> deletion{ { leaf, std::nullopt } };
    const mir::PrefabAuthoredWrite removed = authoring.Write(deletion);
    REQUIRE(removed.mApplied);
    CHECK_FALSE(removed.mSceneMaintained);
    CHECK(coordinator.Registry().Find(document.mId)->mNodes.size() == 3);

    const PrefabAuthoredKey sceneAmount =
        SceneKey(1, SceneNodeAddress{ 2 }, "PrefabAuthoringTestData", "amount");
    const std::vector<PrefabAuthoredEntry> mixed{
        { amount, std::string("99.0") }, { sceneAmount, std::string("25.0") } };
    CHECK_FALSE(authoring.Write(mixed).mApplied);
    CHECK(ReadOne(authoring, amount) == "12.0");
}

TEST_CASE("Field and component presence round-trips through the authored state", "[prefab][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress rootNode{ 1 };
    const SceneNodeAddress second{ 2 };

    const PrefabAuthoredKey amount = SceneKey(fixture.mRoot, rootNode, "PrefabAuthoringTestData", "amount");
    CHECK_FALSE(ReadOne(*fixture.mAuthoring, amount).has_value());

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mRoot, rootNode, "PrefabAuthoringTestData", "amount", 2.0));
    CHECK(ReadOne(*fixture.mAuthoring, amount) == "2.0");
    CHECK(fixture.mAuthoring->FieldState(amount) == PrefabFieldState::Overridden);

    const std::vector<PrefabAuthoredEntry> reverted{ PrefabAuthoredEntry{ amount, std::nullopt } };
    CHECK(fixture.mAuthoring->Write(reverted).mApplied);
    CHECK_FALSE(ReadOne(*fixture.mAuthoring, amount).has_value());

    const PrefabAuthoredKey data =
        SceneKey(fixture.mRoot, second, "PrefabAuthoringTestData");
    CHECK_FALSE(ReadOne(*fixture.mAuthoring, data).has_value());

    REQUIRE(mir::RemoveInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mRoot, second, "PrefabAuthoringTestData"));
    CHECK(ReadOne(*fixture.mAuthoring, data) == "null");
    CHECK(fixture.mAuthoring->ComponentState(fixture.Member(second), "PrefabAuthoringTestData") ==
        PrefabComponentState::Removed);
    CHECK(fixture.mAuthoring->RemovedComponents(fixture.Member(second)) ==
        std::vector<std::string>{ "PrefabAuthoringTestData" });

    const std::vector<PrefabAuthoredEntry> restored{ PrefabAuthoredEntry{ data, std::nullopt } };
    CHECK(fixture.mAuthoring->Write(restored).mApplied);
    CHECK_FALSE(ReadOne(*fixture.mAuthoring, data).has_value());

    const PrefabAuthoredKey extra = SceneKey(fixture.mRoot, rootNode, "PrefabAuthoringTestExtra");
    CHECK_FALSE(ReadOne(*fixture.mAuthoring, extra).has_value());
    const std::vector<PrefabAuthoredEntry> added{ PrefabAuthoredEntry{ extra, std::string("{\"value\":4.5}") } };
    CHECK(fixture.mAuthoring->Write(added).mApplied);
    CHECK(ReadOne(*fixture.mAuthoring, extra) == "{\"value\":4.5}");
    CHECK(fixture.mAuthoring->ComponentState(fixture.Member(rootNode), "PrefabAuthoringTestExtra") ==
        PrefabComponentState::Added);
}

TEST_CASE("Prefab document nodes and fields round-trip", "[prefab][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress rootNode{ 1 };

    const PrefabAuthoredKey node = DocumentKey(fixture.mDocument.mId, rootNode);
    const std::optional<std::string> record = ReadOne(*fixture.mAuthoring, node);
    REQUIRE(record.has_value());
    CHECK(record->find("\"components\"") != std::string::npos);

    const std::vector<PrefabAuthoredEntry> removeRoot{ PrefabAuthoredEntry{ node, std::nullopt } };
    CHECK_FALSE(fixture.mAuthoring->Write(removeRoot).mApplied);

    const PrefabAuthoredKey amount = DocumentKey(fixture.mDocument.mId, rootNode,
        "PrefabAuthoringTestData", "amount");
    CHECK(ReadOne(*fixture.mAuthoring, amount) == "2.0");

    const std::vector<PrefabAuthoredEntry> clearField{ PrefabAuthoredEntry{ amount, std::nullopt } };
    CHECK(fixture.mAuthoring->Write(clearField).mApplied);
    CHECK_FALSE(ReadOne(*fixture.mAuthoring, amount).has_value());

    const std::vector<PrefabAuthoredEntry> setField{
        PrefabAuthoredEntry{ amount, std::string("11.0") } };
    CHECK(fixture.mAuthoring->Write(setField).mApplied);
    CHECK(ReadOne(*fixture.mAuthoring, amount) == "11.0");
}

TEST_CASE("A refused batch changes nothing in either document", "[prefab][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress rootNode{ 1 };
    const PrefabAuthoredKey sceneAmount =
        SceneKey(fixture.mRoot, rootNode, "PrefabAuthoringTestData", "amount");
    const PrefabAuthoredKey badField =
        DocumentKey(fixture.mDocument.mId, SceneNodeAddress{ 3 }, "PrefabAuthoringTestExtra", "value");

    const std::vector<PrefabAuthoredEntry> entries{
        PrefabAuthoredEntry{ sceneAmount, std::string("5.0") },
        PrefabAuthoredEntry{ badField, std::string("1.0") },
    };
    CHECK_FALSE(fixture.mAuthoring->Write(entries).mApplied);
    CHECK_FALSE(ReadOne(*fixture.mAuthoring, sceneAmount).has_value());
    CHECK(ReadOne(*fixture.mAuthoring,
              DocumentKey(fixture.mDocument.mId, rootNode, "PrefabAuthoringTestData", "amount")) == "2.0");

    const PrefabAuthoredKey invalidValue =
        SceneKey(fixture.mRoot, rootNode, "PrefabAuthoringTestData", "bonus");
    const std::vector<PrefabAuthoredEntry> invalid{
        PrefabAuthoredEntry{ invalidValue, std::string("not a float") },
        PrefabAuthoredEntry{ DocumentKey(fixture.mDocument.mId, rootNode, "PrefabAuthoringTestData", "amount"),
            std::string("6.0") },
    };
    CHECK_FALSE(fixture.mAuthoring->Write(invalid).mApplied);
    CHECK_FALSE(ReadOne(*fixture.mAuthoring, invalidValue).has_value());
    CHECK(ReadOne(*fixture.mAuthoring,
              DocumentKey(fixture.mDocument.mId, rootNode, "PrefabAuthoringTestData", "amount")) == "2.0");
}

TEST_CASE("A batch whose own document edits invalidate a scene entry is refused", "[prefab][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress second{ 2 };
    const SceneNodeAddress third{ 3 };

    const PrefabAuthoredKey documentData =
        DocumentKey(fixture.mDocument.mId, second, "PrefabAuthoringTestData");
    const PrefabAuthoredKey sceneAmount =
        SceneKey(fixture.mRoot, second, "PrefabAuthoringTestData", "amount");
    const std::vector<PrefabAuthoredEntry> fieldOnRemovedComponent{
        PrefabAuthoredEntry{ documentData, std::nullopt },
        PrefabAuthoredEntry{ sceneAmount, std::string("11.0") },
    };
    CHECK_FALSE(fixture.mAuthoring->Write(fieldOnRemovedComponent).mApplied);
    CHECK(ReadOne(*fixture.mAuthoring, documentData).has_value());
    CHECK_FALSE(ReadOne(*fixture.mAuthoring, sceneAmount).has_value());

    const PrefabAuthoredKey documentExtra =
        DocumentKey(fixture.mDocument.mId, third, "PrefabAuthoringTestExtra");
    const PrefabAuthoredKey sceneExtra =
        SceneKey(fixture.mRoot, third, "PrefabAuthoringTestExtra");
    const std::vector<PrefabAuthoredEntry> addedTwice{
        PrefabAuthoredEntry{ documentExtra, std::string("{\"value\":2.0}") },
        PrefabAuthoredEntry{ sceneExtra, std::string("{\"value\":3.0}") },
    };
    CHECK_FALSE(fixture.mAuthoring->Write(addedTwice).mApplied);
    CHECK_FALSE(ReadOne(*fixture.mAuthoring, documentExtra).has_value());
    CHECK_FALSE(ReadOne(*fixture.mAuthoring, sceneExtra).has_value());
}

TEST_CASE("Apply writes the prefab document and clears only the selected scene patch", "[prefab][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress rootNode{ 1 };
    const SceneNodeAddress second{ 2 };
    const SceneNodeAddress third{ 3 };

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mRoot, rootNode, "PrefabAuthoringTestData", "amount", 12.0));
    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mRoot, rootNode, "PrefabAuthoringTestData", "bonus", 13.0));
    REQUIRE(SetInstanceEntityRef(fixture.mWorld, fixture.mScene, fixture.mRoot, rootNode,
        "PrefabAuthoringTestData", "target", fixture.Binding(third)));

    const PrefabAuthoredKey amount = SceneKey(fixture.mRoot, rootNode, "PrefabAuthoringTestData", "amount");
    const PrefabAuthoredKey target = SceneKey(fixture.mRoot, rootNode, "PrefabAuthoringTestData", "target");

    TempDir prefabs;
    REQUIRE(mir::WritePrefabFile(prefabs.mPath / "instance.prefab", fixture.mDocument));
    CHECK(fixture.mCoordinator->Registry().Scan(prefabs.mPath).empty());
    const std::map<std::string, std::string> filesBefore = SnapshotFiles(prefabs.mPath);

    const mir::PrefabAuthoredAction applied = fixture.mAuthoring->Apply(amount);
    REQUIRE(applied.mRefusal.empty());
    REQUIRE(applied.mEntries.size() == 2);
    CHECK(fixture.mAuthoring->Write(applied.mEntries).mApplied);

    const PrefabDocument *document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    const PrefabNode &node = document->mNodes.front();
    REQUIRE(node.mComponents.size() >= 1);
    CHECK(node.mComponents.front().mFields.at("amount") == 12.0);
    CHECK(node.mComponents.front().mFields.at("bonus") == 3.0);
    CHECK_FALSE(ReadOne(*fixture.mAuthoring, amount).has_value());
    CHECK(ReadOne(*fixture.mAuthoring,
              SceneKey(fixture.mRoot, rootNode, "PrefabAuthoringTestData", "bonus")).has_value());
    CHECK(SnapshotFiles(prefabs.mPath) == filesBefore);

    const mir::PrefabAuthoredAction translated = fixture.mAuthoring->Apply(target);
    REQUIRE(translated.mRefusal.empty());
    CHECK(fixture.mAuthoring->Write(translated.mEntries).mApplied);
    document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    CHECK(document->mNodes.front().mComponents.front().mFields.at("target") == json::array({ "3" }));

    const Entity ordinary = CreateSceneEntity(fixture.mWorld, fixture.mScene);
    DataOps().AddDefault(fixture.mWorld, ordinary);
    const StableId ordinaryId = fixture.mScene.mNextId - 1;
    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mRoot, second, "PrefabAuthoringTestData", "target", ordinaryId));

    const mir::PrefabAuthoredAction refused = fixture.mAuthoring->Apply(
        SceneKey(fixture.mRoot, second, "PrefabAuthoringTestData", "target"));
    CHECK_FALSE(refused.mRefusal.empty());
    CHECK(refused.mEntries.empty());
}

TEST_CASE("A source node deletion restores nodes but keeps cleared references null", "[prefab][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress rootNode{ 1 };
    const SceneNodeAddress third{ 3 };
    const SceneNodeAddress fourth{ 4 };

    const StableId removedThird = fixture.Binding(third);
    const Entity removedThirdEntity = fixture.Member(third);
    REQUIRE(SetInstanceEntityRef(fixture.mWorld, fixture.mScene, fixture.mRoot, rootNode,
        "PrefabAuthoringTestData", "target", removedThird));

    const Entity ordinary = CreateSceneEntity(fixture.mWorld, fixture.mScene);
    DataOps().AddDefault(fixture.mWorld, ordinary);
    SetEntityField(fixture.mWorld, ordinary, DataOps(), "target", fixture.Member(third));

    const PrefabAuthoredKey thirdNode = DocumentKey(fixture.mDocument.mId, third);
    const PrefabAuthoredKey fourthNode = DocumentKey(fixture.mDocument.mId, fourth);
    const std::optional<std::string> thirdRecord = ReadOne(*fixture.mAuthoring, thirdNode);
    const std::optional<std::string> fourthRecord = ReadOne(*fixture.mAuthoring, fourthNode);
    REQUIRE(thirdRecord.has_value());
    REQUIRE(fourthRecord.has_value());

    fixture.mScene.mDirty = false;
    const std::vector<PrefabAuthoredEntry> deletion{
        PrefabAuthoredEntry{ thirdNode, std::nullopt },
        PrefabAuthoredEntry{ fourthNode, std::nullopt },
    };
    const mir::PrefabAuthoredWrite removed = fixture.mAuthoring->Write(deletion);
    REQUIRE(removed.mApplied);
    CHECK(removed.mSceneMaintained);

    fixture.Commit();
    CHECK_FALSE(fixture.mScene.mInstances.at(fixture.mRoot).mBindings.contains(third));
    CHECK_FALSE(fixture.mScene.mInstances.at(fixture.mRoot).mBindings.contains(fourth));
    CHECK_FALSE(fixture.mWorld.IsAlive(removedThirdEntity));

    const Entity root = fixture.Member(rootNode);
    CHECK(EntityField(fixture.mWorld, root, DataOps(), "target").IsNull());
    CHECK(EntityField(fixture.mWorld, ordinary, DataOps(), "target").IsNull());
    const mir::SceneFieldPatch *patch = FindInstanceFieldPatch(fixture.mScene.mInstances.at(fixture.mRoot), rootNode,
        "PrefabAuthoringTestData", "target");
    REQUIRE(patch != nullptr);
    CHECK(patch->mValue.is_null());
    CHECK(fixture.mScene.mDirty);

    const std::vector<PrefabAuthoredEntry> restore{
        PrefabAuthoredEntry{ thirdNode, *thirdRecord },
        PrefabAuthoredEntry{ fourthNode, *fourthRecord },
    };
    const mir::PrefabAuthoredWrite restored = fixture.mAuthoring->Write(restore);
    REQUIRE(restored.mApplied);
    CHECK_FALSE(restored.mSceneMaintained);

    fixture.Commit();
    CHECK(fixture.mScene.mInstances.at(fixture.mRoot).mBindings.contains(third));
    CHECK(fixture.mScene.mInstances.at(fixture.mRoot).mBindings.contains(fourth));
    CHECK(mir::ParentOf(fixture.mWorld, fixture.Member(third)) == fixture.Member(rootNode));
    CHECK(mir::ParentOf(fixture.mWorld, fixture.Member(fourth)) == fixture.Member(third));

    const std::span<const Entity> children = fixture.mWorld.HierarchyIndex().ChildrenOf(fixture.Member(rootNode));
    REQUIRE(children.size() == 2);
    CHECK(children[1] == fixture.Member(third));

    CHECK(EntityField(fixture.mWorld, root, DataOps(), "target").IsNull());
    CHECK(EntityField(fixture.mWorld, ordinary, DataOps(), "target").IsNull());
    CHECK(FindInstanceFieldPatch(fixture.mScene.mInstances.at(fixture.mRoot), rootNode,
              "PrefabAuthoringTestData", "target")
              ->mValue.is_null());
    CHECK(fixture.mScene.mDirty);
}

TEST_CASE("The instance root Transform is placement, not an authored field", "[prefab][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress rootNode{ 1 };
    const SceneNodeAddress second{ 2 };

    CHECK_FALSE(fixture.mAuthoring->FieldKey(fixture.Member(rootNode), "Transform", "position").has_value());
    CHECK_FALSE(fixture.mAuthoring->ComponentKey(fixture.Member(rootNode), "Transform").has_value());

    const std::optional<PrefabAuthoredKey> key =
        fixture.mAuthoring->FieldKey(fixture.Member(second), "Transform", "position");
    REQUIRE(key.has_value());
    CHECK(key->mAddress == std::vector<uint64_t>{ fixture.mRoot, 2 });
    CHECK(key->mComponent == "Transform");
    CHECK(key->mField == "position");
}

TEST_CASE("A mid-batch scene failure leaves the scene and documents unchanged", "[prefab][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress second{ 2 };
    const PrefabAuthoredKey data = SceneKey(fixture.mRoot, second, "PrefabAuthoringTestData");
    const PrefabAuthoredKey amount = SceneKey(fixture.mRoot, second, "PrefabAuthoringTestData", "amount");
    const PrefabAuthoredKey bonus = SceneKey(fixture.mRoot, second, "PrefabAuthoringTestData", "bonus");

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mRoot, second, "PrefabAuthoringTestData", "bonus", 4.5));

    const json documentBefore =
        mir::PrefabToJson(*fixture.mCoordinator->Registry().Find(fixture.mDocument.mId));
    const float amountBefore = FloatField(fixture.mWorld, fixture.Member(second), DataOps(), "amount");
    fixture.mScene.mDirty = false;

    // removing the component makes the field write that follows fail
    const std::vector<PrefabAuthoredEntry> entries{
        PrefabAuthoredEntry{ data, std::string("null") },
        PrefabAuthoredEntry{ amount, std::string("5.0") },
    };
    const mir::PrefabAuthoredWrite write = fixture.mAuthoring->Write(entries);
    CHECK_FALSE(write.mApplied);
    CHECK_FALSE(write.mSceneMaintained);
    CHECK(mir::PrefabToJson(*fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)) == documentBefore);
    CHECK(DataOps().Has(fixture.mWorld, fixture.Member(second)));
    CHECK(FloatField(fixture.mWorld, fixture.Member(second), DataOps(), "amount") == amountBefore);
    CHECK(FloatField(fixture.mWorld, fixture.Member(second), DataOps(), "bonus") == 4.5f);
    CHECK(ReadOne(*fixture.mAuthoring, data) == json{ { "bonus", 4.5 } }.dump());
    CHECK_FALSE(ReadOne(*fixture.mAuthoring, amount).has_value());
    CHECK(ReadOne(*fixture.mAuthoring, bonus) == "4.5");
    CHECK_FALSE(fixture.mScene.mDirty);
}

TEST_CASE("Only a publication that restructures an instance is scene maintenance", "[prefab][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress second{ 2 };

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mRoot, second, "PrefabAuthoringTestData", "amount", 5.0));
    fixture.Commit();
    CHECK_FALSE(fixture.mCoordinator->ConsumeSceneMaintenance());

    const PrefabAuthoredKey documentAmount =
        DocumentKey(fixture.mDocument.mId, second, "PrefabAuthoringTestData", "amount");
    const std::vector<PrefabAuthoredEntry> fieldEdit{
        PrefabAuthoredEntry{ documentAmount, std::string("6.0") } };
    CHECK(fixture.mAuthoring->Write(fieldEdit).mApplied);
    fixture.Commit();
    CHECK_FALSE(fixture.mCoordinator->ConsumeSceneMaintenance());

    const PrefabAuthoredKey leaf = DocumentKey(fixture.mDocument.mId, SceneNodeAddress{ 4 });
    const std::vector<PrefabAuthoredEntry> deletion{ PrefabAuthoredEntry{ leaf, std::nullopt } };
    CHECK(fixture.mAuthoring->Write(deletion).mApplied);
    fixture.Commit();
    CHECK(fixture.mCoordinator->ConsumeSceneMaintenance());
    CHECK_FALSE(fixture.mCoordinator->ConsumeSceneMaintenance());
}

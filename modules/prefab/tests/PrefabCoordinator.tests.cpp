/**
 * @file PrefabCoordinator.tests.cpp
 * @author Jackey Cen
 * @brief Tests for prefab/PrefabCoordinator.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/PrefabCoordinator.h>

#include <core/ecs/ComponentFieldChanges.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <prefab/PrefabAuthoring.h>
#include <prefab/PrefabFormat.h>
#include <prefab/PrefabOverrides.h>
#include <prefab/PrefabSceneInstance.h>
#include <scene/PlaySnapshot.h>
#include <scene/SceneInstancePatch.h>
#include <scene/SceneIO.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
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
    using mir::AddInstanceComponent;
    using mir::CommandBuffer;
    using mir::CompiledPrefab;
    using mir::ComponentFieldChange;
    using mir::ComponentFieldListeners;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::CreateSceneEntity;
    using mir::EngineMode;
    using mir::Entity;
    using mir::FieldDesc;
    using mir::FieldKind;
    using mir::FindInstanceFieldPatch;
    using mir::HasPrefabErrors;
    using mir::kNullStableId;
    using mir::LoadedScene;
    using mir::NewScene;
    using mir::PlacePrefabInstance;
    using mir::PrefabComponentInstance;
    using mir::PrefabCoordinator;
    using mir::PrefabDocument;
    using mir::PrefabFieldOverride;
    using mir::PrefabId;
    using mir::PrefabNode;
    using mir::PrefabPatch;
    using mir::PrefabNodeKind;
    using mir::PrefabSystem;
    using mir::RemoveInstanceComponent;
    using mir::RuntimeFieldDecl;
    using mir::SceneInstancePatch;
    using mir::SceneNodeAddress;
    using mir::ScenePrefabInstance;
    using mir::SetEngineMode;
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
        return ComponentRegistry::Instance().RegisterRuntime("PrefabCoordinatorTestData", fields);
    }

    const ComponentOps &ExtraOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "value", FieldKind::Float },
            { "link", FieldKind::EntityRef },
        };
        return ComponentRegistry::Instance().RegisterRuntime("PrefabCoordinatorTestExtra", fields);
    }

    struct CoordRuntimeComponent
    {
        float mAuthored = 0.0f;
        float mRuntimeOnly = 0.0f;
    };

    const ComponentOps &RuntimeOps()
    {
        static const FieldDesc fields[] = {
            { "authored", FieldKind::Float, offsetof(CoordRuntimeComponent, mAuthored) },
        };
        return ComponentRegistry::Instance().Register<CoordRuntimeComponent>(fields);
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
                ("mir_prefab_coordinator_test_" + ProcessToken() + "_" + std::to_string(counter.fetch_add(1)));
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
            "PrefabCoordinatorTestData",
            json{ { "amount", 1.0 }, { "bonus", 2.0 }, { "target", json::array({ "2" }) } } });
        document.mNodes[0].mComponents.push_back(PrefabComponentInstance{
            "Transform", json{ { "position", json::array({ 100.0, 0.0, 0.0 }) } } });

        PrefabNode second;
        second.mId = 2;
        second.mParent = 1;
        second.mComponents.push_back(PrefabComponentInstance{
            "PrefabCoordinatorTestData",
            json{ { "amount", 3.0 }, { "bonus", 4.0 }, { "target", nullptr } } });
        second.mComponents.push_back(PrefabComponentInstance{
            "CoordRuntimeComponent", json{ { "authored", 7.0 } } });

        PrefabNode third;
        third.mId = 3;
        third.mParent = 1;
        third.mComponents.push_back(PrefabComponentInstance{
            "PrefabCoordinatorTestData",
            json{ { "amount", 5.0 }, { "bonus", 6.0 }, { "target", nullptr } } });

        document.mNodes.push_back(std::move(second));
        document.mNodes.push_back(std::move(third));
        document.mNextNodeId = 4;
        return document;
    }

    PrefabNode *FindNode(PrefabDocument &document, uint64_t id)
    {
        for (PrefabNode &node : document.mNodes)
        {
            if (node.mId == id)
                return &node;
        }
        return nullptr;
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
        Entity value;
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

    double PositionX(World &world, Entity entity)
    {
        const mir::Transform *transform = world.GetComponent<mir::Transform>(entity);
        REQUIRE(transform != nullptr);
        return static_cast<double>(transform->Position().x);
    }

    std::vector<Entity> ChildrenOf(World &world, Entity parent)
    {
        const std::span<const Entity> children = world.HierarchyIndex().ChildrenOf(parent);
        return std::vector<Entity>(children.begin(), children.end());
    }

    struct Fixture
    {
        World mWorld;
        LoadedScene mScene;
        PrefabCoordinator *mCoordinator = nullptr;
        PrefabDocument mDocument;
        CommandBuffer mCommands;
        StableId mRoot = kNullStableId;
        StableId mSecondRoot = kNullStableId;

        explicit Fixture(bool secondInstance = false)
        {
            mir::RegisterCoreComponents();
            DataOps();
            ExtraOps();
            RuntimeOps();

            mWorld.EmplaceResource<mir::EngineModeState>();
            mWorld.EmplaceResource<ComponentFieldListeners>();
            mCoordinator = &mWorld.EmplaceResource<PrefabCoordinator>();

            mDocument = Document();
            REQUIRE(mCoordinator->Registry().SetDocument(mDocument));
            REQUIRE_FALSE(HasPrefabErrors(
                mCoordinator->Cache().Compile(mDocument, mir::RegistryLookup(mCoordinator->Registry()))));

            mScene = NewScene("test");
            const std::shared_ptr<const CompiledPrefab> prefab = mCoordinator->Cache().Find(mDocument.mId);
            REQUIRE(prefab != nullptr);

            mRoot = PlacePrefabInstance(mWorld, mScene, *prefab);
            REQUIRE(mRoot != kNullStableId);
            if (secondInstance)
            {
                mSecondRoot = PlacePrefabInstance(mWorld, mScene, *prefab);
                REQUIRE(mSecondRoot != kNullStableId);
            }
            mCoordinator->Bind(mScene);
        }

        StableId Binding(StableId root, const SceneNodeAddress &address) const
        {
            return mScene.mInstances.at(root).mBindings.at(address);
        }

        Entity Member(StableId root, const SceneNodeAddress &address) const
        {
            return mScene.mEntities.at(Binding(root, address));
        }

        void SetTemplateField(uint64_t nodeId, std::string_view component, std::string_view field, json value)
        {
            PrefabNode *node = FindNode(mDocument, nodeId);
            REQUIRE(node != nullptr);
            for (PrefabComponentInstance &instance : node->mComponents)
            {
                if (instance.mType == component)
                    instance.mFields[std::string(field)] = std::move(value);
            }
            REQUIRE(mCoordinator->SetDocument(mDocument));
        }

        void Commit()
        {
            PrefabSystem system;
            SystemContext context{ mWorld, mCommands };
            system.OnUpdate(context);
        }
    };

    struct DiskFixture
    {
        World mWorld;
        LoadedScene mScene;
        PrefabCoordinator *mCoordinator = nullptr;
        TempDir mPrefabs;
        PrefabDocument mDocument;
        CommandBuffer mCommands;
        StableId mRoot = kNullStableId;

        DiskFixture()
        {
            mir::RegisterCoreComponents();
            DataOps();
            ExtraOps();
            RuntimeOps();

            mWorld.EmplaceResource<mir::EngineModeState>();
            mWorld.EmplaceResource<ComponentFieldListeners>();

            mDocument = Document();
            REQUIRE(mir::WritePrefabFile(mPrefabs.mPath / "instance.prefab", mDocument));

            mCoordinator = &mWorld.EmplaceResource<PrefabCoordinator>();
            mCoordinator->ScanSources(mPrefabs.mPath);
            const PrefabDocument *loaded = mCoordinator->Registry().Find(mDocument.mId);
            REQUIRE(loaded != nullptr);
            REQUIRE_FALSE(HasPrefabErrors(
                mCoordinator->Cache().Compile(*loaded, mir::RegistryLookup(mCoordinator->Registry()))));

            mScene = NewScene("test");
            const std::shared_ptr<const CompiledPrefab> prefab = mCoordinator->Cache().Find(mDocument.mId);
            REQUIRE(prefab != nullptr);
            mRoot = PlacePrefabInstance(mWorld, mScene, *prefab);
            REQUIRE(mRoot != kNullStableId);
            mCoordinator->Bind(mScene);
        }

        Entity Member(const SceneNodeAddress &address) const
        {
            return mScene.mEntities.at(mScene.mInstances.at(mRoot).mBindings.at(address));
        }

        void Commit()
        {
            PrefabSystem system;
            SystemContext context{ mWorld, mCommands };
            system.OnUpdate(context);
        }
    };

    PrefabDocument NestedInnerDocument()
    {
        PrefabDocument document = mir::NewPrefabDocument();
        document.mNodes[0].mComponents.push_back(PrefabComponentInstance{
            "PrefabCoordinatorTestData",
            json{ { "amount", 1.0 }, { "bonus", 2.0 }, { "target", json::array({ "2" }) } } });
        document.mNodes[0].mComponents.push_back(PrefabComponentInstance{
            "Transform", json{ { "position", json::array({ 0.0, 0.0, 0.0 }) } } });

        PrefabNode child;
        child.mId = 2;
        child.mParent = 1;
        child.mComponents.push_back(PrefabComponentInstance{
            "PrefabCoordinatorTestData",
            json{ { "amount", 3.0 }, { "bonus", 4.0 }, { "target", nullptr } } });
        document.mNodes.push_back(std::move(child));
        document.mNextNodeId = 3;
        return document;
    }

    PrefabDocument NestedOuterDocument(PrefabId inner)
    {
        PrefabDocument document = mir::NewPrefabDocument();
        document.mNodes[0].mComponents.push_back(PrefabComponentInstance{
            "PrefabCoordinatorTestData",
            json{ { "amount", 10.0 }, { "bonus", 20.0 }, { "target", nullptr } } });

        PrefabNode first;
        first.mId = 2;
        first.mKind = PrefabNodeKind::Prefab;
        first.mParent = 1;
        first.mPrefab = inner;

        PrefabPatch reference;
        reference.mTarget = { 1 };
        reference.mFields.push_back(PrefabFieldOverride{
            "PrefabCoordinatorTestData", "target", json::array({ "2", "2" }) });
        first.mPatches.push_back(std::move(reference));

        PrefabPatch amount;
        amount.mTarget = { 2 };
        amount.mFields.push_back(PrefabFieldOverride{ "PrefabCoordinatorTestData", "amount", 20.0 });
        first.mPatches.push_back(std::move(amount));

        PrefabNode second;
        second.mId = 3;
        second.mKind = PrefabNodeKind::Prefab;
        second.mParent = 1;
        second.mPrefab = inner;

        PrefabPatch otherAmount;
        otherAmount.mTarget = { 2 };
        otherAmount.mFields.push_back(PrefabFieldOverride{ "PrefabCoordinatorTestData", "amount", 30.0 });
        second.mPatches.push_back(std::move(otherAmount));

        PrefabPatch outerReference;
        outerReference.mTarget = { 1 };
        outerReference.mFields.push_back(
            PrefabFieldOverride{ "PrefabCoordinatorTestData", "target", json::array({ "1" }) });
        second.mPatches.push_back(std::move(outerReference));

        document.mNodes.push_back(std::move(first));
        document.mNodes.push_back(std::move(second));
        document.mNextNodeId = 4;
        return document;
    }

    struct NestedFixture
    {
        World mWorld;
        LoadedScene mScene;
        PrefabCoordinator *mCoordinator = nullptr;
        PrefabDocument mInner;
        PrefabDocument mOuter;
        CommandBuffer mCommands;
        StableId mOuterRoot = kNullStableId;
        StableId mInnerRoot = kNullStableId;

        NestedFixture()
        {
            mir::RegisterCoreComponents();
            DataOps();
            ExtraOps();

            mWorld.EmplaceResource<mir::EngineModeState>();
            mWorld.EmplaceResource<ComponentFieldListeners>();
            mCoordinator = &mWorld.EmplaceResource<PrefabCoordinator>();

            mInner = NestedInnerDocument();
            mOuter = NestedOuterDocument(mInner.mId);
            REQUIRE(mCoordinator->Registry().SetDocument(mInner));
            REQUIRE(mCoordinator->Registry().SetDocument(mOuter));
            REQUIRE_FALSE(HasPrefabErrors(mCoordinator->Cache().Compile(mInner, Lookup())));
            REQUIRE_FALSE(HasPrefabErrors(mCoordinator->Cache().Compile(mOuter, Lookup())));

            mScene = NewScene("nested");
            const std::shared_ptr<const CompiledPrefab> outer = mCoordinator->Cache().Find(mOuter.mId);
            const std::shared_ptr<const CompiledPrefab> inner = mCoordinator->Cache().Find(mInner.mId);
            REQUIRE(outer != nullptr);
            REQUIRE(inner != nullptr);
            mOuterRoot = PlacePrefabInstance(mWorld, mScene, *outer);
            REQUIRE(mOuterRoot != kNullStableId);
            mInnerRoot = PlacePrefabInstance(mWorld, mScene, *inner);
            REQUIRE(mInnerRoot != kNullStableId);
            mCoordinator->Bind(mScene);
        }

        mir::PrefabDocumentLookup Lookup() const
        {
            return mir::RegistryLookup(mCoordinator->Registry());
        }

        StableId Binding(StableId root, const SceneNodeAddress &address) const
        {
            return mScene.mInstances.at(root).mBindings.at(address);
        }

        Entity Member(StableId root, const SceneNodeAddress &address) const
        {
            return mScene.mEntities.at(Binding(root, address));
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

TEST_CASE("A prefab edit publishes at the next Edit safe point", "[prefab][coordinator]")
{
    Fixture fixture(true);
    const Entity firstRoot = fixture.Member(fixture.mRoot, SceneNodeAddress{ 1 });
    const Entity secondRoot = fixture.Member(fixture.mSecondRoot, SceneNodeAddress{ 1 });
    const StableId firstId = fixture.Binding(fixture.mRoot, SceneNodeAddress{ 1 });
    const std::map<SceneNodeAddress, StableId> bindings = fixture.mScene.mInstances.at(fixture.mRoot).mBindings;
    fixture.mScene.mDirty = false;

    fixture.SetTemplateField(1, "PrefabCoordinatorTestData", "amount", 9.0);

    SetEngineMode(fixture.mWorld, EngineMode::Play);
    fixture.Commit();
    CHECK(FloatField(fixture.mWorld, firstRoot, DataOps(), "amount") == 1.0f);
    CHECK(FloatField(fixture.mWorld, secondRoot, DataOps(), "amount") == 1.0f);
    CHECK(fixture.mCoordinator->HasPendingWork());

    SetEngineMode(fixture.mWorld, EngineMode::Edit);
    fixture.Commit();

    CHECK(FloatField(fixture.mWorld, firstRoot, DataOps(), "amount") == 9.0f);
    CHECK(FloatField(fixture.mWorld, secondRoot, DataOps(), "amount") == 9.0f);
    CHECK(fixture.Member(fixture.mRoot, SceneNodeAddress{ 1 }) == firstRoot);
    CHECK(fixture.Binding(fixture.mRoot, SceneNodeAddress{ 1 }) == firstId);
    CHECK(fixture.mScene.mInstances.at(fixture.mRoot).mBindings == bindings);
    CHECK_FALSE(fixture.mCoordinator->HasPendingWork());
    CHECK_FALSE(fixture.mScene.mDirty);
}

TEST_CASE("An instance placed after Bind receives the next publication", "[prefab][coordinator]")
{
    Fixture fixture;
    const std::shared_ptr<const CompiledPrefab> prefab = fixture.mCoordinator->Cache().Find(fixture.mDocument.mId);
    REQUIRE(prefab != nullptr);

    const StableId late = PlacePrefabInstance(fixture.mWorld, fixture.mScene, *prefab);
    REQUIRE(late != kNullStableId);
    const StableId lateBinding = fixture.Binding(late, SceneNodeAddress{ 1 });
    const Entity lateRoot = fixture.Member(late, SceneNodeAddress{ 1 });

    fixture.SetTemplateField(1, "PrefabCoordinatorTestData", "amount", 27.0);
    fixture.Commit();

    CHECK(FloatField(fixture.mWorld, lateRoot, DataOps(), "amount") == 27.0f);
    CHECK(fixture.Binding(late, SceneNodeAddress{ 1 }) == lateBinding);
    CHECK(fixture.Member(late, SceneNodeAddress{ 1 }) == lateRoot);
}

TEST_CASE("An Apply edit publishes through the coordinator", "[prefab][coordinator]")
{
    Fixture fixture(true);
    const SceneNodeAddress rootAddress{ 1 };
    const Entity firstRoot = fixture.Member(fixture.mRoot, rootAddress);
    const Entity secondRoot = fixture.Member(fixture.mSecondRoot, rootAddress);
    const std::map<SceneNodeAddress, StableId> firstBindings =
        fixture.mScene.mInstances.at(fixture.mRoot).mBindings;

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mRoot, rootAddress, "PrefabCoordinatorTestData", "amount", 33.0));

    mir::PrefabPatchSelection selection;
    selection.mTarget = rootAddress;
    selection.mKind = mir::PrefabPatchKind::Field;
    selection.mComponent = "PrefabCoordinatorTestData";
    selection.mField = "amount";

    const mir::PrefabApplyResult applied = mir::ApplyPrefabInstancePatches(fixture.mScene,
        fixture.mCoordinator->Registry(), fixture.mCoordinator->Cache(), fixture.mRoot,
        std::span<const mir::PrefabPatchSelection>(&selection, 1));
    REQUIRE(applied.mApplied);
    CHECK(mir::InstanceFieldState(fixture.mScene, fixture.mRoot, rootAddress,
        "PrefabCoordinatorTestData", "amount") == mir::PrefabFieldState::Inherited);

    fixture.Commit();

    CHECK(FloatField(fixture.mWorld, firstRoot, DataOps(), "amount") == 33.0f);
    CHECK(FloatField(fixture.mWorld, secondRoot, DataOps(), "amount") == 33.0f);
    CHECK(fixture.Member(fixture.mRoot, rootAddress) == firstRoot);
    CHECK(fixture.Member(fixture.mSecondRoot, rootAddress) == secondRoot);
    CHECK(fixture.mScene.mInstances.at(fixture.mRoot).mBindings == firstBindings);
}

TEST_CASE("A publication keeps overrides and local component edits", "[prefab][coordinator]")
{
    Fixture fixture(true);
    const SceneNodeAddress rootAddress{ 1 };
    const SceneNodeAddress secondAddress{ 2 };
    const SceneNodeAddress thirdAddress{ 3 };

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(), fixture.mSecondRoot,
        rootAddress, "PrefabCoordinatorTestData", "amount", 21.0));
    REQUIRE(RemoveInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mSecondRoot, secondAddress, "PrefabCoordinatorTestData"));
    REQUIRE(AddInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mSecondRoot, thirdAddress, "PrefabCoordinatorTestExtra"));
    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(), fixture.mSecondRoot,
        thirdAddress, "PrefabCoordinatorTestExtra", "value", 11.0));

    fixture.SetTemplateField(1, "PrefabCoordinatorTestData", "amount", 13.0);
    fixture.SetTemplateField(2, "PrefabCoordinatorTestData", "amount", 14.0);
    fixture.SetTemplateField(3, "PrefabCoordinatorTestData", "amount", 15.0);
    fixture.Commit();

    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mRoot, rootAddress), DataOps(), "amount") == 13.0f);
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mSecondRoot, rootAddress), DataOps(), "amount") == 21.0f);

    const Entity firstSecond = fixture.Member(fixture.mRoot, secondAddress);
    CHECK(DataOps().Has(fixture.mWorld, firstSecond));
    CHECK(FloatField(fixture.mWorld, firstSecond, DataOps(), "amount") == 14.0f);
    CHECK_FALSE(DataOps().Has(fixture.mWorld, fixture.Member(fixture.mSecondRoot, secondAddress)));

    for (StableId root : { fixture.mRoot, fixture.mSecondRoot })
    {
        const Entity third = fixture.Member(root, thirdAddress);
        CHECK(DataOps().Has(fixture.mWorld, third));
        CHECK(FloatField(fixture.mWorld, third, DataOps(), "amount") == 15.0f);
    }
    CHECK_FALSE(ExtraOps().Has(fixture.mWorld, fixture.Member(fixture.mRoot, thirdAddress)));
    CHECK(ExtraOps().Has(fixture.mWorld, fixture.Member(fixture.mSecondRoot, thirdAddress)));
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mSecondRoot, thirdAddress), ExtraOps(), "value") == 11.0f);
}

TEST_CASE("A published root Transform keeps the instance placement", "[prefab][coordinator]")
{
    Fixture fixture;
    const Entity root = fixture.Member(fixture.mRoot, SceneNodeAddress{ 1 });
    const Entity child = fixture.Member(fixture.mRoot, SceneNodeAddress{ 2 });
    REQUIRE(PositionX(fixture.mWorld, root) == 100.0);

    fixture.SetTemplateField(1, "Transform", "position", json::array({ 7.0, 0.0, 0.0 }));
    fixture.SetTemplateField(1, "PrefabCoordinatorTestData", "amount", 23.0);
    fixture.SetTemplateField(2, "PrefabCoordinatorTestData", "amount", 24.0);
    fixture.Commit();

    CHECK(PositionX(fixture.mWorld, root) == 100.0);
    CHECK(FloatField(fixture.mWorld, root, DataOps(), "amount") == 23.0f);
    CHECK(FloatField(fixture.mWorld, child, DataOps(), "amount") == 24.0f);
}

TEST_CASE("A retargeted reference follows the node address across a reorder", "[prefab][coordinator]")
{
    Fixture fixture;
    const Entity root = fixture.Member(fixture.mRoot, SceneNodeAddress{ 1 });
    const Entity oldTarget = fixture.Member(fixture.mRoot, SceneNodeAddress{ 2 });
    const Entity newTarget = fixture.Member(fixture.mRoot, SceneNodeAddress{ 3 });
    REQUIRE(EntityField(fixture.mWorld, root, DataOps(), "target") == oldTarget);

    fixture.SetTemplateField(1, "PrefabCoordinatorTestData", "target", json::array({ "3" }));
    std::swap(fixture.mDocument.mNodes[1], fixture.mDocument.mNodes[2]);
    REQUIRE(fixture.mCoordinator->SetDocument(fixture.mDocument));

    fixture.Commit();

    CHECK(EntityField(fixture.mWorld, root, DataOps(), "target") == newTarget);
    CHECK(fixture.Member(fixture.mRoot, SceneNodeAddress{ 1 }) == root);
}

TEST_CASE("A sibling reorder in the source publishes to the instance", "[prefab][coordinator]")
{
    Fixture fixture;
    const Entity root = fixture.Member(fixture.mRoot, SceneNodeAddress{ 1 });
    const Entity second = fixture.Member(fixture.mRoot, SceneNodeAddress{ 2 });
    const Entity third = fixture.Member(fixture.mRoot, SceneNodeAddress{ 3 });
    const std::map<SceneNodeAddress, StableId> bindings = fixture.mScene.mInstances.at(fixture.mRoot).mBindings;
    REQUIRE(ChildrenOf(fixture.mWorld, root) == std::vector<Entity>{ second, third });

    std::swap(fixture.mDocument.mNodes[1], fixture.mDocument.mNodes[2]);
    REQUIRE(fixture.mCoordinator->SetDocument(fixture.mDocument));
    fixture.Commit();

    const std::vector<Entity> children = ChildrenOf(fixture.mWorld, root);
    REQUIRE(children.size() == 2);
    CHECK(children[0] == third);
    CHECK(children[1] == second);
    CHECK(fixture.Member(fixture.mRoot, SceneNodeAddress{ 2 }) == second);
    CHECK(fixture.Member(fixture.mRoot, SceneNodeAddress{ 3 }) == third);
    CHECK(fixture.mScene.mInstances.at(fixture.mRoot).mBindings == bindings);
}

TEST_CASE("A publication reports script resolve work once", "[prefab][coordinator]")
{
    Fixture fixture;
    CHECK_FALSE(fixture.mCoordinator->ConsumePublishedChanges());
    fixture.Commit();
    CHECK_FALSE(fixture.mCoordinator->ConsumePublishedChanges());

    fixture.SetTemplateField(1, "PrefabCoordinatorTestData", "amount", 63.0);
    fixture.Commit();

    CHECK(fixture.mCoordinator->ConsumePublishedChanges());
    CHECK_FALSE(fixture.mCoordinator->ConsumePublishedChanges());
}

TEST_CASE("A node added to the prefab appears in every instance with a fresh StableId", "[prefab][coordinator]")
{
    Fixture fixture(true);
    const StableId nextId = fixture.mScene.mNextId;

    PrefabNode added;
    added.mId = 4;
    added.mParent = 1;
    added.mComponents.push_back(PrefabComponentInstance{
        "PrefabCoordinatorTestData", json{ { "amount", 31.0 }, { "target", nullptr } } });
    fixture.mDocument.mNodes.push_back(std::move(added));
    fixture.mDocument.mNextNodeId = 5;
    REQUIRE(fixture.mCoordinator->SetDocument(fixture.mDocument));
    fixture.Commit();

    const StableId firstAdded = fixture.Binding(fixture.mRoot, SceneNodeAddress{ 4 });
    const StableId secondAdded = fixture.Binding(fixture.mSecondRoot, SceneNodeAddress{ 4 });
    CHECK(firstAdded >= nextId);
    CHECK(secondAdded >= nextId);
    CHECK(firstAdded != secondAdded);

    const Entity first = fixture.mScene.mEntities.at(firstAdded);
    CHECK(mir::ParentOf(fixture.mWorld, first) == fixture.Member(fixture.mRoot, SceneNodeAddress{ 1 }));
    CHECK(FloatField(fixture.mWorld, first, DataOps(), "amount") == 31.0f);
}

TEST_CASE("Deleting a source node clears references and marks the scene dirty", "[prefab][coordinator]")
{
    Fixture fixture;
    const SceneNodeAddress rootAddress{ 1 };
    const StableId removed = fixture.Binding(fixture.mRoot, SceneNodeAddress{ 3 });

    REQUIRE(SetInstanceEntityRef(fixture.mWorld, fixture.mScene, fixture.mRoot, rootAddress,
        "PrefabCoordinatorTestData", "target", removed));

    const Entity ordinary = CreateSceneEntity(fixture.mWorld, fixture.mScene);
    DataOps().AddDefault(fixture.mWorld, ordinary);
    const Entity removedEntity = fixture.mScene.mEntities.at(removed);
    SetEntityField(fixture.mWorld, ordinary, DataOps(), "target", removedEntity);

    fixture.mScene.mDirty = false;
    std::erase_if(fixture.mDocument.mNodes, [](const PrefabNode &node) { return node.mId == 3; });
    REQUIRE(fixture.mCoordinator->SetDocument(fixture.mDocument));
    fixture.Commit();

    CHECK_FALSE(fixture.mWorld.IsAlive(removedEntity));
    CHECK_FALSE(fixture.mScene.mEntities.contains(removed));
    CHECK_FALSE(fixture.mScene.mInstances.at(fixture.mRoot).mBindings.contains(SceneNodeAddress{ 3 }));
    CHECK(fixture.mScene.mDirty);

    const Entity root = fixture.Member(fixture.mRoot, rootAddress);
    CHECK(EntityField(fixture.mWorld, root, DataOps(), "target").IsNull());
    CHECK(EntityField(fixture.mWorld, ordinary, DataOps(), "target").IsNull());

    const mir::SceneFieldPatch *patch = FindInstanceFieldPatch(fixture.mScene.mInstances.at(fixture.mRoot), rootAddress,
        "PrefabCoordinatorTestData", "target");
    REQUIRE(patch != nullptr);
    CHECK(patch->mValue.is_null());
}

TEST_CASE("Deleting a source node unregisters its ordinary descendants", "[prefab][coordinator]")
{
    Fixture fixture;
    std::vector<Entity> removedEntities;
    const mir::Registration hooks = fixture.mWorld.SetComponentHooks<CoordRuntimeComponent>({
        .onRemove = [&removedEntities](World &, Entity entity) { removedEntities.push_back(entity); } });

    const StableId removed = fixture.Binding(fixture.mRoot, SceneNodeAddress{ 3 });
    const Entity member = fixture.mScene.mEntities.at(removed);
    const Entity child = CreateSceneEntity(fixture.mWorld, fixture.mScene, member);
    fixture.mWorld.AddComponent<CoordRuntimeComponent>(child, CoordRuntimeComponent{});

    StableId childId = kNullStableId;
    for (const auto &[id, entity] : fixture.mScene.mEntities)
    {
        if (entity == child)
            childId = id;
    }
    REQUIRE(childId != kNullStableId);

    std::erase_if(fixture.mDocument.mNodes, [](const PrefabNode &node) { return node.mId == 3; });
    REQUIRE(fixture.mCoordinator->SetDocument(fixture.mDocument));
    fixture.Commit();

    CHECK_FALSE(fixture.mWorld.IsAlive(child));
    CHECK_FALSE(fixture.mScene.mEntities.contains(childId));
    CHECK(std::find(removedEntities.begin(), removedEntities.end(), child) != removedEntities.end());
    for (const auto &[id, entity] : fixture.mScene.mEntities)
    {
        (void)id;
        CHECK(fixture.mWorld.IsAlive(entity));
    }

    TempDir dir;
    CHECK(mir::SaveScene(fixture.mWorld, dir.mPath, fixture.mScene, &fixture.mCoordinator->Resolver()));
}

TEST_CASE("A cleared reference stays null when the source node returns", "[prefab][coordinator]")
{
    Fixture fixture;
    const SceneNodeAddress rootAddress{ 1 };
    const StableId removed = fixture.Binding(fixture.mRoot, SceneNodeAddress{ 3 });
    REQUIRE(SetInstanceEntityRef(fixture.mWorld, fixture.mScene, fixture.mRoot, rootAddress,
        "PrefabCoordinatorTestData", "target", removed));

    std::erase_if(fixture.mDocument.mNodes, [](const PrefabNode &node) { return node.mId == 3; });
    REQUIRE(fixture.mCoordinator->SetDocument(fixture.mDocument));
    fixture.Commit();

    PrefabNode returning;
    returning.mId = 3;
    returning.mParent = 1;
    returning.mComponents.push_back(PrefabComponentInstance{
        "PrefabCoordinatorTestData", json{ { "amount", 17.0 }, { "target", nullptr } } });
    fixture.mDocument.mNodes.push_back(std::move(returning));
    fixture.mDocument.mNextNodeId = 4;
    REQUIRE(fixture.mCoordinator->SetDocument(fixture.mDocument));
    fixture.Commit();

    const StableId restored = fixture.Binding(fixture.mRoot, SceneNodeAddress{ 3 });
    CHECK(restored != removed);
    CHECK(fixture.mScene.mDirty);

    const Entity root = fixture.Member(fixture.mRoot, rootAddress);
    CHECK(EntityField(fixture.mWorld, root, DataOps(), "target").IsNull());

    TempDir dir;
    REQUIRE(mir::SaveScene(fixture.mWorld, dir.mPath, fixture.mScene, &fixture.mCoordinator->Resolver()));

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, &fixture.mCoordinator->Resolver());
    CHECK(EntityField(loadedWorld, loaded.mEntities.at(fixture.mRoot), DataOps(), "target").IsNull());
    CHECK(loaded.mInstances.at(fixture.mRoot).mBindings.at(SceneNodeAddress{ 3 }) != removed);
}

TEST_CASE("A failed compile publishes nothing", "[prefab][coordinator]")
{
    Fixture fixture(true);
    const Entity root = fixture.Member(fixture.mRoot, SceneNodeAddress{ 1 });
    const Entity target = fixture.Member(fixture.mRoot, SceneNodeAddress{ 2 });
    const std::map<SceneNodeAddress, StableId> bindings = fixture.mScene.mInstances.at(fixture.mRoot).mBindings;
    const uint64_t revision = fixture.mCoordinator->PublishedRevision(fixture.mRoot);
    REQUIRE(SetInstanceEntityRef(fixture.mWorld, fixture.mScene, fixture.mRoot, SceneNodeAddress{ 1 },
        "PrefabCoordinatorTestData", "target", fixture.Binding(fixture.mRoot, SceneNodeAddress{ 2 })));
    fixture.mScene.mDirty = false;

    fixture.SetTemplateField(1, "PrefabCoordinatorTestData", "amount", 41.0);

    PrefabDocument broken = fixture.mDocument;
    PrefabNode nested;
    nested.mId = 5;
    nested.mKind = PrefabNodeKind::Prefab;
    nested.mParent = 1;
    nested.mPrefab = mir::GeneratePrefabId();
    broken.mNodes.push_back(std::move(nested));
    broken.mNextNodeId = 6;
    REQUIRE(fixture.mCoordinator->SetDocument(broken));

    fixture.Commit();

    CHECK(FloatField(fixture.mWorld, root, DataOps(), "amount") == 1.0f);
    CHECK(EntityField(fixture.mWorld, root, DataOps(), "target") == target);
    CHECK(fixture.Member(fixture.mRoot, SceneNodeAddress{ 1 }) == root);
    CHECK(fixture.mScene.mInstances.at(fixture.mRoot).mBindings == bindings);
    CHECK(fixture.mCoordinator->PublishedRevision(fixture.mRoot) == revision);
    CHECK_FALSE(fixture.mScene.mDirty);
    CHECK_FALSE(fixture.mCoordinator->HasPendingWork());
}

TEST_CASE("Listeners see the old values before the commit and the new values after", "[prefab][coordinator]")
{
    struct Recorder
    {
        std::vector<std::string> mCalls;
        std::vector<float> mValues;
        std::vector<ComponentFieldChange> mBefore;
        std::vector<ComponentFieldChange> mAfter;
    };

    static Recorder recorder;
    recorder = Recorder{};

    const auto beforeFirst = [](World &, std::span<const ComponentFieldChange>, void *user)
        {
            static_cast<Recorder *>(user)->mCalls.push_back("before1");
        };
    const auto beforeSecond = [](World &world, std::span<const ComponentFieldChange> changes, void *user)
        {
            auto &state = *static_cast<Recorder *>(user);
            state.mCalls.push_back("before2");
            state.mBefore.assign(changes.begin(), changes.end());
            for (const ComponentFieldChange &change : changes)
            {
                if (!(change.mType == DataOps().mType))
                    continue;
                float value = 0.0f;
                if (const FieldDesc *field = DataOps().FindField("amount"))
                    field->Read(DataOps().GetComponent(world, change.mEntity), &value);
                state.mValues.push_back(value);
            }
        };
    const auto afterFirst = [](World &, std::span<const ComponentFieldChange>, void *user)
        {
            static_cast<Recorder *>(user)->mCalls.push_back("after1");
        };
    const auto afterSecond = [](World &world, std::span<const ComponentFieldChange> changes, void *user)
        {
            auto &state = *static_cast<Recorder *>(user);
            state.mCalls.push_back("after2");
            state.mAfter.assign(changes.begin(), changes.end());
            for (const ComponentFieldChange &change : changes)
            {
                if (!(change.mType == DataOps().mType))
                    continue;
                float value = 0.0f;
                if (const FieldDesc *field = DataOps().FindField("amount"))
                    field->Read(DataOps().GetComponent(world, change.mEntity), &value);
                state.mValues.push_back(value);
            }
        };

    Fixture fixture;
    ComponentFieldListeners &listeners = fixture.mWorld.Resource<ComponentFieldListeners>();
    listeners.AddBeforeChange(+beforeFirst, &recorder);
    listeners.AddBeforeChange(+beforeSecond, &recorder);
    listeners.AddAfterChange(+afterFirst, &recorder);
    listeners.AddAfterChange(+afterSecond, &recorder);

    const Entity second = fixture.Member(fixture.mRoot, SceneNodeAddress{ 2 });
    World &world = fixture.mWorld;
    REQUIRE(world.Has<CoordRuntimeComponent>(second));
    world.GetComponent<CoordRuntimeComponent>(second)->mRuntimeOnly = 42.0f;

    fixture.SetTemplateField(1, "PrefabCoordinatorTestData", "amount", 19.0f);
    fixture.SetTemplateField(2, "CoordRuntimeComponent", "authored", 23.0f);
    fixture.Commit();

    CHECK(recorder.mCalls == std::vector<std::string>{ "before1", "before2", "after1", "after2" });
    REQUIRE(recorder.mValues.size() == 2);
    CHECK(recorder.mValues.at(0) == 1.0f);
    CHECK(recorder.mValues.at(1) == 19.0f);

    REQUIRE_FALSE(recorder.mBefore.empty());
    REQUIRE_FALSE(recorder.mAfter.empty());

    CHECK(world.GetComponent<CoordRuntimeComponent>(second)->mRuntimeOnly == 42.0f);
    CHECK(world.GetComponent<CoordRuntimeComponent>(second)->mAuthored == 23.0f);
}

TEST_CASE("Component additions and removals reach the component hooks, not the field listeners", "[prefab][coordinator]")
{
    static int fieldCalls;
    fieldCalls = 0;
    const auto count = [](World &, std::span<const ComponentFieldChange>, void *) { ++fieldCalls; };

    Fixture fixture;
    fixture.mWorld.Resource<ComponentFieldListeners>().AddBeforeChange(+count);
    fixture.mWorld.Resource<ComponentFieldListeners>().AddAfterChange(+count);

    std::vector<Entity> added;
    std::vector<Entity> removed;
    const mir::Registration hooks = fixture.mWorld.SetComponentHooks<CoordRuntimeComponent>({
        .onAdd = [&added](World &, Entity entity) { added.push_back(entity); },
        .onRemove = [&removed](World &, Entity entity) { removed.push_back(entity); } });

    const Entity second = fixture.Member(fixture.mRoot, SceneNodeAddress{ 2 });
    const Entity third = fixture.Member(fixture.mRoot, SceneNodeAddress{ 3 });

    PrefabNode *secondNode = FindNode(fixture.mDocument, 2);
    REQUIRE(secondNode != nullptr);
    std::erase_if(secondNode->mComponents, [](const PrefabComponentInstance &component)
        {
            return component.mType == "CoordRuntimeComponent";
        });

    PrefabNode *thirdNode = FindNode(fixture.mDocument, 3);
    REQUIRE(thirdNode != nullptr);
    thirdNode->mComponents.push_back(PrefabComponentInstance{ "CoordRuntimeComponent", json{ { "authored", 5.0 } } });

    REQUIRE(fixture.mCoordinator->SetDocument(fixture.mDocument));
    fixture.Commit();

    CHECK(removed == std::vector<Entity>{ second });
    CHECK(added == std::vector<Entity>{ third });
    CHECK(fieldCalls == 0);
    CHECK_FALSE(fixture.mWorld.Has<CoordRuntimeComponent>(second));
    CHECK(fixture.mWorld.Has<CoordRuntimeComponent>(third));
}

TEST_CASE("A frozen coordinator publishes nothing", "[prefab][coordinator]")
{
    Fixture fixture(true);
    const Entity firstRoot = fixture.Member(fixture.mRoot, SceneNodeAddress{ 1 });
    const Entity secondRoot = fixture.Member(fixture.mSecondRoot, SceneNodeAddress{ 1 });

    fixture.SetTemplateField(1, "PrefabCoordinatorTestData", "amount", 51.0);
    fixture.mCoordinator->SetFrozen(true);
    fixture.Commit();

    CHECK(FloatField(fixture.mWorld, firstRoot, DataOps(), "amount") == 1.0f);
    CHECK(FloatField(fixture.mWorld, secondRoot, DataOps(), "amount") == 1.0f);
    CHECK(fixture.mCoordinator->HasPendingWork());

    fixture.mCoordinator->SetFrozen(false);
    fixture.Commit();
    CHECK(FloatField(fixture.mWorld, firstRoot, DataOps(), "amount") == 51.0f);
    CHECK(FloatField(fixture.mWorld, secondRoot, DataOps(), "amount") == 51.0f);
}

TEST_CASE("A rescan while frozen waits for the next Edit safe point", "[prefab][coordinator]")
{
    DiskFixture fixture;
    const SceneNodeAddress rootAddress{ 1 };

    fixture.mDocument.mNodes.front().mComponents.front().mFields["amount"] = 61.0;
    REQUIRE(mir::WritePrefabFile(fixture.mPrefabs.mPath / "instance.prefab", fixture.mDocument));

    fixture.mCoordinator->SetFrozen(true);
    fixture.mCoordinator->RescanSources(fixture.mPrefabs.mPath);
    CHECK(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.front().mComponents.front()
        .mFields.at("amount") == 1.0);
    fixture.Commit();
    CHECK(FloatField(fixture.mWorld, fixture.Member(rootAddress), DataOps(), "amount") == 1.0f);

    CHECK(fixture.mCoordinator->ConsumeExternalChanges().empty());
    fixture.mCoordinator->SetFrozen(false);
    fixture.Commit();
    CHECK(fixture.mCoordinator->ConsumeExternalChanges() == std::vector<mir::PrefabId>{ fixture.mDocument.mId });
    CHECK(fixture.mCoordinator->ConsumeExternalChanges().empty());
    CHECK(FloatField(fixture.mWorld, fixture.Member(rootAddress), DataOps(), "amount") == 61.0f);
}

TEST_CASE("A rescan keeps an unsaved in-memory document", "[prefab][coordinator]")
{
    DiskFixture fixture;
    const SceneNodeAddress rootAddress{ 1 };

    PrefabDocument edited = fixture.mDocument;
    edited.mNodes.front().mComponents.front().mFields["amount"] = 71.0;
    REQUIRE(fixture.mCoordinator->SetDocument(edited));

    fixture.mDocument.mNodes.front().mComponents.front().mFields["amount"] = 81.0;
    REQUIRE(mir::WritePrefabFile(fixture.mPrefabs.mPath / "instance.prefab", fixture.mDocument));

    fixture.mCoordinator->RescanSources(fixture.mPrefabs.mPath);
    CHECK(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.front().mComponents.front()
        .mFields.at("amount") == 71.0);

    fixture.Commit();
    CHECK(fixture.mCoordinator->ConsumeExternalChanges().empty());
    CHECK(FloatField(fixture.mWorld, fixture.Member(rootAddress), DataOps(), "amount") == 71.0f);
}

TEST_CASE("A rescan replaces an in-memory document that matches its saved file", "[prefab][coordinator]")
{
    DiskFixture fixture;
    const SceneNodeAddress rootAddress{ 1 };

    REQUIRE(fixture.mCoordinator->SetDocument(fixture.mDocument));
    fixture.Commit();

    fixture.mDocument.mNodes.front().mComponents.front().mFields["amount"] = 91.0;
    REQUIRE(mir::WritePrefabFile(fixture.mPrefabs.mPath / "instance.prefab", fixture.mDocument));

    fixture.mCoordinator->RescanSources(fixture.mPrefabs.mPath);
    CHECK(fixture.mCoordinator->ConsumeExternalChanges() == std::vector<mir::PrefabId>{ fixture.mDocument.mId });
    CHECK_FALSE(fixture.mCoordinator->Registry().HasMemoryDocument(fixture.mDocument.mId));
    CHECK(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.front().mComponents.front()
        .mFields.at("amount") == 91.0);

    fixture.Commit();
    CHECK(FloatField(fixture.mWorld, fixture.Member(rootAddress), DataOps(), "amount") == 91.0f);
}

TEST_CASE("Play and Stop leave an unchanged saved scene byte-identical", "[prefab][coordinator]")
{
    Fixture fixture;
    const SceneNodeAddress rootAddress{ 1 };
    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mRoot, rootAddress, "PrefabCoordinatorTestData", "target", json(nullptr)));

    TempDir sceneDir;
    REQUIRE(mir::SaveScene(fixture.mWorld, sceneDir.mPath, fixture.mScene, &fixture.mCoordinator->Resolver()));
    const std::map<std::string, std::string> filesBefore = SnapshotFiles(sceneDir.mPath);
    const std::map<SceneNodeAddress, StableId> bindingsBefore =
        fixture.mScene.mInstances.at(fixture.mRoot).mBindings;

    fixture.mCoordinator->Commit(fixture.mWorld);
    fixture.mCoordinator->SetFrozen(true);
    mir::PlaySnapshot snapshot = mir::CaptureWorld(fixture.mWorld, fixture.mScene,
        &fixture.mCoordinator->Resolver());
    LoadedScene restored = mir::RestoreWorld(fixture.mWorld, snapshot,
        &fixture.mCoordinator->Resolver());
    fixture.mCoordinator->Bind(restored);
    fixture.mCoordinator->SetFrozen(false);
    fixture.Commit();

    REQUIRE(restored.mInstances.contains(fixture.mRoot));
    CHECK(restored.mInstances.at(fixture.mRoot).mBindings == bindingsBefore);
    CHECK(restored.mEntities.contains(bindingsBefore.at(rootAddress)));
    CHECK(EntityField(fixture.mWorld, restored.mEntities.at(bindingsBefore.at(rootAddress)), DataOps(), "target")
        .IsNull());
    const mir::SceneFieldPatch *patch = FindInstanceFieldPatch(restored.mInstances.at(fixture.mRoot), rootAddress,
        "PrefabCoordinatorTestData", "target");
    REQUIRE(patch != nullptr);

    REQUIRE(mir::SaveScene(fixture.mWorld, sceneDir.mPath, restored, &fixture.mCoordinator->Resolver()));
    CHECK(SnapshotFiles(sceneDir.mPath) == filesBefore);
    CHECK(EntityField(fixture.mWorld, restored.mEntities.at(bindingsBefore.at(rootAddress)), DataOps(), "target")
        .IsNull());
    CHECK(FindInstanceFieldPatch(restored.mInstances.at(fixture.mRoot), rootAddress,
        "PrefabCoordinatorTestData", "target") != nullptr);

    mir::DiscardSnapshot(snapshot);
}

TEST_CASE("Nested occurrences compose inner defaults, outer patches and scene overrides", "[prefab][coordinator][nesting]")
{
    NestedFixture fixture;
    const SceneNodeAddress innerRoot{ 2, 1 };
    const SceneNodeAddress innerChild{ 2, 2 };
    const SceneNodeAddress otherChild{ 3, 2 };

    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mOuterRoot, innerRoot), DataOps(), "amount") == 1.0f);
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mOuterRoot, innerChild), DataOps(), "amount") == 20.0f);
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mOuterRoot, otherChild), DataOps(), "amount") == 30.0f);
    CHECK(EntityField(fixture.mWorld, fixture.Member(fixture.mOuterRoot, innerRoot), DataOps(), "target") ==
        fixture.Member(fixture.mOuterRoot, innerChild));
    CHECK(EntityField(fixture.mWorld, fixture.Member(fixture.mOuterRoot, SceneNodeAddress{ 3, 1 }),
        DataOps(), "target") == fixture.Member(fixture.mOuterRoot, SceneNodeAddress{ 1 }));
    CHECK(EntityField(fixture.mWorld, fixture.Member(fixture.mInnerRoot, SceneNodeAddress{ 1 }),
        DataOps(), "target") == fixture.Member(fixture.mInnerRoot, SceneNodeAddress{ 2 }));
    CHECK(fixture.Binding(fixture.mOuterRoot, SceneNodeAddress{ 2, 2 }) !=
        fixture.Binding(fixture.mOuterRoot, SceneNodeAddress{ 3, 2 }));

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mOuterRoot, innerChild, "PrefabCoordinatorTestData", "amount", 45.0));
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mOuterRoot, innerChild), DataOps(), "amount") == 45.0f);
    CHECK(mir::InstanceFieldState(fixture.mScene, fixture.mOuterRoot, innerChild,
        "PrefabCoordinatorTestData", "amount") == mir::PrefabFieldState::Overridden);

    REQUIRE(mir::RevertInstancePatch(fixture.mWorld, fixture.mCoordinator->Registry(),
        fixture.mCoordinator->Cache(), fixture.mScene, fixture.mOuterRoot,
        mir::PrefabPatchSelection{ innerChild, mir::PrefabPatchKind::Field,
            "PrefabCoordinatorTestData", "amount" }));
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mOuterRoot, innerChild), DataOps(), "amount") == 20.0f);

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mOuterRoot, innerChild, "PrefabCoordinatorTestData", "amount", 45.0));

    const StableId otherId = fixture.Binding(fixture.mOuterRoot, otherChild);
    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mOuterRoot, innerRoot, "PrefabCoordinatorTestData", "target", otherId));
    CHECK(EntityField(fixture.mWorld, fixture.Member(fixture.mOuterRoot, innerRoot), DataOps(), "target") ==
        fixture.Member(fixture.mOuterRoot, otherChild));

    TempDir dir;
    REQUIRE(mir::SaveScene(fixture.mWorld, dir.mPath, fixture.mScene, &fixture.mCoordinator->Resolver()));

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, &fixture.mCoordinator->Resolver());

    CHECK(loaded.mInstances.at(fixture.mOuterRoot).mBindings ==
        fixture.mScene.mInstances.at(fixture.mOuterRoot).mBindings);
    CHECK(mir::InstanceFieldState(loaded, fixture.mOuterRoot, innerRoot,
        "PrefabCoordinatorTestData", "target") == mir::PrefabFieldState::Overridden);
    CHECK(mir::InstanceFieldState(loaded, fixture.mOuterRoot, innerChild,
        "PrefabCoordinatorTestData", "amount") == mir::PrefabFieldState::Overridden);
    CHECK(FloatField(loadedWorld, loaded.mEntities.at(fixture.Binding(fixture.mOuterRoot, innerChild)),
        DataOps(), "amount") == 45.0f);
    CHECK(EntityField(loadedWorld, loaded.mEntities.at(fixture.Binding(fixture.mOuterRoot, innerRoot)),
        DataOps(), "target") == loaded.mEntities.at(loaded.mInstances.at(fixture.mOuterRoot).mBindings.at(otherChild)));
    CHECK(FloatField(loadedWorld, loaded.mEntities.at(fixture.Binding(fixture.mOuterRoot, otherChild)),
        DataOps(), "amount") == 30.0f);

    TempDir second;
    REQUIRE(mir::SaveScene(loadedWorld, second.mPath, loaded, &fixture.mCoordinator->Resolver()));
    CHECK(SnapshotFiles(dir.mPath) == SnapshotFiles(second.mPath));
}

TEST_CASE("Scene components on a nested member add, remove and revert", "[prefab][coordinator][nesting]")
{
    NestedFixture fixture;
    const SceneNodeAddress child{ 2, 2 };

    REQUIRE(AddInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mOuterRoot, child, "PrefabCoordinatorTestExtra"));
    CHECK(ExtraOps().Has(fixture.mWorld, fixture.Member(fixture.mOuterRoot, child)));
    CHECK(mir::InstanceComponentState(fixture.mCoordinator->Registry(), fixture.mScene, fixture.mOuterRoot, child,
        "PrefabCoordinatorTestExtra") == mir::PrefabComponentState::Added);

    REQUIRE(RemoveInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mOuterRoot, child, "PrefabCoordinatorTestData"));
    CHECK_FALSE(DataOps().Has(fixture.mWorld, fixture.Member(fixture.mOuterRoot, child)));
    CHECK(mir::InstanceComponentState(fixture.mCoordinator->Registry(), fixture.mScene, fixture.mOuterRoot, child,
        "PrefabCoordinatorTestData") == mir::PrefabComponentState::Removed);

    REQUIRE(mir::RevertInstancePatch(fixture.mWorld, fixture.mCoordinator->Registry(),
        fixture.mCoordinator->Cache(), fixture.mScene, fixture.mOuterRoot,
        mir::PrefabPatchSelection{ child, mir::PrefabPatchKind::RemovedComponent,
            "PrefabCoordinatorTestData", {} }));
    CHECK(DataOps().Has(fixture.mWorld, fixture.Member(fixture.mOuterRoot, child)));
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mOuterRoot, child), DataOps(), "amount") == 20.0f);
}

TEST_CASE("A component an outer occurrence adds is inherited by the scene member", "[prefab][coordinator][nesting]")
{
    NestedFixture fixture;
    const SceneNodeAddress child{ 3, 2 };

    PrefabDocument edited = fixture.mOuter;
    PrefabNode *occurrence = FindNode(edited, 3);
    REQUIRE(occurrence != nullptr);
    occurrence->mPatches.front().mAddedComponents.push_back(
        PrefabComponentInstance{ "PrefabCoordinatorTestExtra", json::object() });
    REQUIRE(fixture.mCoordinator->SetDocument(edited));
    fixture.Commit();

    const mir::PrefabRegistry &registry = fixture.mCoordinator->Registry();
    REQUIRE(ExtraOps().Has(fixture.mWorld, fixture.Member(fixture.mOuterRoot, child)));
    CHECK(mir::InstanceComponentState(registry, fixture.mScene, fixture.mOuterRoot, child,
        "PrefabCoordinatorTestExtra") == mir::PrefabComponentState::Inherited);
    CHECK_FALSE(AddInstanceComponent(fixture.mWorld, fixture.mScene, registry, fixture.mOuterRoot, child,
        "PrefabCoordinatorTestExtra"));

    REQUIRE(RemoveInstanceComponent(fixture.mWorld, fixture.mScene, registry, fixture.mOuterRoot, child,
        "PrefabCoordinatorTestExtra"));
    CHECK_FALSE(ExtraOps().Has(fixture.mWorld, fixture.Member(fixture.mOuterRoot, child)));
    CHECK(mir::InstanceComponentState(registry, fixture.mScene, fixture.mOuterRoot, child,
        "PrefabCoordinatorTestExtra") == mir::PrefabComponentState::Removed);
}

TEST_CASE("An outer prefab compiles once its missing inner prefab arrives", "[prefab][coordinator][nesting]")
{
    mir::RegisterCoreComponents();
    DataOps();

    World world;
    world.EmplaceResource<mir::EngineModeState>();
    world.EmplaceResource<ComponentFieldListeners>();
    PrefabCoordinator &coordinator = world.EmplaceResource<PrefabCoordinator>();
    CommandBuffer commands;
    SystemContext context{ world, commands };
    PrefabSystem system;

    const PrefabDocument inner = NestedInnerDocument();
    const PrefabDocument outer = NestedOuterDocument(inner.mId);
    REQUIRE(coordinator.SetDocument(outer));
    system.OnUpdate(context);
    CHECK(coordinator.Cache().Find(outer.mId) == nullptr);

    REQUIRE(coordinator.SetDocument(inner));
    system.OnUpdate(context);
    CHECK(coordinator.Cache().Find(outer.mId) != nullptr);
}

TEST_CASE("Changing an inner prefab refreshes dependent instances in one commit", "[prefab][coordinator][nesting]")
{
    NestedFixture fixture;
    const SceneNodeAddress child{ 2, 2 };
    const Entity outerChild = fixture.Member(fixture.mOuterRoot, child);
    const Entity outerRoot = fixture.Member(fixture.mOuterRoot, SceneNodeAddress{ 2, 1 });
    const Entity innerChild = fixture.Member(fixture.mInnerRoot, SceneNodeAddress{ 2 });
    const StableId binding = fixture.Binding(fixture.mOuterRoot, child);

    PrefabDocument edited = fixture.mInner;
    PrefabNode *childNode = FindNode(edited, 2);
    REQUIRE(childNode != nullptr);
    childNode->mComponents.front().mFields["bonus"] = 9.0;
    REQUIRE(fixture.mCoordinator->SetDocument(edited));

    fixture.Commit();

    CHECK(FloatField(fixture.mWorld, outerChild, DataOps(), "bonus") == 9.0f);
    CHECK(FloatField(fixture.mWorld, innerChild, DataOps(), "bonus") == 9.0f);
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mOuterRoot, SceneNodeAddress{ 3, 2 }), DataOps(),
        "bonus") == 9.0f);
    CHECK(fixture.Member(fixture.mOuterRoot, child) == outerChild);
    CHECK(fixture.Member(fixture.mOuterRoot, SceneNodeAddress{ 2, 1 }) == outerRoot);
    CHECK(fixture.Binding(fixture.mOuterRoot, child) == binding);
    CHECK_FALSE(fixture.mCoordinator->HasPendingWork());
}

TEST_CASE("A nested occurrence root Transform is template content", "[prefab][coordinator][nesting]")
{
    NestedFixture fixture;
    const Entity nestedRoot = fixture.Member(fixture.mOuterRoot, SceneNodeAddress{ 2, 1 });
    const Entity directRoot = fixture.Member(fixture.mInnerRoot, SceneNodeAddress{ 1 });
    REQUIRE(PositionX(fixture.mWorld, nestedRoot) == 0.0);
    REQUIRE(PositionX(fixture.mWorld, directRoot) == 0.0);

    PrefabDocument edited = fixture.mInner;
    PrefabNode *root = FindNode(edited, 1);
    REQUIRE(root != nullptr);
    for (PrefabComponentInstance &component : root->mComponents)
    {
        if (component.mType == "Transform")
            component.mFields["position"] = json::array({ 5.0, 0.0, 0.0 });
    }
    REQUIRE(fixture.mCoordinator->SetDocument(edited));

    fixture.Commit();

    CHECK(PositionX(fixture.mWorld, nestedRoot) == 5.0);
    CHECK(PositionX(fixture.mWorld, directRoot) == 0.0);
}

TEST_CASE("Deleting an inner node clears nested references permanently", "[prefab][coordinator][nesting]")
{
    NestedFixture fixture;
    const SceneNodeAddress innerRoot{ 2, 1 };
    const SceneNodeAddress innerChild{ 2, 2 };
    const StableId removed = fixture.Binding(fixture.mOuterRoot, innerChild);
    const Entity removedEntity = fixture.Member(fixture.mOuterRoot, innerChild);

    REQUIRE(SetInstanceEntityRef(fixture.mWorld, fixture.mScene, fixture.mOuterRoot, innerRoot,
        "PrefabCoordinatorTestData", "target", removed));

    const Entity ordinary = CreateSceneEntity(fixture.mWorld, fixture.mScene);
    DataOps().AddDefault(fixture.mWorld, ordinary);
    SetEntityField(fixture.mWorld, ordinary, DataOps(), "target", removedEntity);

    PrefabDocument edited = fixture.mInner;
    PrefabNode *editedRoot = FindNode(edited, 1);
    REQUIRE(editedRoot != nullptr);
    editedRoot->mComponents.front().mFields["target"] = nullptr;
    std::erase_if(edited.mNodes, [](const PrefabNode &node) { return node.mId == 2; });
    REQUIRE(fixture.mCoordinator->SetDocument(edited));
    fixture.mScene.mDirty = false;

    fixture.Commit();

    CHECK_FALSE(fixture.mWorld.IsAlive(removedEntity));
    CHECK_FALSE(fixture.mScene.mInstances.at(fixture.mOuterRoot).mBindings.contains(innerChild));
    CHECK(EntityField(fixture.mWorld, fixture.Member(fixture.mOuterRoot, innerRoot), DataOps(), "target").IsNull());
    CHECK(EntityField(fixture.mWorld, ordinary, DataOps(), "target").IsNull());
    const mir::SceneFieldPatch *cleared = FindInstanceFieldPatch(fixture.mScene.mInstances.at(fixture.mOuterRoot),
        innerRoot, "PrefabCoordinatorTestData", "target");
    REQUIRE(cleared != nullptr);
    CHECK(cleared->mValue.is_null());
    CHECK(fixture.mCoordinator->ConsumeSceneMaintenance());

    PrefabNode restored;
    restored.mId = 2;
    restored.mParent = 1;
    restored.mComponents.push_back(PrefabComponentInstance{
        "PrefabCoordinatorTestData", json{ { "amount", 3.0 }, { "bonus", 4.0 }, { "target", nullptr } } });
    edited.mNodes.push_back(std::move(restored));
    REQUIRE(fixture.mCoordinator->SetDocument(edited));
    fixture.Commit();

    const StableId restoredId = fixture.Binding(fixture.mOuterRoot, innerChild);
    CHECK(restoredId != removed);
    CHECK(EntityField(fixture.mWorld, fixture.Member(fixture.mOuterRoot, innerRoot), DataOps(), "target").IsNull());

    TempDir dir;
    REQUIRE(mir::SaveScene(fixture.mWorld, dir.mPath, fixture.mScene, &fixture.mCoordinator->Resolver()));
    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, &fixture.mCoordinator->Resolver());
    CHECK(EntityField(loadedWorld, loaded.mEntities.at(fixture.Binding(fixture.mOuterRoot, innerRoot)),
        DataOps(), "target").IsNull());
    CHECK(loaded.mInstances.at(fixture.mOuterRoot).mBindings.at(innerChild) == restoredId);
}

TEST_CASE("A nested scene instance saves and loads bindings and patches unchanged", "[prefab][coordinator][nesting]")
{
    NestedFixture fixture;
    const SceneNodeAddress innerRoot{ 2, 1 };
    const SceneNodeAddress innerChild{ 2, 2 };
    const SceneNodeAddress otherChild{ 3, 2 };

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mOuterRoot, innerChild, "PrefabCoordinatorTestData", "amount", 45.0));
    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mOuterRoot, innerRoot, "PrefabCoordinatorTestData", "target",
        fixture.Binding(fixture.mOuterRoot, otherChild)));

    const ScenePrefabInstance &before = fixture.mScene.mInstances.at(fixture.mOuterRoot);
    const std::map<SceneNodeAddress, StableId> bindings = before.mBindings;
    const std::size_t patchCount = before.mPatches.size();

    TempDir dir;
    REQUIRE(mir::SaveScene(fixture.mWorld, dir.mPath, fixture.mScene, &fixture.mCoordinator->Resolver()));

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, &fixture.mCoordinator->Resolver());
    const ScenePrefabInstance &after = loaded.mInstances.at(fixture.mOuterRoot);

    CHECK(after.mBindings == bindings);
    CHECK(after.mPatches.size() == patchCount);
    CHECK(mir::InstanceFieldState(loaded, fixture.mOuterRoot, innerChild,
        "PrefabCoordinatorTestData", "amount") == mir::PrefabFieldState::Overridden);
    CHECK(mir::InstanceFieldState(loaded, fixture.mOuterRoot, innerRoot,
        "PrefabCoordinatorTestData", "target") == mir::PrefabFieldState::Overridden);

    const mir::SceneFieldPatch *amount = FindInstanceFieldPatch(after, innerChild,
        "PrefabCoordinatorTestData", "amount");
    REQUIRE(amount != nullptr);
    CHECK(amount->mValue == 45.0);

    const StableId otherId = after.mBindings.at(otherChild);
    const mir::SceneFieldPatch *target = FindInstanceFieldPatch(after, innerRoot,
        "PrefabCoordinatorTestData", "target");
    REQUIRE(target != nullptr);
    CHECK(target->mValue == json(otherId));
    CHECK(EntityField(loadedWorld, loaded.mEntities.at(bindings.at(innerRoot)), DataOps(), "target") ==
        loaded.mEntities.at(otherId));

    TempDir second;
    REQUIRE(mir::SaveScene(loadedWorld, second.mPath, loaded, &fixture.mCoordinator->Resolver()));
    CHECK(SnapshotFiles(dir.mPath) == SnapshotFiles(second.mPath));
}

TEST_CASE("Nested occurrence members refuse to apply or edit through the outer document", "[prefab][coordinator][nesting]")
{
    NestedFixture fixture;
    mir::PrefabAuthoring authoring(fixture.mWorld, *fixture.mCoordinator);

    mir::PrefabAuthoredKey sceneKey;
    sceneKey.mAddress = { fixture.mOuterRoot, 2, 2 };
    sceneKey.mComponent = "PrefabCoordinatorTestData";
    sceneKey.mField = "amount";

    const mir::PrefabAuthoredAction apply = authoring.Apply(sceneKey);
    CHECK_FALSE(apply.mRefusal.empty());
    CHECK(apply.mEntries.empty());

    mir::PrefabAuthoredKey documentKey;
    documentKey.mPrefab = fixture.mInner.mId;
    documentKey.mAddress = { 2, 2 };
    documentKey.mComponent = "PrefabCoordinatorTestData";
    documentKey.mField = "amount";

    const mir::PrefabAuthoredEntry entry{ documentKey, std::string("5.0") };
    const mir::PrefabAuthoredWrite write = authoring.Write(std::span<const mir::PrefabAuthoredEntry>(&entry, 1));
    CHECK_FALSE(write.mApplied);
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mOuterRoot, SceneNodeAddress{ 2, 2 }), DataOps(),
        "amount") == 20.0f);

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mOuterRoot, SceneNodeAddress{ 2, 2 }, "PrefabCoordinatorTestData", "amount", 55.0));
    const mir::PrefabAuthoredAction revert = authoring.Revert(sceneKey);
    CHECK(revert.mRefusal.empty());
    REQUIRE(revert.mEntries.size() == 1);
    const mir::PrefabAuthoredWrite reverted =
        authoring.Write(std::span<const mir::PrefabAuthoredEntry>(revert.mEntries));
    CHECK(reverted.mApplied);
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mOuterRoot, SceneNodeAddress{ 2, 2 }), DataOps(),
        "amount") == 20.0f);
}

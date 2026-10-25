/**
 * @file PrefabCoordinator.tests.cpp
 * @author Jackey Cen
 * @brief Tests for prefab/PrefabCoordinator.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/PrefabCoordinator.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/EntityChanges.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <prefab/PrefabFormat.h>
#include <prefab/PrefabOverrides.h>
#include <prefab/PrefabSceneInstance.h>
#include <scene/SceneInstancePatch.h>
#include <scene/SceneIO.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
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
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::CreateSceneEntity;
    using mir::EngineMode;
    using mir::Entity;
    using mir::EntityChangeListeners;
    using mir::EntityChangeSet;
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
    using mir::PrefabNode;
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
            mWorld.EmplaceResource<EntityChangeListeners>();
            mCoordinator = &mWorld.EmplaceResource<PrefabCoordinator>();

            mDocument = Document();
            REQUIRE(mCoordinator->Registry().SetDocument(mDocument));
            REQUIRE_FALSE(HasPrefabErrors(mCoordinator->Cache().Compile(mDocument)));

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
    struct Recorder
    {
        std::vector<Entity> mRemoved;
    };

    static Recorder recorder;
    recorder = Recorder{};
    const auto before = [](World &, const EntityChangeSet &changes, void *user)
        {
            auto &state = *static_cast<Recorder *>(user);
            for (Entity entity : changes.mRemovedEntities)
                state.mRemoved.push_back(entity);
        };

    Fixture fixture;
    fixture.mWorld.Resource<EntityChangeListeners>().AddBeforeRemove(+before, &recorder);

    const StableId removed = fixture.Binding(fixture.mRoot, SceneNodeAddress{ 3 });
    const Entity member = fixture.mScene.mEntities.at(removed);
    const Entity child = CreateSceneEntity(fixture.mWorld, fixture.mScene, member);

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
    CHECK(std::find(recorder.mRemoved.begin(), recorder.mRemoved.end(), child) != recorder.mRemoved.end());
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
        EntityChangeSet mBefore;
        EntityChangeSet mAfter;
    };

    static Recorder recorder;
    recorder = Recorder{};

    const auto beforeFirst = [](World &, const EntityChangeSet &, void *user)
        {
            static_cast<Recorder *>(user)->mCalls.push_back("before1");
        };
    const auto beforeSecond = [](World &world, const EntityChangeSet &changes, void *user)
        {
            auto &state = *static_cast<Recorder *>(user);
            state.mCalls.push_back("before2");
            state.mBefore = changes;
            for (const mir::EntityComponentChange &change : changes.mChangedComponents)
            {
                if (!(change.mType == DataOps().mType))
                    continue;
                float value = 0.0f;
                if (const FieldDesc *field = DataOps().FindField("amount"))
                    field->Read(DataOps().GetComponent(world, change.mEntity), &value);
                state.mValues.push_back(value);
            }
        };
    const auto afterFirst = [](World &, const EntityChangeSet &, void *user)
        {
            static_cast<Recorder *>(user)->mCalls.push_back("after1");
        };
    const auto afterSecond = [](World &world, const EntityChangeSet &changes, void *user)
        {
            auto &state = *static_cast<Recorder *>(user);
            state.mCalls.push_back("after2");
            state.mAfter = changes;
            for (const mir::EntityComponentChange &change : changes.mChangedComponents)
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
    EntityChangeListeners &listeners = fixture.mWorld.Resource<EntityChangeListeners>();
    listeners.AddBeforeRemove(+beforeFirst, &recorder);
    listeners.AddBeforeRemove(+beforeSecond, &recorder);
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

    REQUIRE_FALSE(recorder.mBefore.mChangedComponents.empty());
    REQUIRE_FALSE(recorder.mAfter.mChangedComponents.empty());

    CHECK(world.GetComponent<CoordRuntimeComponent>(second)->mRuntimeOnly == 42.0f);
    CHECK(world.GetComponent<CoordRuntimeComponent>(second)->mAuthored == 23.0f);
}

TEST_CASE("Component additions and removals reach the change set", "[prefab][coordinator]")
{
    struct Recorder
    {
        EntityChangeSet mAfter;
    };

    static Recorder recorder;
    recorder = Recorder{};

    const auto after = [](World &, const EntityChangeSet &changes, void *user)
        {
            static_cast<Recorder *>(user)->mAfter = changes;
        };

    Fixture fixture;
    fixture.mWorld.Resource<EntityChangeListeners>().AddAfterChange(after, &recorder);

    const Entity second = fixture.Member(fixture.mRoot, SceneNodeAddress{ 2 });
    const Entity third = fixture.Member(fixture.mRoot, SceneNodeAddress{ 3 });

    PrefabNode *secondNode = FindNode(fixture.mDocument, 2);
    REQUIRE(secondNode != nullptr);
    std::erase_if(secondNode->mComponents, [](const PrefabComponentInstance &component)
        {
            return component.mType == "PrefabCoordinatorTestData";
        });

    PrefabNode *thirdNode = FindNode(fixture.mDocument, 3);
    REQUIRE(thirdNode != nullptr);
    thirdNode->mComponents.push_back(PrefabComponentInstance{
        "PrefabCoordinatorTestExtra", json{ { "value", 5.0 }, { "link", nullptr } } });

    REQUIRE(fixture.mCoordinator->SetDocument(fixture.mDocument));
    fixture.Commit();

    bool removedData = false;
    for (const mir::EntityComponentRef &entry : recorder.mAfter.mRemovedComponents)
        removedData = removedData || (entry.mEntity == second && entry.mType == DataOps().mType);

    bool addedExtra = false;
    for (const mir::EntityComponentRef &entry : recorder.mAfter.mAddedComponents)
        addedExtra = addedExtra || (entry.mEntity == third && entry.mType == ExtraOps().mType);

    CHECK(removedData);
    CHECK(addedExtra);
    CHECK_FALSE(DataOps().Has(fixture.mWorld, second));
    CHECK(ExtraOps().Has(fixture.mWorld, third));
}

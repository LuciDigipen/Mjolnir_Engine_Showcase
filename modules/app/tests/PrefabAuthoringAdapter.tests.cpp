/**
 * @file PrefabAuthoringAdapter.tests.cpp
 * @author Jackey Cen
 * @brief Tests for app/PrefabAuthoringAdapter.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "../src/app/PrefabAuthoringAdapter.h"

#include <editor/history/AuthoredEdit.h>
#include <editor/history/Authoring.h>
#include <editor/history/History.h>
#include <editor/history/SceneOps.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/TypeId.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <core/fs/FileIO.h>
#include <prefab/PrefabCoordinator.h>
#include <prefab/PrefabFormat.h>
#include <prefab/PrefabSceneInstance.h>
#include <scene/EntitySnapshot.h>
#include <scene/SceneAsset.h>
#include <scene/SceneDuplicate.h>
#include <scene/SceneInstancePatch.h>
#include <scene/SceneIO.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec3.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
    using nlohmann::json;
    using mir::AuthoredAction;
    using mir::AuthoredComponentState;
    using mir::AuthoredEntry;
    using mir::AuthoredFieldState;
    using mir::AuthoredInstanceRole;
    using mir::AuthoredKey;
    using mir::CapturedSubtree;
    using mir::CommandBuffer;
    using mir::CompiledPrefab;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::CreateSceneEntity;
    using mir::DocumentId;
    using mir::DuplicateSceneEntitySubtree;
    using mir::Entity;
    using mir::FieldDesc;
    using mir::FieldKind;
    using mir::FieldValue;
    using mir::FindInstanceFieldPatch;
    using mir::HasPrefabErrors;
    using mir::History;
    using mir::kNullEntity;
    using mir::kNullStableId;
    using mir::kSceneDocument;
    using mir::LoadedScene;
    using mir::MakeAuthoredCommand;
    using mir::NewScene;
    using mir::PlacePrefabInstance;
    using mir::PrefabAuthoringAdapter;
    using mir::PrefabComponentInstance;
    using mir::PrefabCoordinator;
    using mir::PrefabDocument;
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
        return ComponentRegistry::Instance().RegisterRuntime("AppAuthoringTestData", fields);
    }

    const ComponentOps &ExtraOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "value", FieldKind::Float },
        };
        return ComponentRegistry::Instance().RegisterRuntime("AppAuthoringTestExtra", fields);
    }

    const ComponentOps &RefOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "link", FieldKind::EntityRef }, { "outside", FieldKind::EntityRef },
        };
        return ComponentRegistry::Instance().RegisterRuntime("AppAuthoringTestRefs", fields);
    }

    class NoSceneOps final : public mir::ISceneOps
    {
    public:
        Entity CreateEntity() override { return kNullEntity; }
        Entity Duplicate(Entity) override { return kNullEntity; }
        Entity CopyAsOrdinaryObject(Entity) override { return kNullEntity; }
        CapturedSubtree Capture(Entity) override { return {}; }
        std::vector<Entity> Restore(const CapturedSubtree &, Entity, Entity) override { return {}; }
        bool Destroy(Entity) override { return false; }
    };

    PrefabDocument Document()
    {
        PrefabDocument document = mir::NewPrefabDocument();

        document.mNodes[0].mComponents.push_back(PrefabComponentInstance{
            "AppAuthoringTestData",
            json{ { "amount", 2.0 }, { "bonus", 3.0 }, { "target", json::array({ "2" }) } } });
        document.mNodes[0].mComponents.push_back(PrefabComponentInstance{ "Transform", json::object() });

        PrefabNode second;
        second.mId = 2;
        second.mParent = 1;
        second.mComponents.push_back(PrefabComponentInstance{
            "AppAuthoringTestData",
            json{ { "amount", 9.0 }, { "bonus", 4.0 }, { "target", nullptr } } });
        second.mComponents.push_back(PrefabComponentInstance{ "Transform", json::object() });

        PrefabNode third;
        third.mId = 3;
        third.mParent = 1;
        third.mComponents.push_back(PrefabComponentInstance{
            "AppAuthoringTestData",
            json{ { "amount", 5.0 }, { "bonus", 6.0 }, { "target", nullptr } } });

        PrefabNode fourth;
        fourth.mId = 4;
        fourth.mParent = 3;
        fourth.mComponents.push_back(PrefabComponentInstance{
            "AppAuthoringTestData",
            json{ { "amount", 7.0 }, { "bonus", 8.0 }, { "target", nullptr } } });

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

    Entity EntityField(World &world, Entity entity, const ComponentOps &ops, std::string_view field)
    {
        void *component = ops.GetComponent(world, entity);
        REQUIRE(component != nullptr);
        const FieldDesc *desc = ops.FindField(field);
        REQUIRE(desc != nullptr);
        Entity value = kNullEntity;
        desc->Read(component, &value);
        return value;
    }

    const PrefabComponentInstance *TemplateComponent(const PrefabDocument *document, mir::PrefabNodeId node,
        std::string_view type)
    {
        if (document == nullptr)
            return nullptr;
        for (const PrefabNode &candidate : document->mNodes)
        {
            if (candidate.mId != node)
                continue;
            for (const PrefabComponentInstance &component : candidate.mComponents)
            {
                if (component.mType == type)
                    return &component;
            }
        }
        return nullptr;
    }

    FieldValue ReadField(World &world, Entity entity, const ComponentOps &ops, uint32_t field)
    {
        FieldValue value;
        value.mField = field;
        ops.mFields[field].Read(ops.GetComponent(world, entity), value.mBytes);
        return value;
    }

    struct Fixture
    {
        World mWorld;
        LoadedScene mScene;
        PrefabCoordinator *mCoordinator = nullptr;
        std::unique_ptr<PrefabAuthoringAdapter> mAdapter;
        NoSceneOps mSceneOps;
        History mHistory;
        CommandBuffer mCommands;
        PrefabDocument mDocument;
        StableId mRoot = kNullStableId;
        DocumentId mPrefabDocument;

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

            mAdapter = std::make_unique<PrefabAuthoringAdapter>(mWorld, *mCoordinator);
            mHistory.SetAuthoringBackend(mAdapter.get());
            mPrefabDocument = DocumentId{ mDocument.mId.mHigh, mDocument.mId.mLow };
        }

        StableId Binding(const SceneNodeAddress &address) const
        {
            return mScene.mInstances.at(mRoot).mBindings.at(address);
        }

        Entity Member(const SceneNodeAddress &address) const
        {
            return mScene.mEntities.at(Binding(address));
        }

        void Flush() { mHistory.Flush(mWorld, mSceneOps); }

        void Run(std::unique_ptr<mir::ICommand> command)
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

        void Commit()
        {
            PrefabSystem system;
            SystemContext context{ mWorld, mCommands };
            system.OnUpdate(context);
        }

        // what App::Run does after the scheduler update
        void Bridge()
        {
            if (mCoordinator->ConsumeSceneMaintenance() && !mHistory.IsUnsaved(kSceneDocument))
                mHistory.SetUnsaved(kSceneDocument, true);
        }

        void Save(DocumentId document) { mHistory.SetUnsaved(document, false); }
    };

    AuthoredKey SceneEditKey(Fixture &fixture, const SceneNodeAddress &node, std::string component,
        std::string field)
    {
        const Entity entity = fixture.Member(node);
        const std::optional<AuthoredKey> key = fixture.mAdapter->KeyOf(entity, component, field);
        REQUIRE(key.has_value());
        return *key;
    }

    std::string FloatJson(float value) { return nlohmann::json(value).dump(); }

    StableId StableIdIn(const LoadedScene &scene, Entity entity)
    {
        for (const auto &[id, candidate] : scene.mEntities)
        {
            if (candidate == entity)
                return id;
        }
        return kNullStableId;
    }

    struct TempDir
    {
        std::filesystem::path mPath = std::filesystem::temp_directory_path() /
            ("mir_app_authoring_" + mir::ToString(mir::GeneratePrefabId()));

        TempDir() { std::filesystem::create_directories(mPath); }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(mPath, ec);
        }

        TempDir(const TempDir &) = delete;
        TempDir &operator=(const TempDir &) = delete;
    };

    bool SaveScene(Fixture &fixture, const std::filesystem::path &path)
    {
        const bool prefabsSaved = fixture.mAdapter->SaveScenePrefabs(fixture.mScene,
            [&](mir::PrefabId id)
            {
                return fixture.mHistory.IsUnsaved(DocumentId{ id.mHigh, id.mLow });
            },
            [&](mir::PrefabId id)
            {
                fixture.mHistory.SetUnsaved(DocumentId{ id.mHigh, id.mLow }, false);
            });
        if (!prefabsSaved || !mir::SaveScene(fixture.mWorld, path, fixture.mScene, &fixture.mCoordinator->Resolver()))
            return false;
        fixture.mScene.mDirty = false;
        fixture.Save(kSceneDocument);
        return true;
    }
}

TEST_CASE("Apply survives Save Scene and reload without saving implicitly", "[app][authoring][save]")
{
    TempDir dir;
    Fixture fixture;
    const std::filesystem::path prefabsDir = dir.mPath / "prefabs";
    const std::filesystem::path prefabPath = prefabsDir / "test.prefab";
    const std::filesystem::path sceneDir = dir.mPath / "scene";
    std::filesystem::create_directories(prefabsDir);
    REQUIRE(mir::WritePrefabFile(prefabPath, fixture.mDocument));
    REQUIRE_FALSE(HasPrefabErrors(fixture.mCoordinator->Registry().Scan(prefabsDir)));
    const auto sourceBefore = mir::ReadText(prefabPath);

    const SceneNodeAddress second{ 2 };
    const AuthoredKey amount = SceneEditKey(fixture, second, "AppAuthoringTestData", "amount");
    fixture.Run(MakeAuthoredCommand("Override", { AuthoredEntry{ amount, std::string("12.0") } }));
    const AuthoredAction apply = fixture.mAdapter->Apply(amount);
    REQUIRE(apply.mAllowed);
    fixture.Run(MakeAuthoredCommand(apply.mName, apply.mEntries));
    CHECK(mir::ReadText(prefabPath) == sourceBefore);
    REQUIRE(fixture.mHistory.IsUnsaved(fixture.mPrefabDocument));
    REQUIRE(fixture.mHistory.IsUnsaved(kSceneDocument));

    REQUIRE(SaveScene(fixture, sceneDir));
    CHECK_FALSE(fixture.mHistory.IsUnsaved(fixture.mPrefabDocument));
    CHECK_FALSE(fixture.mHistory.IsUnsaved(kSceneDocument));
    CHECK(fixture.mHistory.StepCount() == 2);

    World loadedWorld;
    PrefabCoordinator loadedCoordinator;
    REQUIRE_FALSE(HasPrefabErrors(loadedCoordinator.Registry().Scan(prefabsDir)));
    const LoadedScene loaded = mir::LoadScene(loadedWorld, sceneDir, &loadedCoordinator.Resolver());
    const ScenePrefabInstance &instance = loaded.mInstances.at(fixture.mRoot);
    CHECK(FindInstanceFieldPatch(instance, second, "AppAuthoringTestData", "amount") == nullptr);
    const Entity member = loaded.mEntities.at(instance.mBindings.at(second));
    CHECK(FloatField(loadedWorld, member, DataOps(), "amount") == 12.0f);

    fixture.Undo();
    CHECK(fixture.mHistory.IsUnsaved(fixture.mPrefabDocument));
    CHECK(fixture.mAdapter->FieldState(amount) == AuthoredFieldState::Overridden);
    CHECK(mir::ReadText(prefabPath) != sourceBefore);
    fixture.Redo();
    CHECK_FALSE(fixture.mHistory.IsUnsaved(fixture.mPrefabDocument));
    CHECK_FALSE(fixture.mHistory.IsUnsaved(kSceneDocument));
}

TEST_CASE("A failed Prefab save preserves the saved scene and dirty state", "[app][authoring][save]")
{
    TempDir dir;
    Fixture fixture;
    const std::filesystem::path prefabsDir = dir.mPath / "prefabs";
    const std::filesystem::path prefabPath = prefabsDir / "test.prefab";
    const std::filesystem::path sceneDir = dir.mPath / "scene";
    std::filesystem::create_directories(prefabsDir);
    REQUIRE(mir::WritePrefabFile(prefabPath, fixture.mDocument));
    REQUIRE_FALSE(HasPrefabErrors(fixture.mCoordinator->Registry().Scan(prefabsDir)));
    REQUIRE(SaveScene(fixture, sceneDir));
    const auto sceneBefore = mir::ReadText(sceneDir / "scene.json");

    const AuthoredKey amount = SceneEditKey(fixture, SceneNodeAddress{ 2 }, "AppAuthoringTestData", "amount");
    fixture.Run(MakeAuthoredCommand("Override", { AuthoredEntry{ amount, std::string("12.0") } }));
    const AuthoredAction apply = fixture.mAdapter->Apply(amount);
    REQUIRE(apply.mAllowed);
    fixture.Run(MakeAuthoredCommand(apply.mName, apply.mEntries));
    std::filesystem::remove(prefabPath);
    std::filesystem::create_directory(prefabPath);

    CHECK_FALSE(SaveScene(fixture, sceneDir));
    CHECK(mir::ReadText(sceneDir / "scene.json") == sceneBefore);
    CHECK(fixture.mHistory.IsUnsaved(fixture.mPrefabDocument));
    CHECK(fixture.mHistory.IsUnsaved(kSceneDocument));
    CHECK(fixture.mScene.mDirty);
}

TEST_CASE("A field override is one History step and survives undo and redo", "[app][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress second{ 2 };
    const AuthoredKey key = SceneEditKey(fixture, second, "AppAuthoringTestData", "amount");

    const float value = 42.0f;
    const std::string encoded = fixture.mAdapter->EncodeField(FieldKind::Float, &value);
    fixture.Run(MakeAuthoredCommand("Edit amount", { AuthoredEntry{ key, encoded } }));

    CHECK(fixture.mHistory.StepCount() == 1);
    CHECK(fixture.mAdapter->FieldState(key) == AuthoredFieldState::Overridden);
    CHECK(FloatField(fixture.mWorld, fixture.Member(second), DataOps(), "amount") == 42.0f);
    CHECK(fixture.mHistory.IsUnsaved(kSceneDocument));
    CHECK_FALSE(fixture.mHistory.IsUnsaved(fixture.mPrefabDocument));

    fixture.Undo();
    CHECK(fixture.mAdapter->FieldState(key) == AuthoredFieldState::Inherited);
    CHECK(FloatField(fixture.mWorld, fixture.Member(second), DataOps(), "amount") == 9.0f);
    CHECK_FALSE(fixture.mHistory.IsUnsaved(kSceneDocument));

    fixture.Redo();
    CHECK(fixture.mAdapter->FieldState(key) == AuthoredFieldState::Overridden);
    CHECK(FloatField(fixture.mWorld, fixture.Member(second), DataOps(), "amount") == 42.0f);
}

TEST_CASE("An override equal to the template value is still an override", "[app][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress second{ 2 };
    const AuthoredKey key = SceneEditKey(fixture, second, "AppAuthoringTestData", "amount");

    const float value = 9.0f;
    fixture.Run(MakeAuthoredCommand("Edit amount",
        { AuthoredEntry{ key, fixture.mAdapter->EncodeField(FieldKind::Float, &value) } }));
    CHECK(fixture.mAdapter->FieldState(key) == AuthoredFieldState::Overridden);

    fixture.Undo();
    CHECK(fixture.mAdapter->FieldState(key) == AuthoredFieldState::Inherited);
    CHECK(FloatField(fixture.mWorld, fixture.Member(second), DataOps(), "amount") == 9.0f);
}

TEST_CASE("Component additions and removals round-trip through History", "[app][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress first{ 1 };
    const SceneNodeAddress second{ 2 };

    const std::optional<AuthoredKey> added =
        fixture.mAdapter->ComponentKeyOf(fixture.Member(first), "AppAuthoringTestExtra");
    REQUIRE(added.has_value());
    fixture.Run(MakeAuthoredCommand("Add component",
        { AuthoredEntry{ *added, std::string("{\"value\":4.5}") } }));

    CHECK(fixture.mAdapter->ComponentState(fixture.Member(first), "AppAuthoringTestExtra") ==
        AuthoredComponentState::Added);
    CHECK(ExtraOps().Has(fixture.mWorld, fixture.Member(first)));
    CHECK(FloatField(fixture.mWorld, fixture.Member(first), ExtraOps(), "value") == 4.5f);

    fixture.Undo();
    CHECK(fixture.mAdapter->ComponentState(fixture.Member(first), "AppAuthoringTestExtra") ==
        AuthoredComponentState::Absent);
    CHECK_FALSE(ExtraOps().Has(fixture.mWorld, fixture.Member(first)));

    fixture.Redo();
    CHECK(ExtraOps().Has(fixture.mWorld, fixture.Member(first)));

    const std::optional<AuthoredKey> removed =
        fixture.mAdapter->ComponentKeyOf(fixture.Member(second), "AppAuthoringTestData");
    REQUIRE(removed.has_value());
    fixture.Run(MakeAuthoredCommand("Remove component",
        { AuthoredEntry{ *removed, std::string("null") } }));
    CHECK(fixture.mAdapter->ComponentState(fixture.Member(second), "AppAuthoringTestData") ==
        AuthoredComponentState::Removed);
    CHECK_FALSE(DataOps().Has(fixture.mWorld, fixture.Member(second)));
    CHECK(fixture.mAdapter->RemovedComponents(fixture.Member(second)) ==
        std::vector<std::string>{ "AppAuthoringTestData" });

    fixture.Undo();
    CHECK(fixture.mAdapter->ComponentState(fixture.Member(second), "AppAuthoringTestData") ==
        AuthoredComponentState::Inherited);
    CHECK(FloatField(fixture.mWorld, fixture.Member(second), DataOps(), "amount") == 9.0f);
}

TEST_CASE("Undo of removing an inherited component restores its field overrides", "[app][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress second{ 2 };
    const AuthoredKey amount = SceneEditKey(fixture, second, "AppAuthoringTestData", "amount");
    const float value = 99.0f;
    fixture.Run(MakeAuthoredCommand("Edit amount",
        { AuthoredEntry{ amount, fixture.mAdapter->EncodeField(FieldKind::Float, &value) } }));

    fixture.Run(mir::MakeRemoveComponentEditCommand(fixture.Member(second), DataOps(), fixture.mAdapter.get()));
    REQUIRE(fixture.mAdapter->ComponentState(fixture.Member(second), "AppAuthoringTestData") ==
        AuthoredComponentState::Removed);
    CHECK(fixture.mHistory.StepCount() == 2);

    fixture.Undo();
    CHECK(fixture.mAdapter->ComponentState(fixture.Member(second), "AppAuthoringTestData") ==
        AuthoredComponentState::Inherited);
    CHECK(fixture.mAdapter->FieldState(amount) == AuthoredFieldState::Overridden);
    CHECK(FloatField(fixture.mWorld, fixture.Member(second), DataOps(), "amount") == 99.0f);

    fixture.Redo();
    CHECK_FALSE(DataOps().Has(fixture.mWorld, fixture.Member(second)));

    fixture.Undo();
    CHECK(FloatField(fixture.mWorld, fixture.Member(second), DataOps(), "amount") == 99.0f);
}

TEST_CASE("Apply writes both documents as one step and undo restores both", "[app][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress first{ 1 };
    const SceneNodeAddress third{ 3 };
    const AuthoredKey amount = SceneEditKey(fixture, first, "AppAuthoringTestData", "amount");
    const AuthoredKey target = SceneEditKey(fixture, first, "AppAuthoringTestData", "target");

    const float value = 12.0f;
    fixture.Run(MakeAuthoredCommand("Edit amount",
        { AuthoredEntry{ amount, fixture.mAdapter->EncodeField(FieldKind::Float, &value) } }));
    fixture.Run(MakeAuthoredCommand("Edit target",
        { AuthoredEntry{ target, fixture.mAdapter->EncodeField(FieldKind::EntityRef,
              &fixture.mScene.mEntities.at(fixture.Binding(third))) } }));

    fixture.Save(kSceneDocument);
    fixture.Save(fixture.mPrefabDocument);

    const AuthoredAction applied = fixture.mAdapter->Apply(amount);
    REQUIRE(applied.mAllowed);
    REQUIRE(applied.mEntries.size() == 2);
    fixture.Run(MakeAuthoredCommand(applied.mName, applied.mEntries));

    CHECK(fixture.mHistory.StepCount() == 3);
    const PrefabDocument *document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    CHECK(document->mNodes.front().mComponents.front().mFields.at("amount") == 12.0);
    CHECK(fixture.mAdapter->FieldState(amount) == AuthoredFieldState::Inherited);
    CHECK(fixture.mAdapter->FieldState(target) == AuthoredFieldState::Overridden);
    CHECK(fixture.mHistory.IsUnsaved(fixture.mPrefabDocument));
    CHECK(fixture.mHistory.IsUnsaved(kSceneDocument));

    fixture.Undo();
    document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    CHECK(document->mNodes.front().mComponents.front().mFields.at("amount") == 2.0);
    CHECK(fixture.mAdapter->FieldState(amount) == AuthoredFieldState::Overridden);
    CHECK(FloatField(fixture.mWorld, fixture.Member(first), DataOps(), "amount") == 12.0f);

    fixture.Redo();
    document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    CHECK(document->mNodes.front().mComponents.front().mFields.at("amount") == 12.0);
    CHECK(fixture.mAdapter->FieldState(amount) == AuthoredFieldState::Inherited);

    const Entity ordinary = CreateSceneEntity(fixture.mWorld, fixture.mScene);
    DataOps().AddDefault(fixture.mWorld, ordinary);
    const StableId ordinaryId = fixture.mScene.mNextId - 1;
    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mCoordinator->Registry(),
        fixture.mRoot, SceneNodeAddress{ 2 }, "AppAuthoringTestData", "target", ordinaryId));
    const AuthoredAction refused =
        fixture.mAdapter->Apply(SceneEditKey(fixture, SceneNodeAddress{ 2 }, "AppAuthoringTestData", "target"));
    CHECK_FALSE(refused.mAllowed);
    CHECK_FALSE(refused.mReason.empty());
}

TEST_CASE("Undo of applying an added component restores the scene override", "[app][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress first{ 1 };
    const std::optional<AuthoredKey> added =
        fixture.mAdapter->ComponentKeyOf(fixture.Member(first), "AppAuthoringTestExtra");
    REQUIRE(added.has_value());

    fixture.Run(MakeAuthoredCommand("Add component",
        { AuthoredEntry{ *added, std::string("{\"value\":4.5}") } }));
    REQUIRE(fixture.mAdapter->ComponentState(fixture.Member(first), "AppAuthoringTestExtra") ==
        AuthoredComponentState::Added);

    const AuthoredAction applied = fixture.mAdapter->Apply(*added);
    REQUIRE(applied.mAllowed);
    fixture.Run(MakeAuthoredCommand(applied.mName, applied.mEntries));
    CHECK(fixture.mHistory.StepCount() == 2);

    const PrefabDocument *document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(TemplateComponent(document, 1, "AppAuthoringTestExtra") != nullptr);
    CHECK(TemplateComponent(document, 1, "AppAuthoringTestExtra")->mFields.at("value") == 4.5);
    CHECK(fixture.mAdapter->ComponentState(fixture.Member(first), "AppAuthoringTestExtra") ==
        AuthoredComponentState::Inherited);

    fixture.Commit();
    REQUIRE(ExtraOps().Has(fixture.mWorld, fixture.Member(first)));
    CHECK(FloatField(fixture.mWorld, fixture.Member(first), ExtraOps(), "value") == 4.5f);

    fixture.Undo();
    CHECK(fixture.mHistory.StepCount() == 2);
    document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    CHECK(TemplateComponent(document, 1, "AppAuthoringTestExtra") == nullptr);
    CHECK(fixture.mAdapter->ComponentState(fixture.Member(first), "AppAuthoringTestExtra") ==
        AuthoredComponentState::Added);
    const mir::SceneComponentRecord *restored = mir::FindAddedComponent(fixture.mScene.mInstances.at(fixture.mRoot),
        first, "AppAuthoringTestExtra");
    REQUIRE(restored != nullptr);
    CHECK(restored->mFields.at("value") == 4.5);
    CHECK(FloatField(fixture.mWorld, fixture.Member(first), ExtraOps(), "value") == 4.5f);

    fixture.Commit();
    REQUIRE(ExtraOps().Has(fixture.mWorld, fixture.Member(first)));
    CHECK(FloatField(fixture.mWorld, fixture.Member(first), ExtraOps(), "value") == 4.5f);

    fixture.Redo();
    CHECK(fixture.mHistory.StepCount() == 2);
    document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    REQUIRE(TemplateComponent(document, 1, "AppAuthoringTestExtra") != nullptr);
    CHECK(TemplateComponent(document, 1, "AppAuthoringTestExtra")->mFields.at("value") == 4.5);
    CHECK(fixture.mAdapter->ComponentState(fixture.Member(first), "AppAuthoringTestExtra") ==
        AuthoredComponentState::Inherited);

    fixture.Commit();
    REQUIRE(ExtraOps().Has(fixture.mWorld, fixture.Member(first)));
    CHECK(FloatField(fixture.mWorld, fixture.Member(first), ExtraOps(), "value") == 4.5f);
}

TEST_CASE("Undo of applying a removed component restores the scene override", "[app][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress second{ 2 };
    const std::optional<AuthoredKey> removed =
        fixture.mAdapter->ComponentKeyOf(fixture.Member(second), "AppAuthoringTestData");
    REQUIRE(removed.has_value());

    fixture.Run(MakeAuthoredCommand("Remove component", { AuthoredEntry{ *removed, std::string("null") } }));
    REQUIRE(fixture.mAdapter->ComponentState(fixture.Member(second), "AppAuthoringTestData") ==
        AuthoredComponentState::Removed);

    const AuthoredAction applied = fixture.mAdapter->Apply(*removed);
    REQUIRE(applied.mAllowed);
    fixture.Run(MakeAuthoredCommand(applied.mName, applied.mEntries));
    CHECK(fixture.mHistory.StepCount() == 2);

    const PrefabDocument *document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    CHECK(TemplateComponent(document, 2, "AppAuthoringTestData") == nullptr);
    CHECK(fixture.mAdapter->ComponentState(fixture.Member(second), "AppAuthoringTestData") ==
        AuthoredComponentState::Absent);

    fixture.Commit();
    CHECK_FALSE(DataOps().Has(fixture.mWorld, fixture.Member(second)));

    fixture.Undo();
    CHECK(fixture.mHistory.StepCount() == 2);
    document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    REQUIRE(TemplateComponent(document, 2, "AppAuthoringTestData") != nullptr);
    CHECK(TemplateComponent(document, 2, "AppAuthoringTestData")->mFields.at("amount") == 9.0);
    CHECK(fixture.mAdapter->ComponentState(fixture.Member(second), "AppAuthoringTestData") ==
        AuthoredComponentState::Removed);
    CHECK(fixture.mAdapter->RemovedComponents(fixture.Member(second)) ==
        std::vector<std::string>{ "AppAuthoringTestData" });
    CHECK_FALSE(DataOps().Has(fixture.mWorld, fixture.Member(second)));

    fixture.Commit();
    CHECK_FALSE(DataOps().Has(fixture.mWorld, fixture.Member(second)));

    fixture.Redo();
    CHECK(fixture.mHistory.StepCount() == 2);
    document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    CHECK(TemplateComponent(document, 2, "AppAuthoringTestData") == nullptr);
    CHECK(fixture.mAdapter->ComponentState(fixture.Member(second), "AppAuthoringTestData") ==
        AuthoredComponentState::Absent);

    fixture.Commit();
    CHECK_FALSE(DataOps().Has(fixture.mWorld, fixture.Member(second)));
}

TEST_CASE("A gizmo-like drag merges and leaves untouched channels inherited", "[app][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress second{ 2 };
    const Entity member = fixture.Member(second);
    const ComponentOps *transform = ComponentRegistry::Instance().FindBySeq(mir::TypeIdOf<mir::Transform>().seq);
    REQUIRE(transform != nullptr);

    const uint32_t scaleField = static_cast<uint32_t>(transform->FindField("scale") - transform->mFields.data());

    for (float scale = 1.0f; scale <= 3.0f; scale += 1.0f)
    {
        FieldValue value = ReadField(fixture.mWorld, member, *transform, scaleField);
        auto *vector = reinterpret_cast<glm::vec3 *>(value.mBytes);
        vector->x = scale;
        fixture.Run(mir::MakeSetFieldsEditCommand(member, *transform, { value }, "Scale", fixture.mAdapter.get()));
    }

    CHECK(fixture.mHistory.StepCount() == 1);

    std::optional<AuthoredKey> position =
        fixture.mAdapter->KeyOf(member, "Transform", "position");
    std::optional<AuthoredKey> rotation =
        fixture.mAdapter->KeyOf(member, "Transform", "rotation");
    const std::optional<AuthoredKey> scale = fixture.mAdapter->KeyOf(member, "Transform", "scale");
    REQUIRE(scale.has_value());
    CHECK(fixture.mAdapter->FieldState(*scale) == AuthoredFieldState::Overridden);
    REQUIRE(position.has_value());
    REQUIRE(rotation.has_value());
    CHECK(fixture.mAdapter->FieldState(*position) == AuthoredFieldState::Inherited);
    CHECK(fixture.mAdapter->FieldState(*rotation) == AuthoredFieldState::Inherited);

    fixture.mHistory.EndStep();
    fixture.Flush();

    FieldValue value = ReadField(fixture.mWorld, member, *transform, scaleField);
    reinterpret_cast<glm::vec3 *>(value.mBytes)->y = 4.0f;
    fixture.Run(mir::MakeSetFieldsEditCommand(member, *transform, { value }, "Scale", fixture.mAdapter.get()));
    CHECK(fixture.mHistory.StepCount() == 2);
}

TEST_CASE("Undo of a source-node deletion restores nodes but not cleared references", "[app][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress first{ 1 };
    const SceneNodeAddress third{ 3 };
    const SceneNodeAddress fourth{ 4 };

    REQUIRE(SetInstanceEntityRef(fixture.mWorld, fixture.mScene, fixture.mRoot, first,
        "AppAuthoringTestData", "target", fixture.Binding(third)));
    const Entity removedThird = fixture.Member(third);
    const Entity removedFourth = fixture.Member(fourth);
    fixture.Save(kSceneDocument);
    fixture.Save(fixture.mPrefabDocument);

    AuthoredKey thirdNode;
    thirdNode.mObject.mDocument = fixture.mPrefabDocument;
    thirdNode.mObject.mAddress = { 3 };
    AuthoredKey fourthNode;
    fourthNode.mObject.mDocument = fixture.mPrefabDocument;
    fourthNode.mObject.mAddress = { 4 };
    fixture.Run(MakeAuthoredCommand("Delete prefab nodes", {
        AuthoredEntry{ thirdNode, std::nullopt },
        AuthoredEntry{ fourthNode, std::nullopt },
    }));
    fixture.Commit();
    fixture.Bridge();

    CHECK(fixture.mHistory.IsUnsaved(fixture.mPrefabDocument));
    CHECK(fixture.mHistory.IsUnsaved(kSceneDocument));
    CHECK_FALSE(fixture.mWorld.IsAlive(removedThird));
    CHECK_FALSE(fixture.mWorld.IsAlive(removedFourth));
    CHECK(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.size() == 2);

    const Entity root = fixture.Member(first);
    CHECK(EntityField(fixture.mWorld, root, DataOps(), "target").IsNull());
    REQUIRE(FindInstanceFieldPatch(fixture.mScene.mInstances.at(fixture.mRoot), first,
        "AppAuthoringTestData", "target") != nullptr);
    CHECK(FindInstanceFieldPatch(fixture.mScene.mInstances.at(fixture.mRoot), first,
              "AppAuthoringTestData", "target")->mValue.is_null());

    fixture.Undo();
    fixture.Commit();
    fixture.Bridge();

    CHECK(fixture.mScene.mInstances.at(fixture.mRoot).mBindings.contains(third));
    CHECK(fixture.mScene.mInstances.at(fixture.mRoot).mBindings.contains(fourth));
    CHECK(mir::ParentOf(fixture.mWorld, fixture.Member(third)) == root);
    CHECK(mir::ParentOf(fixture.mWorld, fixture.Member(fourth)) == fixture.Member(third));
    CHECK(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.size() == 4);
    CHECK(fixture.mHistory.IsUnsaved(kSceneDocument));
    CHECK_FALSE(fixture.mHistory.IsUnsaved(fixture.mPrefabDocument));

    CHECK(EntityField(fixture.mWorld, root, DataOps(), "target").IsNull());
    REQUIRE(FindInstanceFieldPatch(fixture.mScene.mInstances.at(fixture.mRoot), first,
        "AppAuthoringTestData", "target") != nullptr);
    CHECK(FindInstanceFieldPatch(fixture.mScene.mInstances.at(fixture.mRoot), first,
              "AppAuthoringTestData", "target")->mValue.is_null());

    fixture.Redo();
    fixture.Commit();
    CHECK(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.size() == 2);
}

TEST_CASE("Undoing an override edit back to the save point leaves the scene saved", "[app][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress second{ 2 };
    const AuthoredKey key = SceneEditKey(fixture, second, "AppAuthoringTestData", "amount");
    fixture.Save(kSceneDocument);

    const float value = 5.0f;
    fixture.Run(MakeAuthoredCommand("Edit amount",
        { AuthoredEntry{ key, fixture.mAdapter->EncodeField(FieldKind::Float, &value) } }));
    fixture.Commit();
    fixture.Bridge();
    CHECK(fixture.mHistory.IsUnsaved(kSceneDocument));

    fixture.Undo();
    fixture.Commit();
    fixture.Bridge();
    CHECK_FALSE(fixture.mHistory.IsUnsaved(kSceneDocument));
    CHECK_FALSE(fixture.mCoordinator->ConsumeSceneMaintenance());
}

TEST_CASE("The authored view reports the instance root and its members", "[app][authoring]")
{
    Fixture fixture;
    CHECK(fixture.mAdapter->InstanceRole(fixture.Member(SceneNodeAddress{ 1 })) == AuthoredInstanceRole::Root);
    CHECK(fixture.mAdapter->InstanceRole(fixture.Member(SceneNodeAddress{ 2 })) == AuthoredInstanceRole::Member);

    const Entity ordinary = CreateSceneEntity(fixture.mWorld, fixture.mScene);
    CHECK(fixture.mAdapter->InstanceRole(ordinary) == AuthoredInstanceRole::Ordinary);
    CHECK(fixture.mAdapter->InstanceRole(kNullEntity) == AuthoredInstanceRole::Ordinary);
}

TEST_CASE("The authored view lists every scene entity a Prefab binds", "[app][authoring]")
{
    Fixture fixture;
    const std::vector<Entity> linked = fixture.mAdapter->PrefabLinkedEntities();
    const auto contains = [&](Entity entity)
        {
            return std::find(linked.begin(), linked.end(), entity) != linked.end();
        };
    CHECK(contains(fixture.Member(SceneNodeAddress{ 1 })));
    CHECK(contains(fixture.Member(SceneNodeAddress{ 2 })));
    CHECK(linked.size() == 4);

    const Entity ordinary = CreateSceneEntity(fixture.mWorld, fixture.mScene);
    CHECK_FALSE(contains(ordinary));
}

TEST_CASE("A linked duplicate keeps the Prefab and remaps its own references", "[app][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress first{ 1 };
    const SceneNodeAddress second{ 2 };
    const StableId secondId = fixture.Binding(second);

    REQUIRE(SetInstanceEntityRef(fixture.mWorld, fixture.mScene, fixture.mRoot, first,
        "AppAuthoringTestData", "target", secondId));

    const Entity copy = DuplicateSceneEntitySubtree(fixture.mWorld, fixture.mScene, fixture.Member(first));
    REQUIRE(fixture.mWorld.IsAlive(copy));

    const StableId copyRoot = StableIdIn(fixture.mScene, copy);
    REQUIRE(copyRoot != kNullStableId);
    REQUIRE(fixture.mScene.mInstances.contains(copyRoot));

    const ScenePrefabInstance &copyRecord = fixture.mScene.mInstances.at(copyRoot);
    CHECK(copyRecord.mPrefab == fixture.mScene.mInstances.at(fixture.mRoot).mPrefab);
    CHECK(copyRecord.mBindings.at(second) != secondId);

    const mir::SceneFieldPatch *target =
        FindInstanceFieldPatch(copyRecord, first, "AppAuthoringTestData", "target");
    REQUIRE(target != nullptr);
    CHECK(target->mValue == nlohmann::json(copyRecord.mBindings.at(second)));

    const Entity copiedMember = fixture.mScene.mEntities.at(copyRecord.mBindings.at(second));
    CHECK(EntityField(fixture.mWorld, copy, DataOps(), "target") == copiedMember);
    CHECK(EntityField(fixture.mWorld, fixture.Member(first), DataOps(), "target") == fixture.Member(second));
}

TEST_CASE("Duplicate patches remap ordinary descendants across a save and reload", "[app][duplicate][save]")
{
    TempDir dir;
    Fixture fixture;
    World &world = fixture.mWorld;
    LoadedScene &scene = fixture.mScene;
    const Entity root = fixture.Member(SceneNodeAddress{ 1 });
    const Entity ordinary = CreateSceneEntity(world, scene, root);
    mir::AddTransform(world, ordinary);
    const StableId ordinaryId = StableIdIn(scene, ordinary);
    const Entity outside = CreateSceneEntity(world, scene);
    const StableId outsideId = StableIdIn(scene, outside);
    REQUIRE(SetInstanceEntityRef(world, scene, fixture.mRoot, SceneNodeAddress{ 1 },
        "AppAuthoringTestData", "target", ordinaryId));
    RefOps().AddDefault(world, root);
    REQUIRE(RefOps().FindField("link")->Write(RefOps().GetComponent(world, root), &ordinary));
    REQUIRE(RefOps().FindField("outside")->Write(RefOps().GetComponent(world, root), &outside));
    mir::GetOrAddInstancePatch(scene.mInstances.at(fixture.mRoot), SceneNodeAddress{ 1 }).mAddedComponents.push_back(
        { "AppAuthoringTestRefs", json{ { "link", ordinaryId }, { "outside", outsideId } } });

    const std::vector<Entity> sourceMembers = mir::SnapshotMembers(world, root);
    const auto ordinaryIndex = std::find(sourceMembers.begin(), sourceMembers.end(), ordinary) - sourceMembers.begin();
    const Entity copy = DuplicateSceneEntitySubtree(world, scene, root);
    REQUIRE(world.IsAlive(copy));
    const StableId copyId = StableIdIn(scene, copy);
    const Entity copiedOrdinary = mir::SnapshotMembers(world, copy).at(ordinaryIndex);
    const StableId copiedOrdinaryId = StableIdIn(scene, copiedOrdinary);
    REQUIRE(copiedOrdinaryId != ordinaryId);
    const ScenePrefabInstance &record = scene.mInstances.at(copyId);
    REQUIRE(FindInstanceFieldPatch(record, SceneNodeAddress{ 1 }, "AppAuthoringTestData", "target") != nullptr);
    CHECK(FindInstanceFieldPatch(record, SceneNodeAddress{ 1 }, "AppAuthoringTestData", "target")->mValue ==
        json(copiedOrdinaryId));
    const auto *added = mir::FindAddedComponent(record, SceneNodeAddress{ 1 }, "AppAuthoringTestRefs");
    REQUIRE(added != nullptr);
    CHECK(added->mFields.at("link") == json(copiedOrdinaryId));
    CHECK(added->mFields.at("outside") == json(outsideId));

    REQUIRE(mir::SaveScene(world, dir.mPath, scene, &fixture.mCoordinator->Resolver()));
    World loadedWorld;
    PrefabCoordinator loadedCoordinator;
    REQUIRE(loadedCoordinator.SetDocument(fixture.mDocument));
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, &loadedCoordinator.Resolver());
    const Entity loadedCopy = loaded.mEntities.at(copyId);
    CHECK(EntityField(loadedWorld, loadedCopy, DataOps(), "target") == loaded.mEntities.at(copiedOrdinaryId));
    CHECK(EntityField(loadedWorld, loadedCopy, RefOps(), "link") == loaded.mEntities.at(copiedOrdinaryId));
    CHECK(EntityField(loadedWorld, loadedCopy, RefOps(), "outside") == loaded.mEntities.at(outsideId));
    CHECK(EntityField(world, root, DataOps(), "target") == ordinary);
    CHECK(EntityField(world, root, RefOps(), "link") == ordinary);
}

TEST_CASE("Copied unresolved references survive saving until their target is available", "[app][duplicate][save]")
{
    TempDir dir;
    Fixture fixture;
    PrefabDocument targetDocument = fixture.mDocument;
    targetDocument.mId = mir::GeneratePrefabId();
    REQUIRE(fixture.mCoordinator->SetDocument(targetDocument));
    REQUIRE_FALSE(HasPrefabErrors(fixture.mCoordinator->Cache().Compile(targetDocument)));
    const auto targetTemplate = fixture.mCoordinator->Cache().Find(targetDocument.mId);
    REQUIRE(targetTemplate != nullptr);
    const StableId targetRoot = PlacePrefabInstance(fixture.mWorld, fixture.mScene, *targetTemplate);
    const StableId targetId = fixture.mScene.mInstances.at(targetRoot).mBindings.at(SceneNodeAddress{ 2 });
    const Entity target = fixture.mScene.mEntities.at(targetId);
    const Entity sourceRoot = fixture.Member(SceneNodeAddress{ 1 });
    const Entity ordinary = CreateSceneEntity(fixture.mWorld, fixture.mScene, sourceRoot);
    const StableId ordinaryId = StableIdIn(fixture.mScene, ordinary);
    mir::AddTransform(fixture.mWorld, ordinary);
    DataOps().AddDefault(fixture.mWorld, ordinary);
    REQUIRE(DataOps().FindField("target")->Write(DataOps().GetComponent(fixture.mWorld, ordinary), &target));
    REQUIRE(SetInstanceEntityRef(fixture.mWorld, fixture.mScene, fixture.mRoot, SceneNodeAddress{ 1 },
        "AppAuthoringTestData", "target", targetId));
    REQUIRE(mir::SaveScene(fixture.mWorld, dir.mPath / "initial", fixture.mScene, &fixture.mCoordinator->Resolver()));

    World partialWorld;
    PrefabCoordinator partialCoordinator;
    REQUIRE(partialCoordinator.SetDocument(fixture.mDocument));
    LoadedScene partial = mir::LoadScene(partialWorld, dir.mPath / "initial", &partialCoordinator.Resolver());
    REQUIRE_FALSE(partial.mEntities.contains(targetId));
    REQUIRE(partial.mUnresolvedEntityRefs.size() == 2);

    Entity source = partial.mEntities.at(fixture.mRoot);
    bool detached = false;
    SECTION("Linked instance") { }
    SECTION("Detached instance") { detached = true; }
    SECTION("Ordinary subtree") { source = partial.mEntities.at(ordinaryId); }
    const std::vector<Entity> members = mir::SnapshotMembers(partialWorld, source);
    const auto ownerIndex = std::find(members.begin(), members.end(), partial.mEntities.at(ordinaryId)) - members.begin();
    const Entity copy = detached ? mir::CopyInstanceMemberAsOrdinary(partialWorld, partial, source)
                                 : DuplicateSceneEntitySubtree(partialWorld, partial, source);
    REQUIRE(partialWorld.IsAlive(copy));
    const Entity copiedOwner = mir::SnapshotMembers(partialWorld, copy).at(ownerIndex);
    const StableId copiedOwnerId = StableIdIn(partial, copiedOwner);
    const auto copiedKey = std::make_tuple(copiedOwnerId, std::string("AppAuthoringTestData"), std::string("target"));
    CHECK(partial.mUnresolvedEntityRefs.contains(copiedKey));
    if (partial.mUnresolvedEntityRefs.contains(copiedKey))
        CHECK(partial.mUnresolvedEntityRefs.at(copiedKey) == targetId);
    CHECK(EntityField(partialWorld, copiedOwner, DataOps(), "target").IsNull());
    const StableId copyId = StableIdIn(partial, copy);
    if (source != partial.mEntities.at(ordinaryId))
        CHECK(partial.mUnresolvedEntityRefs.contains(
            { copyId, std::string("AppAuthoringTestData"), std::string("target") }));
    CHECK(partial.mUnresolvedEntityRefs.at(
        { ordinaryId, std::string("AppAuthoringTestData"), std::string("target") }) == targetId);
    REQUIRE(mir::SaveScene(partialWorld, dir.mPath / "copied", partial, &partialCoordinator.Resolver()));

    World loadedWorld;
    PrefabCoordinator loadedCoordinator;
    REQUIRE(loadedCoordinator.SetDocument(fixture.mDocument));
    REQUIRE(loadedCoordinator.SetDocument(targetDocument));
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath / "copied", &loadedCoordinator.Resolver());
    CHECK(EntityField(loadedWorld, loaded.mEntities.at(copiedOwnerId), DataOps(), "target") ==
        loaded.mEntities.at(targetId));
    CHECK(EntityField(loadedWorld, loaded.mEntities.at(copyId), DataOps(), "target") == loaded.mEntities.at(targetId));
    CHECK(EntityField(loadedWorld, loaded.mEntities.at(ordinaryId), DataOps(), "target") == loaded.mEntities.at(targetId));
    CHECK(loaded.mUnresolvedEntityRefs.empty());
}

TEST_CASE("A frozen authored view reports no overrides and refuses actions", "[app][authoring]")
{
    Fixture fixture;
    const SceneNodeAddress second{ 2 };
    const Entity member = fixture.Member(second);
    const AuthoredKey field = SceneEditKey(fixture, second, "AppAuthoringTestData", "amount");
    const std::optional<AuthoredKey> component =
        fixture.mAdapter->ComponentKeyOf(member, "AppAuthoringTestData");
    REQUIRE(component.has_value());

    fixture.mCoordinator->SetFrozen(true);

    CHECK_FALSE(fixture.mAdapter->KeyOf(member, "AppAuthoringTestData", "amount").has_value());
    CHECK_FALSE(fixture.mAdapter->ComponentKeyOf(member, "AppAuthoringTestData").has_value());
    CHECK(fixture.mAdapter->FieldState(field) == AuthoredFieldState::Inherited);
    CHECK(fixture.mAdapter->ComponentState(member, "AppAuthoringTestData") == AuthoredComponentState::Absent);
    CHECK(fixture.mAdapter->RemovedComponents(member).empty());

    const AuthoredAction revert = fixture.mAdapter->Revert(field);
    CHECK_FALSE(revert.mAllowed);
    CHECK_FALSE(revert.mReason.empty());
    const AuthoredAction apply = fixture.mAdapter->Apply(field);
    CHECK_FALSE(apply.mAllowed);
    CHECK_FALSE(apply.mReason.empty());

    const std::vector<AuthoredEntry> entries{ AuthoredEntry{ field, std::string("93.0") } };
    const mir::AuthoringWrite write = fixture.mAdapter->Write(entries);
    CHECK_FALSE(write.mApplied);
    CHECK(FloatField(fixture.mWorld, member, DataOps(), "amount") == 9.0f);

    fixture.mCoordinator->SetFrozen(false);
    CHECK(fixture.mAdapter->KeyOf(member, "AppAuthoringTestData", "amount").has_value());
}

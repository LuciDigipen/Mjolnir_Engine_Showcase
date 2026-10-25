/**
 * @file PrefabWorkspace.tests.cpp
 * @author Jackey Cen
 * @brief Tests for app/PrefabWorkspace.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "../src/app/PrefabAuthoringAdapter.h"
#include "../src/app/PrefabWorkspace.h"

#include <editor/history/AuthoredEdit.h>
#include <editor/history/Authoring.h>
#include <editor/history/DocumentNodeCommands.h>
#include <editor/history/EntityCommands.h>
#include <editor/history/History.h>
#include <editor/history/DocumentNodeCommands.h>
#include <editor/panels/HierarchyPanel.h>
#include <editor/history/SceneOps.h>
#include <editor/panels/HierarchyPanel.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/EntityPresetRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/Transform.h>
#include <core/ecs/components/WorldTransform.h>
#include <prefab/PrefabCoordinator.h>
#include <prefab/PrefabFormat.h>
#include <prefab/PrefabSceneInstance.h>
#include <scene/EntitySnapshot.h>
#include <scene/SceneAsset.h>
#include <scene/SceneDuplicate.h>
#include <scene/SceneIO.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    using nlohmann::json;
    using mir::AuthoredAction;
    using mir::AuthoredEntry;
    using mir::AuthoredInstanceRole;
    using mir::AuthoredKey;
    using mir::CapturedSubtree;
    using mir::CommandBuffer;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::DocumentId;
    using mir::Entity;
    using mir::FieldKind;
    using mir::HasPrefabErrors;
    using mir::History;
    using mir::kNullEntity;
    using mir::kNullStableId;
    using mir::LoadedScene;
    using mir::MakeAuthoredCommand;
    using mir::NewScene;
    using mir::ParentOf;
    using mir::PrefabAuthoringAdapter;
    using mir::PrefabComponentInstance;
    using mir::PrefabCoordinator;
    using mir::PrefabDocument;
    using mir::PrefabWorkspace;
    using mir::PrefabId;
    using mir::PrefabNode;
    using mir::PrefabSystem;
    using mir::RuntimeFieldDecl;
    using mir::SceneNodeAddress;
    using mir::ScenePrefabInstance;
    using mir::StableId;
    using mir::SystemContext;
    using mir::ToString;
    using mir::World;

    const ComponentOps &DataOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "amount", FieldKind::Float },
            { "bonus", FieldKind::Float },
            { "target", FieldKind::EntityRef },
        };
        return ComponentRegistry::Instance().RegisterRuntime("AppDocumentsTestData", fields);
    }

    void CreatePresetEntity(World &world, Entity entity)
    {
        mir::AddTransform(world, entity);
        world.AddComponent<mir::Name>(entity, mir::MakeName("Preset Node"));

        const ComponentOps &ops = DataOps();
        alignas(16) std::byte data[64]{};
        const float amount = 7.0f;
        const float bonus = 4.0f;
        ops.FindField("amount")->Write(data, &amount);
        ops.FindField("bonus")->Write(data, &bonus);
        ops.AddCopy(world, entity, data);
    }

    bool CanCreateNothing(const World &) { return false; }

    const mir::EntityPresetDesc &TestPreset()
    {
        static const mir::EntityPresetDesc preset{ "Test/Preset", &CreatePresetEntity, nullptr };
        static const bool registered = []
            {
                mir::EntityPresetRegistry::Instance().Register(preset.mName, preset.mCreate, preset.mCanCreate);
                return true;
            }();
        (void)registered;
        return preset;
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

    class NoSceneOps final : public mir::ISceneOps
    {
    public:
        Entity CreateEntity() override { return kNullEntity; }
        Entity Duplicate(Entity) override { return kNullEntity; }
        Entity CopyAsOrdinaryObject(Entity) override { return kNullEntity; }
        CapturedSubtree Capture(Entity) override { return {}; }
        std::vector<Entity> Restore(const CapturedSubtree &, Entity, Entity) override { return {}; }
        bool Destroy(Entity) override { return false; }
        Entity PlacePrefab(std::string_view, Entity, const mir::WorldVec3 &) override { return kNullEntity; }
    };

    class DocumentSceneOps final : public mir::ISceneOps
    {
    public:
        DocumentSceneOps(World &world, LoadedScene &scene, PrefabWorkspace &documents, PrefabId prefab)
            : mWorld(world), mScene(scene), mDocuments(documents), mPrefab(prefab)
        {
        }

        Entity CreateEntity() override { return mir::CreateSceneEntity(mWorld, mScene); }
        Entity Duplicate(Entity source) override { return mir::DuplicateSceneEntitySubtree(mWorld, mScene, source); }
        Entity CopyAsOrdinaryObject(Entity source) override
        {
            return mir::CopyInstanceMemberAsOrdinary(mWorld, mScene, source);
        }

        CapturedSubtree Capture(Entity root) override
        {
            CapturedSubtree subtree;
            subtree.mMembers = mir::SnapshotMembers(mWorld, root);
            subtree.mData = mir::CaptureEntity(mWorld, root).mData;
            for (const Entity member : subtree.mMembers)
            {
                StableId id = kNullStableId;
                for (const auto &[candidate, entity] : mScene.mEntities)
                {
                    if (entity == member)
                        id = candidate;
                }
                subtree.mStableIds.push_back(id);
            }
            subtree.mInstances = mir::CaptureInstanceRecords(mScene, subtree.mStableIds);
            return subtree;
        }

        std::vector<Entity> Restore(const CapturedSubtree &subtree, Entity parent, Entity before) override
        {
            std::vector<Entity> members = mir::RestoreEntityAt(mWorld, mir::EntitySnapshot{ subtree.mData }, mScene,
                subtree.mStableIds, parent, before);
            if (!members.empty())
                mir::RestoreInstanceRecords(mScene, subtree.mInstances);
            return members;
        }

        bool Destroy(Entity root) override { return mir::DestroySceneEntity(mWorld, mScene, root); }

        Entity PlacePrefab(std::string_view, Entity parent, const mir::WorldVec3 &position) override
        {
            return mDocuments.Place(mWorld, mPrefab, parent, position);
        }

    private:
        World &mWorld;
        LoadedScene &mScene;
        PrefabWorkspace &mDocuments;
        PrefabId mPrefab;
    };

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
                ("mir_prefab_documents_test_" + ProcessToken() + "_" + std::to_string(counter.fetch_add(1)));
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

    std::string ReadText(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    PrefabDocument Document()
    {
        PrefabDocument document = mir::NewPrefabDocument();
        document.mNodes[0].mComponents.push_back(PrefabComponentInstance{
            "AppDocumentsTestData",
            json{ { "amount", 2.0 }, { "bonus", 3.0 }, { "target", json::array({ "2" }) } } });
        document.mNodes[0].mComponents.push_back(PrefabComponentInstance{ "Transform", json::object() });

        PrefabNode second;
        second.mId = 2;
        second.mParent = 1;
        second.mComponents.push_back(PrefabComponentInstance{
            "AppDocumentsTestData",
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

    PrefabDocument OuterDocument(PrefabId inner)
    {
        PrefabDocument document = mir::NewPrefabDocument();
        document.mNodes[0].mComponents.push_back(PrefabComponentInstance{ "Transform", json::object() });

        PrefabNode occurrence;
        occurrence.mId = 2;
        occurrence.mKind = mir::PrefabNodeKind::Prefab;
        occurrence.mParent = 1;
        occurrence.mPrefab = inner;
        document.mNodes.push_back(std::move(occurrence));
        document.mNextNodeId = 3;
        return document;
    }

    float FloatField(World &world, Entity entity, const ComponentOps &ops, std::string_view field)
    {
        void *component = ops.GetComponent(world, entity);
        REQUIRE(component != nullptr);
        const mir::FieldDesc *desc = ops.FindField(field);
        REQUIRE(desc != nullptr);
        float value = 0.0f;
        desc->Read(component, &value);
        return value;
    }

    Entity EntityField(World &world, Entity entity, const ComponentOps &ops, std::string_view field)
    {
        void *component = ops.GetComponent(world, entity);
        REQUIRE(component != nullptr);
        const mir::FieldDesc *desc = ops.FindField(field);
        REQUIRE(desc != nullptr);
        Entity value = kNullEntity;
        desc->Read(component, &value);
        return value;
    }

    struct Fixture
    {
        World mWorld;
        LoadedScene mScene = NewScene("main");
        PrefabCoordinator *mCoordinator = nullptr;
        TempDir mContentRoot;
        TempDir mPrefabsDir;
        std::unique_ptr<PrefabAuthoringAdapter> mAdapter;
        std::unique_ptr<PrefabWorkspace> mDocuments;
        History mHistory;
        NoSceneOps mSceneOps;
        CommandBuffer mCommands;
        PrefabDocument mDocument;
        DocumentId mDocumentId;

        Fixture()
        {
            mir::RegisterCoreComponents();
            DataOps();

            mWorld.EmplaceResource<mir::EngineModeState>();
            mCoordinator = &mWorld.EmplaceResource<PrefabCoordinator>();

            mDocument = Document();
            REQUIRE(mCoordinator->Registry().SetDocument(mDocument));
            mCoordinator->Bind(mScene);
            mDocumentId = DocumentId{ mDocument.mId.mHigh, mDocument.mId.mLow };

            mAdapter = std::make_unique<PrefabAuthoringAdapter>(mWorld, *mCoordinator, mContentRoot.mPath,
                mPrefabsDir.mPath);
            mHistory.SetAuthoringBackend(mAdapter.get());
            mDocuments = std::make_unique<PrefabWorkspace>(*mCoordinator, *mAdapter);
        }

        const LoadedScene &PreviewScene() const
        {
            const LoadedScene *scene = mDocuments->PreviewScene(mDocument.mId);
            REQUIRE(scene != nullptr);
            return *scene;
        }

        StableId PreviewRoot() const { return PreviewScene().mInstances.begin()->first; }

        Entity PreviewEntity(const SceneNodeAddress &address) const
        {
            const LoadedScene &scene = PreviewScene();
            return scene.mEntities.at(scene.mInstances.at(PreviewRoot()).mBindings.at(address));
        }

        void Commit()
        {
            PrefabSystem system;
            SystemContext context{ mWorld, mCommands };
            system.OnUpdate(context);
        }

        void Run(std::unique_ptr<mir::ICommand> command)
        {
            mHistory.Submit(std::move(command));
            mHistory.Flush(mWorld, mSceneOps);
        }

        void Undo()
        {
            mHistory.RequestUndo();
            mHistory.Flush(mWorld, mSceneOps);
        }

        void Redo()
        {
            mHistory.RequestRedo();
            mHistory.Flush(mWorld, mSceneOps);
        }

        PrefabWorkspace::PrefabUnsaved Unsaved() const
        {
            return [this](PrefabId id)
                {
                    return !mCoordinator->Registry().PathOf(id).has_value() ||
                        mHistory.IsUnsaved(DocumentId{ id.mHigh, id.mLow });
                };
        }

        PrefabWorkspace::PrefabSaved MarkSaved()
        {
            return [this](PrefabId id) { mHistory.SetUnsaved(DocumentId{ id.mHigh, id.mLow }, false); };
        }

        bool SavePrefab(PrefabId id) { return mDocuments->SavePrefab(id, Unsaved(), MarkSaved()); }

        bool SaveScene(const std::function<bool()> &writeScene)
        {
            return mDocuments->SaveScene(mScene, Unsaved(), MarkSaved(), writeScene);
        }
    };

    AuthoredKey FieldKey(Fixture &fixture, const SceneNodeAddress &node, std::string component, std::string field)
    {
        const std::optional<AuthoredKey> key =
            fixture.mAdapter->KeyOf(fixture.PreviewEntity(node), component, field);
        REQUIRE(key.has_value());
        return *key;
    }
}

TEST_CASE("Opening a prefab creates one isolated preview and focuses it again", "[app][prefabs]")
{
    Fixture fixture;
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));
    CHECK(fixture.mDocuments->IsOpen(fixture.mDocument.mId));
    CHECK(fixture.mDocuments->ActiveDocument() == fixture.mDocument.mId);
    CHECK(fixture.mAdapter->ActiveDocument() == fixture.mDocument.mId);

    const LoadedScene &preview = fixture.PreviewScene();
    CHECK(preview.mInstances.size() == 1);
    CHECK(preview.mEntities.size() == preview.mInstances.begin()->second.mBindings.size());
    CHECK(fixture.mScene.mEntities.empty());
    std::size_t mainTransforms = 0;
    fixture.mWorld.GetOrCreateQuery<const mir::Transform>().ForEach(
        [&mainTransforms](Entity, const mir::Transform &) { ++mainTransforms; });
    CHECK(mainTransforms == 0);

    World *world = fixture.mDocuments->PreviewWorld(fixture.mDocument.mId);
    REQUIRE(world != nullptr);
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));
    CHECK(fixture.mDocuments->PreviewWorld(fixture.mDocument.mId) == world);
}

TEST_CASE("A preview edit leaves the main scene alone and closing keeps the document", "[app][prefabs]")
{
    Fixture fixture;
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));

    const AuthoredKey key = FieldKey(fixture, SceneNodeAddress{ 2 }, "AppDocumentsTestData", "amount");
    const float value = 42.0f;
    fixture.Run(MakeAuthoredCommand("Edit amount",
        { AuthoredEntry{ key, fixture.mAdapter->EncodeField(FieldKind::Float, &value) } }));
    CHECK(fixture.mHistory.StepCount() == 1);
    fixture.Commit();

    CHECK(fixture.mHistory.IsUnsaved(fixture.mDocumentId));
    CHECK(fixture.mScene.mEntities.empty());
    CHECK(fixture.mScene.mInstances.empty());
    CHECK_FALSE(fixture.mCoordinator->ConsumeSceneMaintenance());

    REQUIRE(fixture.mDocuments->Close(fixture.mDocument.mId));
    CHECK_FALSE(fixture.mDocuments->IsOpen(fixture.mDocument.mId));
    CHECK(fixture.mDocuments->PreviewWorld(fixture.mDocument.mId) == nullptr);
    CHECK(fixture.mDocuments->ActiveDocument() == std::nullopt);
    CHECK_FALSE(fixture.mAdapter->ActiveDocument().has_value());
    CHECK(fixture.mHistory.IsUnsaved(fixture.mDocumentId));

    const PrefabDocument *document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    CHECK(document->mNodes[1].mComponents.front().mFields.at("amount") == 42.0);

    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));
    CHECK(fixture.PreviewEntity(SceneNodeAddress{ 2 }) != kNullEntity);
}

TEST_CASE("A preview World answers engine mode queries in Edit", "[app][prefabs]")
{
    Fixture fixture;
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));

    World *world = fixture.mDocuments->PreviewWorld(fixture.mDocument.mId);
    REQUIRE(world != nullptr);
    REQUIRE(world->TryResource<mir::EngineModeState>() != nullptr);
    CHECK(mir::IsEditing(*world));
}

TEST_CASE("Moving the root in a Prefab tab moves its preview root", "[app][prefabs]")
{
    Fixture fixture;
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));

    const AuthoredKey key = FieldKey(fixture, SceneNodeAddress{ 1 }, "Transform", "position");
    const mir::WorldVec3 position{ 7.0, 0.0, 0.0 };
    fixture.Run(MakeAuthoredCommand("Move root",
        { AuthoredEntry{ key, fixture.mAdapter->EncodeField(FieldKind::WorldPosition, &position) } }));
    fixture.Commit();

    World *world = fixture.mDocuments->PreviewWorld(fixture.mDocument.mId);
    REQUIRE(world != nullptr);
    const mir::Transform *transform = world->GetComponent<mir::Transform>(fixture.PreviewEntity(SceneNodeAddress{ 1 }));
    REQUIRE(transform != nullptr);
    CHECK(transform->Position().x == 7.0);
}

TEST_CASE("Preview entities expose document keys and direct component authoring", "[app][prefabs]")
{
    Fixture fixture;
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));

    const Entity second = fixture.PreviewEntity(SceneNodeAddress{ 2 });
    const AuthoredKey key = FieldKey(fixture, SceneNodeAddress{ 2 }, "AppDocumentsTestData", "amount");
    CHECK(key.mObject.mDocument == fixture.mDocumentId);
    CHECK(key.mObject.mAddress == std::vector<uint64_t>{ 2 });
    CHECK(fixture.mAdapter->FieldState(key) == mir::AuthoredFieldState::Inherited);
    CHECK(fixture.mAdapter->ComponentState(second, "AppDocumentsTestData") ==
        mir::AuthoredComponentState::Inherited);
    CHECK(fixture.mAdapter->WritesComponentDirectly(second, "AppDocumentsTestData"));
    CHECK_FALSE(fixture.mAdapter->WritesComponentDirectly(second, "Name"));

    const Entity third = fixture.PreviewEntity(SceneNodeAddress{ 3 });
    CHECK(json::parse(fixture.mAdapter->EncodeField(FieldKind::EntityRef, &third)) ==
        json::array({ "3" }));

    fixture.Run(mir::MakeRemoveComponentEditCommand(second, DataOps(), fixture.mAdapter.get()));
    fixture.Commit();
    CHECK(fixture.mAdapter->ComponentState(second, "AppDocumentsTestData") ==
        mir::AuthoredComponentState::Absent);
    CHECK_FALSE(DataOps().Has(*fixture.mDocuments->PreviewWorld(fixture.mDocument.mId),
        fixture.PreviewEntity(SceneNodeAddress{ 2 })));

    fixture.Undo();
    fixture.Commit();
    CHECK(DataOps().Has(*fixture.mDocuments->PreviewWorld(fixture.mDocument.mId),
        fixture.PreviewEntity(SceneNodeAddress{ 2 })));
}

TEST_CASE("Node create, delete and reparent are one undoable command each", "[app][prefabs]")
{
    Fixture fixture;
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));

    const Entity second = fixture.PreviewEntity(SceneNodeAddress{ 2 });
    const Entity third = fixture.PreviewEntity(SceneNodeAddress{ 3 });
    const Entity fourth = fixture.PreviewEntity(SceneNodeAddress{ 4 });

    const AuthoredAction created = fixture.mAdapter->CreateNode(second, 0);
    REQUIRE(created.mAllowed);
    REQUIRE(created.mEntries.size() == 1);
    fixture.Run(MakeAuthoredCommand(created.mName, created.mEntries));
    fixture.Commit();
    REQUIRE(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.size() == 5);
    CHECK(fixture.PreviewScene().mInstances.at(fixture.PreviewRoot()).mBindings.size() == 5);

    fixture.Undo();
    fixture.Commit();
    CHECK(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.size() == 4);
    fixture.Redo();
    fixture.Commit();
    CHECK(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.size() == 5);
    fixture.Undo();
    fixture.Commit();

    const AuthoredAction moved = fixture.mAdapter->ReparentNode(fourth, second, 0);
    REQUIRE(moved.mAllowed);
    fixture.Run(MakeAuthoredCommand(moved.mName, moved.mEntries));
    fixture.Commit();
    CHECK(ParentOf(*fixture.mDocuments->PreviewWorld(fixture.mDocument.mId), fourth) == second);

    fixture.Undo();
    fixture.Commit();
    CHECK(ParentOf(*fixture.mDocuments->PreviewWorld(fixture.mDocument.mId), fourth) == third);

    const AuthoredAction deleted = fixture.mAdapter->DeleteNodes(std::vector<Entity>{ third });
    REQUIRE(deleted.mAllowed);
    REQUIRE(deleted.mEntries.size() == 2);
    fixture.Run(MakeAuthoredCommand(deleted.mName, deleted.mEntries));
    fixture.Commit();
    CHECK(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.size() == 2);
    CHECK_FALSE(fixture.mDocuments->PreviewWorld(fixture.mDocument.mId)->IsAlive(third));
    CHECK_FALSE(fixture.mDocuments->PreviewWorld(fixture.mDocument.mId)->IsAlive(fourth));

    fixture.Undo();
    fixture.Commit();
    CHECK(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.size() == 4);
    CHECK(fixture.mDocuments->PreviewWorld(fixture.mDocument.mId)->IsAlive(
        fixture.PreviewEntity(SceneNodeAddress{ 3 })));
    CHECK(fixture.mDocuments->PreviewWorld(fixture.mDocument.mId)->IsAlive(
        fixture.PreviewEntity(SceneNodeAddress{ 4 })));

    const Entity root = fixture.PreviewEntity(SceneNodeAddress{ 1 });
    const AuthoredAction rootDelete = fixture.mAdapter->DeleteNodes(std::vector<Entity>{ root });
    CHECK_FALSE(rootDelete.mAllowed);
    CHECK(fixture.mDocuments->PreviewWorld(fixture.mDocument.mId)->IsAlive(root));
}

TEST_CASE("A preset creates its document node as one undoable step", "[app][prefabs]")
{
    Fixture fixture;
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));

    const AuthoredAction created =
        fixture.mAdapter->CreateNode(fixture.PreviewEntity(SceneNodeAddress{ 1 }), 0, &TestPreset());
    REQUIRE(created.mAllowed);
    CHECK(created.mName == "Create Preset");

    fixture.Run(MakeAuthoredCommand(created.mName, created.mEntries));
    CHECK(fixture.mHistory.StepCount() == 1);
    fixture.Commit();

    const PrefabDocument *document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    REQUIRE(document->mNodes.size() == 5);
    const PrefabNode &node = document->mNodes[1];
    const mir::PrefabNodeId createdId = node.mId;
    CHECK(node.mParent == 1);
    CHECK(node.mKind == mir::PrefabNodeKind::Entity);

    const PrefabComponentInstance *data = FindComponent(node, "AppDocumentsTestData");
    REQUIRE(data != nullptr);
    CHECK(data->mFields.at("amount") == 7.0);
    CHECK(data->mFields.at("bonus") == 4.0);
    CHECK(data->mFields.at("target").is_null());
    const PrefabComponentInstance *name = FindComponent(node, "Name");
    REQUIRE(name != nullptr);
    CHECK(name->mFields.at("name") == "Preset Node");
    const PrefabComponentInstance *worldTransform = FindComponent(node, "WorldTransform");
    REQUIRE(worldTransform != nullptr);
    CHECK(worldTransform->mFields.empty());

    World *previewWorld = fixture.mDocuments->PreviewWorld(fixture.mDocument.mId);
    REQUIRE(previewWorld != nullptr);
    const Entity createdEntity = fixture.PreviewEntity(SceneNodeAddress{ createdId });
    CHECK(FloatField(*previewWorld, createdEntity, DataOps(), "amount") == 7.0f);

    fixture.Undo();
    fixture.Commit();
    document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    CHECK(document->mNodes.size() == 4);
    CHECK(fixture.mHistory.StepCount() == 1);

    fixture.Redo();
    fixture.Commit();
    document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    CHECK(document->mNodes.size() == 5);
    CHECK(fixture.PreviewEntity(SceneNodeAddress{ createdId }) != kNullEntity);
}

TEST_CASE("A preset that refuses the active World creates no node", "[app][prefabs]")
{
    Fixture fixture;
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));
    const mir::EntityPresetDesc refused{ "Test/Refused", &CreatePresetEntity, &CanCreateNothing };

    const AuthoredAction created =
        fixture.mAdapter->CreateNode(fixture.PreviewEntity(SceneNodeAddress{ 1 }), 0, &refused);
    CHECK_FALSE(created.mAllowed);
    CHECK(created.mEntries.empty());
    CHECK(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.size() == 4);
}

TEST_CASE("Preview roles mark the nested occurrence, not the document", "[app][prefabs]")
{
    Fixture fixture;
    const PrefabDocument outer = OuterDocument(fixture.mDocument.mId);
    REQUIRE(fixture.mCoordinator->Registry().SetDocument(outer));
    REQUIRE(fixture.mDocuments->Open(outer.mId));

    const LoadedScene &preview = *fixture.mDocuments->PreviewScene(outer.mId);
    const StableId rootId = preview.mInstances.begin()->first;
    const auto entityAt = [&](const SceneNodeAddress &address)
        {
            return preview.mEntities.at(preview.mInstances.at(rootId).mBindings.at(address));
        };

    CHECK(fixture.mAdapter->InstanceRole(entityAt(SceneNodeAddress{ 1 })) == AuthoredInstanceRole::Ordinary);
    CHECK(fixture.mAdapter->InstanceRole(entityAt(SceneNodeAddress{ 2, 1 })) == AuthoredInstanceRole::Root);
    CHECK(fixture.mAdapter->InstanceRole(entityAt(SceneNodeAddress{ 2, 2 })) == AuthoredInstanceRole::Member);
    CHECK(fixture.mAdapter->InstanceRole(kNullEntity) == AuthoredInstanceRole::Ordinary);
}

TEST_CASE("Prefab-linked rows cover a document's nested occurrences only", "[app][prefabs]")
{
    Fixture fixture;
    const PrefabDocument outer = OuterDocument(fixture.mDocument.mId);
    REQUIRE(fixture.mCoordinator->Registry().SetDocument(outer));
    REQUIRE(fixture.mDocuments->Open(outer.mId));

    const LoadedScene &preview = *fixture.mDocuments->PreviewScene(outer.mId);
    const StableId rootId = preview.mInstances.begin()->first;
    const auto entityAt = [&](const SceneNodeAddress &address)
        {
            return preview.mEntities.at(preview.mInstances.at(rootId).mBindings.at(address));
        };

    const std::vector<Entity> linked = fixture.mAdapter->PrefabLinkedEntities();
    const auto contains = [&](Entity entity)
        {
            return std::find(linked.begin(), linked.end(), entity) != linked.end();
        };
    CHECK_FALSE(contains(entityAt(SceneNodeAddress{ 1 })));
    CHECK(contains(entityAt(SceneNodeAddress{ 2, 1 })));
    CHECK(contains(entityAt(SceneNodeAddress{ 2, 2 })));
    CHECK(contains(entityAt(SceneNodeAddress{ 2, 3 })));
    CHECK(contains(entityAt(SceneNodeAddress{ 2, 4 })));
    CHECK(linked.size() == 4);
}

TEST_CASE("One document is active and the Scene tab clears it", "[app][prefabs]")
{
    Fixture fixture;
    PrefabDocument second = Document();
    second.mId = mir::GeneratePrefabId();
    REQUIRE(fixture.mCoordinator->Registry().SetDocument(second));

    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));
    REQUIRE(fixture.mDocuments->Open(second.mId));
    CHECK(fixture.mDocuments->ActiveDocument() == second.mId);
    CHECK(fixture.mDocuments->OpenDocuments() == std::vector<PrefabId>{ fixture.mDocument.mId, second.mId });

    fixture.mDocuments->ActivateScene();
    CHECK(fixture.mDocuments->ActiveDocument() == std::nullopt);
    CHECK_FALSE(fixture.mAdapter->ActiveDocument().has_value());
    CHECK(fixture.mDocuments->IsOpen(fixture.mDocument.mId));
    CHECK(fixture.mDocuments->IsOpen(second.mId));

    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));
    CHECK(fixture.mDocuments->ActiveDocument() == fixture.mDocument.mId);
}

TEST_CASE("A tab is labelled by its file or a short id before it has one", "[app][prefabs]")
{
    Fixture fixture;
    PrefabDocument second = Document();
    second.mId = mir::GeneratePrefabId();
    REQUIRE(fixture.mCoordinator->Registry().SetDocument(second));

    TempDir directory;
    REQUIRE(mir::WritePrefabFile(directory.mPath / "Prefab One.prefab", fixture.mDocument));
    CHECK(fixture.mCoordinator->Registry().Scan(directory.mPath).empty());

    CHECK(fixture.mDocuments->LabelOf(fixture.mDocument.mId) == "Prefab One");
    CHECK(fixture.mDocuments->LabelOf(second.mId) == ToString(second.mId).substr(0, 8));
}

TEST_CASE("A content path resolves to its prefab", "[app][prefabs]")
{
    Fixture fixture;
    TempDir directory;
    const std::filesystem::path file = directory.mPath / (ToString(fixture.mDocument.mId) + ".prefab");
    REQUIRE(mir::WritePrefabFile(file, fixture.mDocument));
    CHECK(fixture.mCoordinator->Registry().Scan(directory.mPath).empty());

    CHECK(fixture.mDocuments->FindByPath(file) == fixture.mDocument.mId);
    CHECK_FALSE(fixture.mDocuments->FindByPath(directory.mPath / "missing.prefab").has_value());
}

TEST_CASE("Save Prefab writes its file and clears only its unsaved state", "[app][prefabs]")
{
    Fixture fixture;
    TempDir directory;
    REQUIRE(mir::WritePrefabFile(directory.mPath / (ToString(fixture.mDocument.mId) + ".prefab"),
        fixture.mDocument));
    CHECK(fixture.mCoordinator->Registry().Scan(directory.mPath).empty());

    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));
    const AuthoredKey key = FieldKey(fixture, SceneNodeAddress{ 2 }, "AppDocumentsTestData", "amount");
    const float value = 42.0f;
    fixture.Run(MakeAuthoredCommand("Edit amount",
        { AuthoredEntry{ key, fixture.mAdapter->EncodeField(FieldKind::Float, &value) } }));
    CHECK(fixture.mHistory.IsUnsaved(fixture.mDocumentId));
    fixture.mHistory.SetUnsaved(mir::kSceneDocument, true);

    CHECK(fixture.SavePrefab(fixture.mDocument.mId));
    CHECK_FALSE(fixture.mHistory.IsUnsaved(fixture.mDocumentId));
    CHECK(fixture.mHistory.IsUnsaved(mir::kSceneDocument));

    const std::filesystem::path path = *fixture.mCoordinator->Registry().PathOf(fixture.mDocument.mId);
    CHECK(ReadText(path).find("42.0") != std::string::npos);
}

TEST_CASE("An unsaved prefab's intended path opens and labels its document", "[app][prefabs]")
{
    Fixture fixture;
    const std::filesystem::path path = fixture.mPrefabsDir.mPath / "Chair.prefab";
    REQUIRE(fixture.mCoordinator->Registry().SetIntendedPath(fixture.mDocument.mId, path));

    CHECK(fixture.mDocuments->FindByPath(path) == fixture.mDocument.mId);
    CHECK(fixture.mDocuments->LabelOf(fixture.mDocument.mId) == "Chair");
    CHECK_FALSE(fixture.mCoordinator->Registry().HasSavedFile(fixture.mDocument.mId));

    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));
    CHECK(fixture.mDocuments->IsOpen(fixture.mDocument.mId));
    CHECK(fixture.mDocuments->PreviewScene(fixture.mDocument.mId) != nullptr);
}

TEST_CASE("Placing a Prefab is one step that keeps its StableIds and record", "[app][prefabs]")
{
    Fixture fixture;
    DocumentSceneOps ops(fixture.mWorld, fixture.mScene, *fixture.mDocuments, fixture.mDocument.mId);
    History history;
    const mir::WorldVec3 position(2.0, 0.0, 3.0);

    history.Submit(mir::MakePlacePrefabCommand("assets/prefabs/Chair.prefab", kNullEntity, position));
    history.Flush(fixture.mWorld, ops);
    REQUIRE(history.StepCount() == 1);
    REQUIRE(fixture.mScene.mInstances.size() == 1);

    const StableId rootId = fixture.mScene.mInstances.begin()->first;
    REQUIRE(fixture.mScene.mEntities.contains(rootId));
    const Entity root = fixture.mScene.mEntities.at(rootId);
    CHECK(fixture.mWorld.GetComponent<mir::Transform>(root)->Position() == position);
    const ScenePrefabInstance record = fixture.mScene.mInstances.at(rootId);
    CHECK(record.mPrefab == ToString(fixture.mDocument.mId));
    CHECK(record.mPlacement.at("position") == nlohmann::json::array({ 2.0, 0.0, 3.0 }));

    history.RequestUndo();
    history.Flush(fixture.mWorld, ops);
    CHECK_FALSE(fixture.mScene.mEntities.contains(rootId));
    CHECK_FALSE(fixture.mScene.mInstances.contains(rootId));

    history.RequestRedo();
    history.Flush(fixture.mWorld, ops);
    REQUIRE(fixture.mScene.mEntities.contains(rootId));
    CHECK(fixture.mWorld.IsAlive(fixture.mScene.mEntities.at(rootId)));
    REQUIRE(fixture.mScene.mInstances.contains(rootId));
    CHECK(fixture.mScene.mInstances.at(rootId).mBindings == record.mBindings);
    CHECK(fixture.mScene.mInstances.at(rootId).mPlacement == record.mPlacement);
}

TEST_CASE("Placing a Prefab again keeps its compiled revision", "[app][prefabs]")
{
    Fixture fixture;
    DocumentSceneOps ops(fixture.mWorld, fixture.mScene, *fixture.mDocuments, fixture.mDocument.mId);
    History history;

    history.Submit(mir::MakePlacePrefabCommand("assets/prefabs/Chair.prefab", kNullEntity, mir::WorldVec3(0.0)));
    history.Flush(fixture.mWorld, ops);
    REQUIRE(fixture.mCoordinator->Cache().Find(fixture.mDocument.mId) != nullptr);
    const uint64_t revision = fixture.mCoordinator->Cache().Find(fixture.mDocument.mId)->Revision();

    history.Submit(mir::MakePlacePrefabCommand("assets/prefabs/Chair.prefab", kNullEntity, mir::WorldVec3(1.0)));
    history.Flush(fixture.mWorld, ops);
    CHECK(fixture.mScene.mInstances.size() == 2);
    CHECK(fixture.mCoordinator->Cache().Find(fixture.mDocument.mId)->Revision() == revision);
}

TEST_CASE("Save Scene writes unsaved prefabs first and stops before the scene on failure", "[app][prefabs]")
{
    Fixture fixture;
    TempDir directory;
    const std::filesystem::path path = directory.mPath / (ToString(fixture.mDocument.mId) + ".prefab");
    REQUIRE(mir::WritePrefabFile(path, fixture.mDocument));
    CHECK(fixture.mCoordinator->Registry().Scan(directory.mPath).empty());

    fixture.mScene.mInstances[1].mPrefab = ToString(fixture.mDocument.mId);
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));
    const AuthoredKey key = FieldKey(fixture, SceneNodeAddress{ 2 }, "AppDocumentsTestData", "amount");
    const float value = 42.0f;
    fixture.Run(MakeAuthoredCommand("Edit amount",
        { AuthoredEntry{ key, fixture.mAdapter->EncodeField(FieldKind::Float, &value) } }));
    fixture.Commit();

    bool sceneWritten = false;
    CHECK(fixture.SaveScene([&]()
        {
            sceneWritten = true;
            return ReadText(path).find("42.0") != std::string::npos;
        }));
    CHECK(sceneWritten);
    CHECK_FALSE(fixture.mHistory.IsUnsaved(fixture.mDocumentId));

    const float later = 43.0f;
    fixture.Run(MakeAuthoredCommand("Edit amount",
        { AuthoredEntry{ key, fixture.mAdapter->EncodeField(FieldKind::Float, &later) } }));
    CHECK(fixture.mHistory.IsUnsaved(fixture.mDocumentId));
    std::filesystem::create_directories(path.string() + ".tmp");

    bool wroteScene = false;
    CHECK_FALSE(fixture.SaveScene([&]()
        {
            wroteScene = true;
            return true;
        }));
    CHECK_FALSE(wroteScene);
    CHECK(fixture.mHistory.IsUnsaved(fixture.mDocumentId));
}

TEST_CASE("A Prefab hierarchy reorder uses the drop anchor after removing moved nodes", "[app][prefabs]")
{
    Fixture fixture;
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));
    World &world = *fixture.mDocuments->PreviewWorld(fixture.mDocument.mId);
    const Entity root = fixture.PreviewEntity({ 1 });
    const Entity second = fixture.PreviewEntity({ 2 });
    const Entity third = fixture.PreviewEntity({ 3 });
    fixture.Run(mir::MakeDocumentEntityCommand(mir::EntityAction{
        .mKind = mir::EntityActionKind::Reparent, .mTargets = { second }, .mParent = root, .mBefore = third },
        *fixture.mAdapter, world));
    fixture.Commit();
    const auto children = world.HierarchyIndex().ChildrenOf(root);
    CHECK(std::vector<Entity>(children.begin(), children.end()) == std::vector<Entity>{ second, third });
    CHECK(fixture.mHistory.StepCount() == 0);
}

TEST_CASE("A multi-node Prefab drop preserves order through undo and redo", "[app][prefabs]")
{
    Fixture fixture;
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));
    World &world = *fixture.mDocuments->PreviewWorld(fixture.mDocument.mId);
    const Entity second = fixture.PreviewEntity({ 2 });
    const Entity third = fixture.PreviewEntity({ 3 });
    const Entity fourth = fixture.PreviewEntity({ 4 });
    fixture.Run(mir::MakeDocumentEntityCommand(mir::EntityAction{
        .mKind = mir::EntityActionKind::Reparent, .mTargets = { second, fourth }, .mParent = third },
        *fixture.mAdapter, world));
    fixture.Commit();
    auto children = world.HierarchyIndex().ChildrenOf(third);
    CHECK(std::vector<Entity>(children.begin(), children.end()) == std::vector<Entity>{ second, fourth });
    REQUIRE(fixture.mHistory.StepCount() == 1);
    fixture.Undo();
    fixture.Commit();
    CHECK(ParentOf(world, second) == fixture.PreviewEntity({ 1 }));
    CHECK(ParentOf(world, fourth) == third);
    fixture.Redo();
    fixture.Commit();
    children = world.HierarchyIndex().ChildrenOf(third);
    CHECK(std::vector<Entity>(children.begin(), children.end()) == std::vector<Entity>{ second, fourth });
}

TEST_CASE("Deleting a referenced source node clears source references and remains undoable", "[app][prefabs]")
{
    Fixture fixture;
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));
    const Entity second = fixture.PreviewEntity({ 2 });
    const AuthoredAction deleted = fixture.mAdapter->DeleteNodes(std::vector<Entity>{ second });
    REQUIRE(deleted.mAllowed);
    fixture.Run(MakeAuthoredCommand(deleted.mName, deleted.mEntries));
    fixture.Commit();
    CHECK_FALSE(fixture.mDocuments->PreviewWorld(fixture.mDocument.mId)->IsAlive(second));
    REQUIRE(fixture.mHistory.StepCount() == 1);
    CHECK(FindComponent(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.front(),
        "AppDocumentsTestData")->mFields.at("target").is_null());
    fixture.Undo();
    fixture.Commit();
    CHECK(fixture.mDocuments->PreviewWorld(fixture.mDocument.mId)->IsAlive(fixture.PreviewEntity({ 2 })));
    CHECK(FindComponent(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.front(),
        "AppDocumentsTestData")->mFields.at("target") == json::array({ "2" }));
}

TEST_CASE("A multi-node Prefab drop before a sibling preserves the requested order", "[app][prefabs]")
{
    Fixture fixture;
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));
    World &world = *fixture.mDocuments->PreviewWorld(fixture.mDocument.mId);
    const Entity root = fixture.PreviewEntity({ 1 });
    const Entity second = fixture.PreviewEntity({ 2 });
    const Entity third = fixture.PreviewEntity({ 3 });
    const Entity fourth = fixture.PreviewEntity({ 4 });
    fixture.Run(mir::MakeDocumentEntityCommand(mir::EntityAction{
        .mKind = mir::EntityActionKind::Reparent, .mTargets = { second, fourth }, .mParent = root, .mBefore = third },
        *fixture.mAdapter, world));
    fixture.Commit();
    auto children = world.HierarchyIndex().ChildrenOf(root);
    CHECK(std::vector<Entity>(children.begin(), children.end()) == std::vector<Entity>{ second, fourth, third });
    REQUIRE(fixture.mHistory.StepCount() == 1);
    fixture.Undo();
    fixture.Commit();
    children = world.HierarchyIndex().ChildrenOf(root);
    CHECK(std::vector<Entity>(children.begin(), children.end()) == std::vector<Entity>{ second, third });
    CHECK(ParentOf(world, fourth) == third);
    fixture.Redo();
    fixture.Commit();
    children = world.HierarchyIndex().ChildrenOf(root);
    CHECK(std::vector<Entity>(children.begin(), children.end()) == std::vector<Entity>{ second, fourth, third });
}

TEST_CASE("Opening a captured Prefab previews members with WorldTransform", "[app][prefabs]")
{
    Fixture fixture;
    const Entity root = mir::CreateSceneEntity(fixture.mWorld, fixture.mScene);
    mir::AddTransform(fixture.mWorld, root, mir::Transform(mir::WorldVec3(1.0, 2.0, 3.0)));
    const Entity child = mir::CreateSceneEntity(fixture.mWorld, fixture.mScene, root);
    mir::AddTransform(fixture.mWorld, child, mir::Transform(mir::WorldVec3(4.0, 5.0, 6.0)));

    REQUIRE(fixture.mAdapter->CreatePrefab(root).mAllowed);
    fixture.Run(mir::MakeCreatePrefabCommand(*fixture.mAdapter, root));

    std::optional<PrefabId> created;
    for (const PrefabId id : fixture.mCoordinator->Registry().Ids())
    {
        if (id != fixture.mDocument.mId)
            created = id;
    }
    REQUIRE(created.has_value());
    REQUIRE(fixture.mDocuments->Open(*created));

    World *world = fixture.mDocuments->PreviewWorld(*created);
    const LoadedScene *preview = fixture.mDocuments->PreviewScene(*created);
    REQUIRE(world != nullptr);
    REQUIRE(preview != nullptr);
    REQUIRE(preview->mInstances.size() == 1);

    std::size_t transforms = 0;
    for (const auto &[address, id] : preview->mInstances.begin()->second.mBindings)
    {
        (void)address;
        const Entity member = preview->mEntities.at(id);
        CHECK(world->Has<mir::Transform>(member) == world->Has<mir::WorldTransform>(member));
        if (world->Has<mir::Transform>(member))
            ++transforms;
    }
    CHECK(transforms == 2);
}

TEST_CASE("Deleting a nested occurrence root is one undoable step", "[app][prefabs]")
{
    Fixture fixture;
    const PrefabDocument outer = OuterDocument(fixture.mDocument.mId);
    REQUIRE(fixture.mCoordinator->Registry().SetDocument(outer));
    REQUIRE(fixture.mDocuments->Open(outer.mId));

    const DocumentId outerDocument{ outer.mId.mHigh, outer.mId.mLow };
    const SceneNodeAddress nestedMember{ 2, 2 };
    World *previewWorld = fixture.mDocuments->PreviewWorld(outer.mId);
    const LoadedScene *previewScene = fixture.mDocuments->PreviewScene(outer.mId);
    REQUIRE(previewWorld != nullptr);
    REQUIRE(previewScene != nullptr);
    const StableId previewRoot = previewScene->mInstances.begin()->first;
    const Entity occurrenceRoot =
        previewScene->mEntities.at(previewScene->mInstances.at(previewRoot).mBindings.at(SceneNodeAddress{ 2, 1 }));

    AuthoredKey amount;
    amount.mObject.mDocument = outerDocument;
    amount.mObject.mAddress = nestedMember;
    amount.mComponent = "AppDocumentsTestData";
    amount.mField = "amount";
    fixture.Run(MakeAuthoredCommand("Edit amount", { AuthoredEntry{ amount, std::string("21.0") } }));
    fixture.Commit();

    const LoadedScene &savedPreview = *fixture.mDocuments->PreviewScene(outer.mId);
    const StableId savedRoot = savedPreview.mInstances.begin()->first;
    const Entity savedMember =
        savedPreview.mEntities.at(savedPreview.mInstances.at(savedRoot).mBindings.at(nestedMember));
    CHECK(FloatField(*previewWorld, savedMember, DataOps(), "amount") == 21.0f);

    const AuthoredAction deleted = fixture.mAdapter->DeleteNodes(std::vector<Entity>{ occurrenceRoot });
    REQUIRE(deleted.mAllowed);
    REQUIRE(deleted.mEntries.size() == 1);
    fixture.Run(MakeAuthoredCommand(deleted.mName, deleted.mEntries));
    fixture.Commit();

    const PrefabDocument *document = fixture.mCoordinator->Registry().Find(outer.mId);
    REQUIRE(document != nullptr);
    CHECK(document->mNodes.size() == 1);

    fixture.Undo();
    fixture.Commit();

    document = fixture.mCoordinator->Registry().Find(outer.mId);
    REQUIRE(document != nullptr);
    REQUIRE(document->mNodes.size() == 2);
    const PrefabNode &restored = document->mNodes.back();
    CHECK(restored.mKind == mir::PrefabNodeKind::Prefab);
    CHECK(restored.mPrefab == fixture.mDocument.mId);
    REQUIRE(restored.mPatches.size() == 1);
    CHECK(restored.mPatches.front().mTarget == SceneNodeAddress{ 2 });
    REQUIRE(restored.mPatches.front().mFields.size() == 1);
    CHECK(restored.mPatches.front().mFields.front().mValue == 21.0);

    const LoadedScene &after = *fixture.mDocuments->PreviewScene(outer.mId);
    const StableId rootAfter = after.mInstances.begin()->first;
    const Entity restoredMember = after.mEntities.at(after.mInstances.at(rootAfter).mBindings.at(nestedMember));
    CHECK(FloatField(*previewWorld, restoredMember, DataOps(), "amount") == 21.0f);
}

TEST_CASE("Pasting a nested reference in a Prefab tab keeps the link", "[app][prefabs]")
{
    Fixture fixture;
    const PrefabDocument outer = OuterDocument(fixture.mDocument.mId);
    REQUIRE(fixture.mCoordinator->Registry().SetDocument(outer));
    REQUIRE(fixture.mDocuments->Open(outer.mId));

    const LoadedScene &preview = *fixture.mDocuments->PreviewScene(outer.mId);
    const StableId previewRoot = preview.mInstances.begin()->first;
    const ScenePrefabInstance &instance = preview.mInstances.at(previewRoot);
    const Entity source = preview.mEntities.at(instance.mBindings.at(SceneNodeAddress{ 2, 2 }));
    const Entity destination = preview.mEntities.at(instance.mBindings.at(SceneNodeAddress{ 2, 1 }));

    const std::string encoded = fixture.mAdapter->EncodeField(FieldKind::EntityRef, &source);
    CHECK(json::parse(encoded) == json::array({ "2", "2" }));

    mir::FieldValue value;
    value.mField = static_cast<uint32_t>(DataOps().FindField("target") - DataOps().mFields.data());
    std::memcpy(value.mBytes, &source, sizeof(source));
    CHECK(json::parse(fixture.mAdapter->EncodeField(FieldKind::EntityRef, value.mBytes)) ==
        json::array({ "2", "2" }));
    const std::optional<AuthoredKey> destinationKey =
        fixture.mAdapter->KeyOf(destination, "AppDocumentsTestData", "target");
    REQUIRE(destinationKey.has_value());
    CHECK(destinationKey->mObject.mAddress == SceneNodeAddress{ 2, 1 });
    fixture.Run(MakeSetFieldsEditCommand(destination, DataOps(), { value }, "Paste Values",
        fixture.mAdapter.get()));
    fixture.Commit();

    const PrefabDocument *document = fixture.mCoordinator->Registry().Find(outer.mId);
    REQUIRE(document != nullptr);
    REQUIRE(document->mNodes.size() == 2);
    const PrefabNode &occurrence = document->mNodes.back();
    REQUIRE(occurrence.mPatches.size() == 1);
    CHECK(occurrence.mPatches.front().mTarget == SceneNodeAddress{ 1 });
    REQUIRE(occurrence.mPatches.front().mFields.size() == 1);
    CHECK(occurrence.mPatches.front().mFields.front().mValue == json::array({ "2", "2" }));

    const LoadedScene &after = *fixture.mDocuments->PreviewScene(outer.mId);
    const StableId rootAfter = after.mInstances.begin()->first;
    const ScenePrefabInstance &rebuilt = after.mInstances.at(rootAfter);
    const Entity sourceAfter = after.mEntities.at(rebuilt.mBindings.at(SceneNodeAddress{ 2, 2 }));
    const Entity destinationAfter = after.mEntities.at(rebuilt.mBindings.at(SceneNodeAddress{ 2, 1 }));
    CHECK(EntityField(*fixture.mDocuments->PreviewWorld(outer.mId), destinationAfter, DataOps(), "target") ==
        sourceAfter);
}

TEST_CASE("Undo of removing a nested member's component restores its occurrence overrides", "[app][prefabs]")
{
    Fixture fixture;
    const PrefabDocument outer = OuterDocument(fixture.mDocument.mId);
    REQUIRE(fixture.mCoordinator->Registry().SetDocument(outer));
    REQUIRE(fixture.mDocuments->Open(outer.mId));

    const SceneNodeAddress nestedMember{ 2, 2 };
    const auto member = [&]
        {
            const LoadedScene &preview = *fixture.mDocuments->PreviewScene(outer.mId);
            const StableId root = preview.mInstances.begin()->first;
            return preview.mEntities.at(preview.mInstances.at(root).mBindings.at(nestedMember));
        };
    World &previewWorld = *fixture.mDocuments->PreviewWorld(outer.mId);

    AuthoredKey amount;
    amount.mObject.mDocument = DocumentId{ outer.mId.mHigh, outer.mId.mLow };
    amount.mObject.mAddress = nestedMember;
    amount.mComponent = "AppDocumentsTestData";
    amount.mField = "amount";
    fixture.Run(MakeAuthoredCommand("Edit amount", { AuthoredEntry{ amount, std::string("21.0") } }));
    fixture.Commit();

    fixture.Run(mir::MakeRemoveComponentEditCommand(member(), DataOps(), fixture.mAdapter.get()));
    fixture.Commit();
    CHECK_FALSE(DataOps().Has(previewWorld, member()));

    fixture.Undo();
    fixture.Commit();

    const PrefabDocument *document = fixture.mCoordinator->Registry().Find(outer.mId);
    REQUIRE(document != nullptr);
    const PrefabNode &occurrence = document->mNodes.back();
    REQUIRE(occurrence.mPatches.size() == 1);
    CHECK(occurrence.mPatches.front().mRemovedComponents.empty());
    CHECK(occurrence.mPatches.front().mAddedComponents.empty());
    REQUIRE(occurrence.mPatches.front().mFields.size() == 1);
    CHECK(occurrence.mPatches.front().mFields.front().mValue == 21.0);
    CHECK(FloatField(previewWorld, member(), DataOps(), "amount") == 21.0f);
}

TEST_CASE("Dropping a Prefab in a Prefab tab nests it as one undoable step", "[app][prefabs]")
{
    Fixture fixture;
    PrefabDocument inner = Document();
    REQUIRE(fixture.mCoordinator->Registry().SetDocument(inner));
    REQUIRE(fixture.mCoordinator->Registry().SetIntendedPath(inner.mId,
        fixture.mContentRoot.mPath / "assets" / "prefabs" / "Inner.prefab"));
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));

    World *world = fixture.mDocuments->PreviewWorld(fixture.mDocument.mId);
    REQUIRE(world != nullptr);

    mir::EntityAction action;
    action.mKind = mir::EntityActionKind::PlacePrefab;
    action.mParent = fixture.PreviewEntity(SceneNodeAddress{ 2 });
    action.mPrefabPath = "assets/prefabs/Inner.prefab";
    fixture.Run(mir::MakeDocumentEntityCommand(action, *fixture.mAdapter, *world));
    CHECK(fixture.mHistory.StepCount() == 1);

    const PrefabDocument *document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    REQUIRE(document->mNodes.size() == 5);
    const PrefabNode *occurrence = nullptr;
    for (const PrefabNode &node : document->mNodes)
    {
        if (node.mKind == mir::PrefabNodeKind::Prefab)
            occurrence = &node;
    }
    REQUIRE(occurrence != nullptr);
    const mir::PrefabNodeId newId = occurrence->mId;
    CHECK(occurrence->mPrefab == inner.mId);
    CHECK(occurrence->mParent == 2);
    CHECK(occurrence->mPatches.empty());

    fixture.Commit();
    const SceneNodeAddress nestedRoot{ newId, inner.mRootNode };
    const LoadedScene &preview = fixture.PreviewScene();
    REQUIRE(preview.mInstances.at(fixture.PreviewRoot()).mBindings.contains(nestedRoot));
    CHECK(world->IsAlive(preview.mEntities.at(preview.mInstances.at(fixture.PreviewRoot()).mBindings.at(
        nestedRoot))));

    fixture.Undo();
    fixture.Commit();
    CHECK(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.size() == 4);
    CHECK_FALSE(fixture.PreviewScene().mInstances.at(fixture.PreviewRoot()).mBindings.contains(nestedRoot));

    fixture.Redo();
    fixture.Commit();
    CHECK(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.size() == 5);
    CHECK(fixture.PreviewScene().mInstances.at(fixture.PreviewRoot()).mBindings.contains(nestedRoot));
}

TEST_CASE("A viewport Prefab drop in a Prefab tab writes the inner root position", "[app][prefabs]")
{
    Fixture fixture;
    PrefabDocument inner = Document();
    REQUIRE(fixture.mCoordinator->Registry().SetDocument(inner));
    REQUIRE(fixture.mCoordinator->Registry().SetIntendedPath(inner.mId,
        fixture.mContentRoot.mPath / "assets" / "prefabs" / "Inner.prefab"));
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));

    World *world = fixture.mDocuments->PreviewWorld(fixture.mDocument.mId);
    REQUIRE(world != nullptr);

    const mir::WorldVec3 position(2.0, 3.0, 4.0);
    mir::EntityAction action;
    action.mKind = mir::EntityActionKind::PlacePrefab;
    action.mPrefabPath = "assets/prefabs/Inner.prefab";
    action.mPlacement = position;
    fixture.Run(mir::MakeDocumentEntityCommand(action, *fixture.mAdapter, *world));
    CHECK(fixture.mHistory.StepCount() == 1);

    const PrefabDocument *document = fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    REQUIRE(document != nullptr);
    REQUIRE(document->mNodes.size() == 5);
    const PrefabNode *occurrence = nullptr;
    for (const PrefabNode &node : document->mNodes)
    {
        if (node.mKind == mir::PrefabNodeKind::Prefab)
            occurrence = &node;
    }
    REQUIRE(occurrence != nullptr);
    const mir::PrefabNodeId newId = occurrence->mId;
    CHECK(occurrence->mParent == document->mRootNode);
    REQUIRE(occurrence->mPatches.size() == 1);
    CHECK(occurrence->mPatches.front().mTarget == SceneNodeAddress{ inner.mRootNode });

    fixture.Commit();
    const LoadedScene &preview = fixture.PreviewScene();
    const SceneNodeAddress nestedRoot{ newId, inner.mRootNode };
    REQUIRE(preview.mInstances.at(fixture.PreviewRoot()).mBindings.contains(nestedRoot));
    const Entity root =
        preview.mEntities.at(preview.mInstances.at(fixture.PreviewRoot()).mBindings.at(nestedRoot));
    const mir::Transform *transform = world->GetComponent<mir::Transform>(root);
    REQUIRE(transform != nullptr);
    CHECK(transform->Position() == position);
}

TEST_CASE("A Prefab drop that would nest the active document is refused", "[app][prefabs]")
{
    Fixture fixture;
    const PrefabDocument cycle = OuterDocument(fixture.mDocument.mId);
    REQUIRE(fixture.mCoordinator->Registry().SetDocument(cycle));
    REQUIRE(fixture.mCoordinator->Registry().SetIntendedPath(cycle.mId,
        fixture.mContentRoot.mPath / "assets" / "prefabs" / "Cycle.prefab"));
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));

    World *world = fixture.mDocuments->PreviewWorld(fixture.mDocument.mId);
    REQUIRE(world != nullptr);

    mir::EntityAction action;
    action.mKind = mir::EntityActionKind::PlacePrefab;
    action.mPrefabPath = "assets/prefabs/Cycle.prefab";
    CHECK(mir::MakeDocumentEntityCommand(action, *fixture.mAdapter, *world) == nullptr);
    CHECK(fixture.mHistory.StepCount() == 0);
    CHECK(fixture.mCoordinator->Registry().Find(fixture.mDocument.mId)->mNodes.size() == 4);

    action.mPrefabPath = "assets/prefabs/Missing.prefab";
    CHECK(mir::MakeDocumentEntityCommand(action, *fixture.mAdapter, *world) == nullptr);
    CHECK(fixture.mHistory.StepCount() == 0);
}

TEST_CASE("A Prefab drop on a nested occurrence is refused", "[app][prefabs]")
{
    Fixture fixture;
    PrefabDocument inner = Document();
    REQUIRE(fixture.mCoordinator->Registry().SetDocument(inner));
    REQUIRE(fixture.mCoordinator->Registry().SetIntendedPath(inner.mId,
        fixture.mContentRoot.mPath / "assets" / "prefabs" / "Inner.prefab"));
    const PrefabDocument outer = OuterDocument(inner.mId);
    REQUIRE(fixture.mCoordinator->Registry().SetDocument(outer));
    REQUIRE(fixture.mDocuments->Open(outer.mId));

    World *world = fixture.mDocuments->PreviewWorld(outer.mId);
    REQUIRE(world != nullptr);
    const LoadedScene &preview = *fixture.mDocuments->PreviewScene(outer.mId);
    const StableId root = preview.mInstances.begin()->first;
    const Entity occurrenceRoot =
        preview.mEntities.at(preview.mInstances.at(root).mBindings.at(SceneNodeAddress{ 2, inner.mRootNode }));

    mir::EntityAction action;
    action.mKind = mir::EntityActionKind::PlacePrefab;
    action.mParent = occurrenceRoot;
    action.mPrefabPath = "assets/prefabs/Inner.prefab";
    CHECK(mir::MakeDocumentEntityCommand(action, *fixture.mAdapter, *world) == nullptr);
    CHECK(fixture.mHistory.StepCount() == 0);
    CHECK(fixture.mCoordinator->Registry().Find(outer.mId)->mNodes.size() == 2);

    action.mParent =
        preview.mEntities.at(preview.mInstances.at(root).mBindings.at(SceneNodeAddress{ 2, 2 }));
    CHECK(mir::MakeDocumentEntityCommand(action, *fixture.mAdapter, *world) == nullptr);
    CHECK(fixture.mHistory.StepCount() == 0);
    CHECK(fixture.mCoordinator->Registry().Find(outer.mId)->mNodes.size() == 2);
}

TEST_CASE("External Prefab adoption invalidates saved History before undo redo and Save Scene", "[app][prefabs]")
{
    Fixture fixture;
    TempDir directory;
    const auto path = directory.mPath / "external.prefab";
    REQUIRE(mir::WritePrefabFile(path, fixture.mDocument));
    REQUIRE(fixture.mCoordinator->Registry().Scan(directory.mPath).empty());
    REQUIRE(fixture.mDocuments->Open(fixture.mDocument.mId));
    fixture.mScene.mInstances[1].mPrefab = ToString(fixture.mDocument.mId);
    const AuthoredKey key = FieldKey(fixture, { 2 }, "AppDocumentsTestData", "amount");
    const float value = 42.0f;
    fixture.Run(MakeAuthoredCommand("Edit amount",
        { AuthoredEntry{ key, fixture.mAdapter->EncodeField(FieldKind::Float, &value) } }));
    fixture.Commit();
    REQUIRE(fixture.SavePrefab(fixture.mDocument.mId));
    REQUIRE_FALSE(fixture.mHistory.IsUnsaved(fixture.mDocumentId));
    const DocumentId unrelated{ 123, 456 };
    fixture.mHistory.SetUnsaved(unrelated, false);

    PrefabDocument external = *fixture.mCoordinator->Registry().Find(fixture.mDocument.mId);
    external.mNodes[1].mComponents.front().mFields["amount"] = 72.0;
    REQUIRE(mir::WritePrefabFile(path, external));
    bool deferred = false;
    SECTION("Edit adopts before the next UI History flush") {}
    SECTION("Play defers adoption until the next Edit Commit") { deferred = true; }
    fixture.mCoordinator->SetFrozen(deferred);
    fixture.mCoordinator->RescanSources(directory.mPath);
    auto syncHistory = [&]()
        {
            fixture.mDocuments->InvalidateExternalSavePoints([&](PrefabId id)
                { fixture.mHistory.SetUnsaved(DocumentId{ id.mHigh, id.mLow }, true); });
        };
    if (deferred)
    {
        syncHistory();
        CHECK_FALSE(fixture.mHistory.IsUnsaved(fixture.mDocumentId));
        fixture.mCoordinator->SetFrozen(false);
        fixture.Commit();
    }
    syncHistory();
    CHECK(fixture.mHistory.IsUnsaved(fixture.mDocumentId));
    CHECK_FALSE(fixture.mHistory.IsUnsaved(unrelated));
    fixture.Commit();
    fixture.Undo();
    fixture.Commit();
    fixture.Redo();
    fixture.Commit();
    CHECK(fixture.mHistory.IsUnsaved(fixture.mDocumentId));
    CHECK(fixture.mHistory.StepCount() == 1);
    CHECK(fixture.SaveScene([&]()
        {
            const auto saved = mir::ReadPrefabFile(path);
            return saved.mDocument && saved.mDocument->mNodes[1].mComponents.front().mFields.at("amount") == 42.0;
        }));
    CHECK_FALSE(fixture.mHistory.IsUnsaved(fixture.mDocumentId));
    syncHistory();
    CHECK_FALSE(fixture.mHistory.IsUnsaved(fixture.mDocumentId));
    fixture.Undo();
    fixture.Redo();
    CHECK_FALSE(fixture.mHistory.IsUnsaved(fixture.mDocumentId));
}

TEST_CASE("Save Scene keeps an occupied unsaved Prefab dirty and leaves the other file intact", "[app][prefabs]")
{
    Fixture fixture;
    TempDir directory;
    const auto path = directory.mPath / "Occupied.prefab";
    REQUIRE(fixture.mCoordinator->Registry().SetDocument(fixture.mDocument, path));
    fixture.mHistory.SetUnsaved(fixture.mDocumentId, true);
    fixture.mScene.mInstances[1].mPrefab = ToString(fixture.mDocument.mId);
    REQUIRE(mir::WritePrefabFile(path, mir::NewPrefabDocument()));
    const auto before = ReadText(path);
    bool sceneWritten = false;
    CHECK_FALSE(fixture.SaveScene([&]() { sceneWritten = true; return true; }));
    CHECK_FALSE(sceneWritten);
    CHECK(ReadText(path) == before);
    CHECK(fixture.mHistory.IsUnsaved(fixture.mDocumentId));
    CHECK_FALSE(fixture.mCoordinator->Registry().HasSavedFile(fixture.mDocument.mId));
}

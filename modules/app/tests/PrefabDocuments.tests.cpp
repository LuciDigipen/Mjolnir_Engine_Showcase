/**
 * @file PrefabDocuments.tests.cpp
 * @author Jackey Cen
 * @brief Tests for app/PrefabDocuments.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "../src/app/PrefabAuthoring.h"
#include "../src/app/PrefabDocuments.h"

#include <editor/history/AuthoredEdit.h>
#include <editor/history/Authoring.h>
#include <editor/history/History.h>
#include <editor/history/SceneOps.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/EntityPresetRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/Transform.h>
#include <prefab/PrefabCoordinator.h>
#include <prefab/PrefabFormat.h>
#include <prefab/PrefabSceneInstance.h>
#include <scene/SceneAsset.h>
#include <scene/SceneIO.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
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
    using mir::PrefabDocuments;
    using mir::PrefabId;
    using mir::PrefabNode;
    using mir::PrefabSystem;
    using mir::RuntimeFieldDecl;
    using mir::SceneNodeAddress;
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

    struct Fixture
    {
        World mWorld;
        LoadedScene mScene = NewScene("main");
        PrefabCoordinator *mCoordinator = nullptr;
        std::unique_ptr<PrefabAuthoringAdapter> mAdapter;
        std::unique_ptr<PrefabDocuments> mDocuments;
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
            mDocumentId = DocumentId{ mDocument.mId.mHigh, mDocument.mId.mLow };

            mAdapter = std::make_unique<PrefabAuthoringAdapter>(mWorld, *mCoordinator);
            mHistory.SetAuthoringBackend(mAdapter.get());
            mDocuments = std::make_unique<PrefabDocuments>(*mCoordinator, *mAdapter);
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

        PrefabDocuments::PrefabUnsaved Unsaved() const
        {
            return [this](PrefabId id)
                {
                    return !mCoordinator->Registry().PathOf(id).has_value() ||
                        mHistory.IsUnsaved(DocumentId{ id.mHigh, id.mLow });
                };
        }

        PrefabDocuments::PrefabSaved MarkSaved()
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
    REQUIRE(moved.mEntries.size() == 1);
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
    REQUIRE(DataOps().Has(*previewWorld, createdEntity));
    float amount = 0.0f;
    DataOps().FindField("amount")->Read(DataOps().GetComponent(*previewWorld, createdEntity), &amount);
    CHECK(amount == 7.0f);

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

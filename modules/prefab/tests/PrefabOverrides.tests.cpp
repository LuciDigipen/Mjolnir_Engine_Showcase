/**
 * @file PrefabOverrides.tests.cpp
 * @author Jackey Cen
 * @brief Tests for prefab/PrefabOverrides.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/PrefabOverrides.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/World.h>
#include <prefab/PrefabFormat.h>
#include <prefab/PrefabSceneInstance.h>
#include <scene/SceneInstancePatch.h>
#include <scene/SceneIO.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
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
    using mir::AddInstanceComponent;
    using mir::ApplyPrefabInstancePatches;
    using mir::CompiledPrefab;
    using mir::CompiledPrefabCache;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::CreateSceneEntity;
    using mir::Entity;
    using mir::FieldDesc;
    using mir::FieldKind;
    using mir::FindAddedComponent;
    using mir::FindInstanceFieldPatch;
    using mir::HasPrefabErrors;
    using mir::HasRemovedComponent;
    using mir::InstanceComponentState;
    using mir::InstanceFieldState;
    using mir::kNullStableId;
    using mir::LoadedScene;
    using mir::NewScene;
    using mir::PlacePrefabInstance;
    using mir::PrefabComponentInstance;
    using mir::PrefabDocument;
    using mir::PrefabFieldState;
    using mir::PrefabComponentState;
    using mir::PrefabPatchKind;
    using mir::PrefabPatchSelection;
    using mir::PrefabRegistry;
    using mir::PrefabSceneResolver;
    using mir::RemoveInstanceComponent;
    using mir::RevertInstancePatch;
    using mir::RuntimeFieldDecl;
    using mir::SceneNodeAddress;
    using mir::ScenePrefabInstance;
    using mir::SetInstanceFieldOverride;
    using mir::StableId;
    using mir::World;

    const char *kUuid = "11111111-2222-4333-8444-555555555555";

    const ComponentOps &DataOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "amount", FieldKind::Float },
            { "bonus", FieldKind::Float },
            { "target", FieldKind::EntityRef },
        };
        return ComponentRegistry::Instance().RegisterRuntime("PrefabOverridesTestData", fields);
    }

    const ComponentOps &ExtraOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "value", FieldKind::Float },
            { "link", FieldKind::EntityRef },
        };
        return ComponentRegistry::Instance().RegisterRuntime("PrefabOverridesTestExtra", fields);
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
                ("mir_prefab_overrides_test_" + ProcessToken() + "_" + std::to_string(counter.fetch_add(1)));
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

    json DocumentJson()
    {
        json root;
        root["formatVersion"] = 1;
        root["prefabId"] = kUuid;
        root["nextNodeId"] = "3";
        root["rootNode"] = "1";
        root["nodes"] = json::array({
            json{
                { "id", "1" },
                { "kind", "entity" },
                { "components", json::array({
                    json{ { "type", "PrefabOverridesTestData" }, { "fields", json{
                        { "amount", 2.0 },
                        { "bonus", 3.0 },
                        { "target", json::array({ "2" }) },
                    } } },
                }) },
            },
            json{
                { "id", "2" },
                { "kind", "entity" },
                { "parent", "1" },
                { "components", json::array({
                    json{ { "type", "PrefabOverridesTestData" }, { "fields", json{
                        { "amount", 9.0 },
                        { "bonus", 4.0 },
                        { "target", nullptr },
                    } } },
                }) },
            },
        });
        return root;
    }

    PrefabDocument ParseDocument(const json &root)
    {
        const mir::PrefabParseResult parsed = mir::ParsePrefab(root);
        REQUIRE_FALSE(HasPrefabErrors(parsed.mDiagnostics));
        REQUIRE(parsed.mDocument.has_value());
        return *parsed.mDocument;
    }

    const mir::PrefabNode *FindNode(const PrefabDocument &document, uint64_t id)
    {
        for (const mir::PrefabNode &node : document.mNodes)
        {
            if (node.mId == id)
                return &node;
        }
        return nullptr;
    }

    const PrefabComponentInstance *FindComponent(const mir::PrefabNode &node, std::string_view type)
    {
        for (const PrefabComponentInstance &component : node.mComponents)
        {
            if (component.mType == type)
                return &component;
        }
        return nullptr;
    }

    std::string AddressText(const SceneNodeAddress &address)
    {
        std::string text;
        for (uint64_t segment : address)
        {
            if (!text.empty())
                text += '/';
            text += std::to_string(segment);
        }
        return text;
    }

    std::string PatchFingerprint(const ScenePrefabInstance &instance)
    {
        std::string text;
        for (const mir::SceneInstancePatch &patch : instance.mPatches)
        {
            text += "[" + AddressText(patch.mTarget) + "]";
            for (const mir::SceneFieldPatch &field : patch.mFields)
                text += "F:" + field.mComponent + "." + field.mField + "=" + field.mValue.dump();
            for (const mir::SceneComponentRecord &added : patch.mAddedComponents)
                text += "A:" + added.mType + "=" + added.mFields.dump();
            for (const std::string &removed : patch.mRemovedComponents)
                text += "R:" + removed;
        }
        return text;
    }

    std::string DiagnosticText(const std::vector<mir::PrefabDiagnostic> &diagnostics)
    {
        std::string text;
        for (const mir::PrefabDiagnostic &diagnostic : diagnostics)
            text += diagnostic.mMessage + "\n";
        return text;
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

    float CompiledField(const CompiledPrefabCache &cache, mir::PrefabId id, const SceneNodeAddress &address,
        std::string_view componentType, std::string_view field)
    {
        const std::shared_ptr<const CompiledPrefab> prefab = cache.Find(id);
        REQUIRE(prefab != nullptr);

        for (const mir::CompiledPrefabNode &node : prefab->Nodes())
        {
            if (node.mAddress != address)
                continue;

            for (const mir::CompiledPrefabComponent &component : node.mComponents)
            {
                if (component.mOps->mType.name != componentType)
                    continue;

                const FieldDesc *desc = component.mOps->FindField(field);
                REQUIRE(desc != nullptr);
                float value = 0.0f;
                desc->Read(component.mBytes.data(), &value);
                return value;
            }
        }
        FAIL("compiled field not found");
        return 0.0f;
    }

    struct Fixture
    {
        PrefabDocument mDocument;
        PrefabRegistry mRegistry;
        CompiledPrefabCache mCache;
        World mWorld;
        LoadedScene mScene;
        std::unique_ptr<PrefabSceneResolver> mResolver;
        StableId mRoot = kNullStableId;
        StableId mSecondRoot = kNullStableId;

        explicit Fixture(bool secondInstance = false)
        {
            mir::RegisterCoreComponents();
            DataOps();
            ExtraOps();

            mDocument = ParseDocument(DocumentJson());
            REQUIRE(mRegistry.SetDocument(mDocument));
            REQUIRE_FALSE(HasPrefabErrors(mCache.Compile(mDocument)));
            mResolver = std::make_unique<PrefabSceneResolver>(mRegistry, mCache);

            const std::shared_ptr<const CompiledPrefab> prefab = mCache.Find(mDocument.mId);
            REQUIRE(prefab != nullptr);
            mScene = NewScene("test");
            mRoot = PlacePrefabInstance(mWorld, mScene, *prefab);
            REQUIRE(mRoot != kNullStableId);
            if (secondInstance)
            {
                mSecondRoot = PlacePrefabInstance(mWorld, mScene, *prefab);
                REQUIRE(mSecondRoot != kNullStableId);
            }
        }

        StableId Binding(StableId root, const SceneNodeAddress &address) const
        {
            return mScene.mInstances.at(root).mBindings.at(address);
        }

        Entity Member(StableId root, const SceneNodeAddress &address) const
        {
            return mScene.mEntities.at(Binding(root, address));
        }

        void ChangeTemplateField(const SceneNodeAddress &address, std::string_view component, std::string_view field,
            json value)
        {
            for (mir::PrefabNode &node : mDocument.mNodes)
            {
                if (node.mId != address.front())
                    continue;

                for (PrefabComponentInstance &instance : node.mComponents)
                {
                    if (instance.mType != component)
                        continue;
                    instance.mFields[std::string(field)] = std::move(value);
                }
            }
            REQUIRE(mRegistry.SetDocument(mDocument));
            REQUIRE_FALSE(HasPrefabErrors(mCache.Compile(mDocument)));
        }
    };
}

TEST_CASE("A field override records presence even at the template value", "[prefab][scene]")
{
    Fixture fixture;
    const SceneNodeAddress address{ 1 };

    CHECK(InstanceFieldState(fixture.mScene, fixture.mRoot, address, "PrefabOverridesTestData", "amount") ==
        PrefabFieldState::Inherited);
    CHECK(InstanceFieldState(fixture.mScene, fixture.mRoot, address, "PrefabOverridesTestData", "bonus") ==
        PrefabFieldState::Inherited);

    std::vector<mir::PrefabDiagnostic> diagnostics;
    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, address,
        "PrefabOverridesTestData", "amount", 2.0, &diagnostics));
    CHECK(diagnostics.empty());
    CHECK(InstanceFieldState(fixture.mScene, fixture.mRoot, address, "PrefabOverridesTestData", "amount") ==
        PrefabFieldState::Overridden);
    CHECK(InstanceFieldState(fixture.mScene, fixture.mRoot, address, "PrefabOverridesTestData", "bonus") ==
        PrefabFieldState::Inherited);
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mRoot, address), DataOps(), "amount") == 2.0f);

    TempDir first;
    REQUIRE(mir::SaveScene(fixture.mWorld, first.mPath, fixture.mScene, fixture.mResolver.get()));
    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, first.mPath, fixture.mResolver.get());
    CHECK(InstanceFieldState(loaded, fixture.mRoot, address, "PrefabOverridesTestData", "amount") ==
        PrefabFieldState::Overridden);
    CHECK(FloatField(loadedWorld, loaded.mEntities.at(fixture.mRoot), DataOps(), "amount") == 2.0f);
    CHECK(FloatField(loadedWorld, loaded.mEntities.at(fixture.mRoot), DataOps(), "bonus") == 3.0f);

    TempDir second;
    REQUIRE(mir::SaveScene(loadedWorld, second.mPath, loaded, fixture.mResolver.get()));

    fixture.ChangeTemplateField(address, "PrefabOverridesTestData", "amount", 7.0);
    fixture.ChangeTemplateField(address, "PrefabOverridesTestData", "bonus", 8.0);

    World changedWorld;
    const LoadedScene changed = mir::LoadScene(changedWorld, second.mPath, fixture.mResolver.get());
    CHECK(InstanceFieldState(changed, fixture.mRoot, address, "PrefabOverridesTestData", "amount") ==
        PrefabFieldState::Overridden);
    CHECK(FloatField(changedWorld, changed.mEntities.at(fixture.mRoot), DataOps(), "amount") == 2.0f);
    CHECK(FloatField(changedWorld, changed.mEntities.at(fixture.mRoot), DataOps(), "bonus") == 8.0f);
}

TEST_CASE("A reference override equal to the template target stays overridden", "[prefab][scene]")
{
    Fixture fixture;
    const SceneNodeAddress address{ 1 };
    const StableId target = fixture.Binding(fixture.mRoot, SceneNodeAddress{ 2 });

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, address,
        "PrefabOverridesTestData", "target", target));
    CHECK(InstanceFieldState(fixture.mScene, fixture.mRoot, address, "PrefabOverridesTestData", "target") ==
        PrefabFieldState::Overridden);
    CHECK(EntityField(fixture.mWorld, fixture.Member(fixture.mRoot, address), DataOps(), "target") ==
        fixture.Member(fixture.mRoot, SceneNodeAddress{ 2 }));

    TempDir first;
    REQUIRE(mir::SaveScene(fixture.mWorld, first.mPath, fixture.mScene, fixture.mResolver.get()));
    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, first.mPath, fixture.mResolver.get());
    CHECK(InstanceFieldState(loaded, fixture.mRoot, address, "PrefabOverridesTestData", "target") ==
        PrefabFieldState::Overridden);
    CHECK(EntityField(loadedWorld, loaded.mEntities.at(fixture.mRoot), DataOps(), "target") ==
        loaded.mEntities.at(target));

    TempDir second;
    REQUIRE(mir::SaveScene(loadedWorld, second.mPath, loaded, fixture.mResolver.get()));

    fixture.ChangeTemplateField(address, "PrefabOverridesTestData", "target", nullptr);
    World changedWorld;
    const LoadedScene changed = mir::LoadScene(changedWorld, second.mPath, fixture.mResolver.get());
    CHECK(InstanceFieldState(changed, fixture.mRoot, address, "PrefabOverridesTestData", "target") ==
        PrefabFieldState::Overridden);
    CHECK(EntityField(changedWorld, changed.mEntities.at(fixture.mRoot), DataOps(), "target") ==
        changed.mEntities.at(target));
}

TEST_CASE("Local components and removed inherited components stay distinct", "[prefab][scene]")
{
    Fixture fixture;
    const SceneNodeAddress rootAddress{ 1 };
    const SceneNodeAddress childAddress{ 2 };

    CHECK(InstanceComponentState(fixture.mRegistry, fixture.mScene, fixture.mRoot, rootAddress,
        "PrefabOverridesTestExtra") == PrefabComponentState::Absent);

    REQUIRE(AddInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
        "PrefabOverridesTestExtra"));
    CHECK(InstanceComponentState(fixture.mRegistry, fixture.mScene, fixture.mRoot, rootAddress,
        "PrefabOverridesTestExtra") == PrefabComponentState::Added);
    CHECK(ExtraOps().Has(fixture.mWorld, fixture.Member(fixture.mRoot, rootAddress)));

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
        "PrefabOverridesTestExtra", "value", 4.5));
    const ScenePrefabInstance &instance = fixture.mScene.mInstances.at(fixture.mRoot);
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mRoot, rootAddress), ExtraOps(), "value") == 4.5f);
    CHECK(FindInstanceFieldPatch(instance, rootAddress, "PrefabOverridesTestExtra", "value") == nullptr);
    REQUIRE(FindAddedComponent(instance, rootAddress, "PrefabOverridesTestExtra") != nullptr);
    CHECK(FindAddedComponent(instance, rootAddress, "PrefabOverridesTestExtra")->mFields.at("value") == 4.5);

    REQUIRE(RemoveInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
        "PrefabOverridesTestExtra"));
    CHECK(InstanceComponentState(fixture.mRegistry, fixture.mScene, fixture.mRoot, rootAddress,
        "PrefabOverridesTestExtra") == PrefabComponentState::Absent);
    CHECK_FALSE(ExtraOps().Has(fixture.mWorld, fixture.Member(fixture.mRoot, rootAddress)));
    CHECK_FALSE(HasRemovedComponent(instance, rootAddress, "PrefabOverridesTestExtra"));

    REQUIRE(AddInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
        "PrefabOverridesTestExtra"));
    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
        "PrefabOverridesTestExtra", "value", 4.5));

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, childAddress,
        "PrefabOverridesTestData", "amount", 11.0));
    CHECK(InstanceComponentState(fixture.mRegistry, fixture.mScene, fixture.mRoot, childAddress,
        "PrefabOverridesTestData") == PrefabComponentState::Inherited);
    REQUIRE(RemoveInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, childAddress,
        "PrefabOverridesTestData"));
    CHECK(InstanceComponentState(fixture.mRegistry, fixture.mScene, fixture.mRoot, childAddress,
        "PrefabOverridesTestData") == PrefabComponentState::Removed);
    CHECK_FALSE(DataOps().Has(fixture.mWorld, fixture.Member(fixture.mRoot, childAddress)));
    CHECK(HasRemovedComponent(instance, childAddress, "PrefabOverridesTestData"));
    CHECK(FindInstanceFieldPatch(instance, childAddress, "PrefabOverridesTestData", "amount") == nullptr);

    std::vector<mir::PrefabDiagnostic> diagnostics;
    CHECK_FALSE(AddInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
        "PrefabOverridesTestData", &diagnostics));
    CHECK_FALSE(diagnostics.empty());
    CHECK(InstanceComponentState(fixture.mRegistry, fixture.mScene, fixture.mRoot, rootAddress,
        "PrefabOverridesTestData") == PrefabComponentState::Inherited);

    diagnostics.clear();
    CHECK_FALSE(AddInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, childAddress,
        "PrefabOverridesTestData", &diagnostics));
    CHECK_FALSE(diagnostics.empty());
    CHECK(InstanceComponentState(fixture.mRegistry, fixture.mScene, fixture.mRoot, childAddress,
        "PrefabOverridesTestData") == PrefabComponentState::Removed);

    TempDir dir;
    REQUIRE(mir::SaveScene(fixture.mWorld, dir.mPath, fixture.mScene, fixture.mResolver.get()));
    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath, fixture.mResolver.get());
    CHECK(InstanceComponentState(fixture.mRegistry, loaded, fixture.mRoot, rootAddress,
        "PrefabOverridesTestExtra") == PrefabComponentState::Added);
    CHECK(InstanceComponentState(fixture.mRegistry, loaded, fixture.mRoot, childAddress,
        "PrefabOverridesTestData") == PrefabComponentState::Removed);
    const Entity loadedRoot = loaded.mEntities.at(fixture.mRoot);
    CHECK(ExtraOps().Has(loadedWorld, loadedRoot));
    CHECK(FloatField(loadedWorld, loadedRoot, ExtraOps(), "value") == 4.5f);
    CHECK_FALSE(DataOps().Has(loadedWorld, loaded.mEntities.at(fixture.Binding(fixture.mRoot, childAddress))));
    CHECK_FALSE(HasRemovedComponent(loaded.mInstances.at(fixture.mRoot), rootAddress, "PrefabOverridesTestExtra"));
}

TEST_CASE("Revert removes one selected patch and reveals the template", "[prefab][scene]")
{
    Fixture fixture;
    const SceneNodeAddress rootAddress{ 1 };
    const SceneNodeAddress childAddress{ 2 };
    const PrefabRegistry &registry = fixture.mRegistry;

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
        "PrefabOverridesTestData", "amount", 5.0));
    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
        "PrefabOverridesTestData", "target", StableId{ 0 }));
    fixture.ChangeTemplateField(childAddress, "PrefabOverridesTestData", "target", json::array({ "1" }));
    REQUIRE(RemoveInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, childAddress,
        "PrefabOverridesTestData"));
    REQUIRE(AddInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
        "PrefabOverridesTestExtra"));
    fixture.ChangeTemplateField(rootAddress, "PrefabOverridesTestData", "amount", 6.0);

    std::vector<mir::PrefabDiagnostic> diagnostics;
    fixture.mScene.mDirty = false;
    REQUIRE(RevertInstancePatch(fixture.mWorld, fixture.mRegistry, fixture.mCache, fixture.mScene, fixture.mRoot,
        PrefabPatchSelection{ rootAddress, PrefabPatchKind::Field, "PrefabOverridesTestData", "amount" },
        &diagnostics));
    CHECK(diagnostics.empty());
    CHECK(fixture.mScene.mDirty);
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mRoot, rootAddress), DataOps(), "amount") == 6.0f);
    CHECK(InstanceFieldState(fixture.mScene, fixture.mRoot, rootAddress, "PrefabOverridesTestData", "amount") ==
        PrefabFieldState::Inherited);
    CHECK(InstanceFieldState(fixture.mScene, fixture.mRoot, rootAddress, "PrefabOverridesTestData", "target") ==
        PrefabFieldState::Overridden);
    CHECK(InstanceComponentState(registry, fixture.mScene, fixture.mRoot, rootAddress,
        "PrefabOverridesTestExtra") == PrefabComponentState::Added);
    CHECK(InstanceComponentState(registry, fixture.mScene, fixture.mRoot, childAddress,
        "PrefabOverridesTestData") == PrefabComponentState::Removed);

    fixture.mScene.mDirty = false;
    diagnostics.clear();
    REQUIRE(RevertInstancePatch(fixture.mWorld, fixture.mRegistry, fixture.mCache, fixture.mScene, fixture.mRoot,
        PrefabPatchSelection{ rootAddress, PrefabPatchKind::Field, "PrefabOverridesTestData", "target" },
        &diagnostics));
    CHECK(fixture.mScene.mDirty);
    CHECK(EntityField(fixture.mWorld, fixture.Member(fixture.mRoot, rootAddress), DataOps(), "target") ==
        fixture.Member(fixture.mRoot, childAddress));
    CHECK(InstanceFieldState(fixture.mScene, fixture.mRoot, rootAddress, "PrefabOverridesTestData", "target") ==
        PrefabFieldState::Inherited);
    CHECK(InstanceComponentState(registry, fixture.mScene, fixture.mRoot, rootAddress,
        "PrefabOverridesTestExtra") == PrefabComponentState::Added);

    fixture.mScene.mDirty = false;
    diagnostics.clear();
    REQUIRE(RevertInstancePatch(fixture.mWorld, fixture.mRegistry, fixture.mCache, fixture.mScene, fixture.mRoot,
        PrefabPatchSelection{ childAddress, PrefabPatchKind::RemovedComponent, "PrefabOverridesTestData", {} },
        &diagnostics));
    CHECK(fixture.mScene.mDirty);
    CHECK(DataOps().Has(fixture.mWorld, fixture.Member(fixture.mRoot, childAddress)));
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mRoot, childAddress), DataOps(), "amount") == 9.0f);
    CHECK(InstanceComponentState(registry, fixture.mScene, fixture.mRoot, childAddress,
        "PrefabOverridesTestData") == PrefabComponentState::Inherited);
    CHECK(EntityField(fixture.mWorld, fixture.Member(fixture.mRoot, childAddress), DataOps(), "target") ==
        fixture.Member(fixture.mRoot, rootAddress));
    CHECK(FindInstanceFieldPatch(fixture.mScene.mInstances.at(fixture.mRoot), childAddress,
        "PrefabOverridesTestData", "amount") == nullptr);

    fixture.mScene.mDirty = false;
    diagnostics.clear();
    REQUIRE(RevertInstancePatch(fixture.mWorld, fixture.mRegistry, fixture.mCache, fixture.mScene, fixture.mRoot,
        PrefabPatchSelection{ rootAddress, PrefabPatchKind::AddedComponent, "PrefabOverridesTestExtra", {} },
        &diagnostics));
    CHECK(fixture.mScene.mDirty);
    CHECK_FALSE(ExtraOps().Has(fixture.mWorld, fixture.Member(fixture.mRoot, rootAddress)));
    CHECK(InstanceComponentState(registry, fixture.mScene, fixture.mRoot, rootAddress,
        "PrefabOverridesTestExtra") == PrefabComponentState::Absent);

    fixture.mScene.mDirty = false;
    diagnostics.clear();
    CHECK_FALSE(RevertInstancePatch(fixture.mWorld, fixture.mRegistry, fixture.mCache, fixture.mScene, fixture.mRoot,
        PrefabPatchSelection{ rootAddress, PrefabPatchKind::Field, "PrefabOverridesTestData", "amount" },
        &diagnostics));
    CHECK_FALSE(fixture.mScene.mDirty);
}

TEST_CASE("Apply writes selected patches and leaves the rest", "[prefab][scene]")
{
    Fixture fixture(true);
    const SceneNodeAddress rootAddress{ 1 };
    const SceneNodeAddress childAddress{ 2 };

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
        "PrefabOverridesTestData", "amount", 5.0));
    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
        "PrefabOverridesTestData", "bonus", 6.0));
    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
        "PrefabOverridesTestData", "target", fixture.Binding(fixture.mRoot, childAddress)));
    REQUIRE(AddInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
        "PrefabOverridesTestExtra"));
    REQUIRE(RemoveInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, childAddress,
        "PrefabOverridesTestData"));
    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mSecondRoot,
        rootAddress, "PrefabOverridesTestData", "amount", 7.0));

    TempDir prefabs;
    REQUIRE(mir::WritePrefabFile(prefabs.mPath / "instance.prefab", fixture.mDocument));
    CHECK(fixture.mRegistry.Scan(prefabs.mPath).empty());
    const std::map<std::string, std::string> filesBefore = SnapshotFiles(prefabs.mPath);
    const uint64_t revisionBefore = fixture.mCache.Find(fixture.mDocument.mId)->Revision();

    const std::vector<PrefabPatchSelection> selections{
        { rootAddress, PrefabPatchKind::Field, "PrefabOverridesTestData", "amount" },
        { rootAddress, PrefabPatchKind::Field, "PrefabOverridesTestData", "target" },
        { rootAddress, PrefabPatchKind::AddedComponent, "PrefabOverridesTestExtra", {} },
        { childAddress, PrefabPatchKind::RemovedComponent, "PrefabOverridesTestData", {} },
    };
    const mir::PrefabApplyResult result = ApplyPrefabInstancePatches(fixture.mScene, fixture.mRegistry, fixture.mCache,
        fixture.mRoot, selections);
    REQUIRE(result.mApplied);
    CHECK_FALSE(HasPrefabErrors(result.mDiagnostics));

    const PrefabDocument *applied = fixture.mRegistry.Find(fixture.mDocument.mId);
    REQUIRE(applied != nullptr);
    const mir::PrefabNode *rootNode = FindNode(*applied, 1);
    REQUIRE(rootNode != nullptr);
    const PrefabComponentInstance *data = FindComponent(*rootNode, "PrefabOverridesTestData");
    REQUIRE(data != nullptr);
    CHECK(data->mFields.at("amount") == 5.0);
    CHECK(data->mFields.at("bonus") == 3.0);
    CHECK(data->mFields.at("target") == json::array({ "2" }));
    CHECK(FindComponent(*rootNode, "PrefabOverridesTestExtra") != nullptr);
    const mir::PrefabNode *childNode = FindNode(*applied, 2);
    REQUIRE(childNode != nullptr);
    CHECK(FindComponent(*childNode, "PrefabOverridesTestData") == nullptr);

    const std::shared_ptr<const CompiledPrefab> compiled = fixture.mCache.Find(fixture.mDocument.mId);
    REQUIRE(compiled != nullptr);
    CHECK(compiled->Revision() > revisionBefore);
    CHECK(CompiledField(fixture.mCache, fixture.mDocument.mId, rootAddress, "PrefabOverridesTestData", "amount") ==
        5.0f);

    const ScenePrefabInstance &instance = fixture.mScene.mInstances.at(fixture.mRoot);
    CHECK(FindInstanceFieldPatch(instance, rootAddress, "PrefabOverridesTestData", "amount") == nullptr);
    CHECK(FindInstanceFieldPatch(instance, rootAddress, "PrefabOverridesTestData", "target") == nullptr);
    CHECK(FindInstanceFieldPatch(instance, rootAddress, "PrefabOverridesTestData", "bonus") != nullptr);
    CHECK(FindAddedComponent(instance, rootAddress, "PrefabOverridesTestExtra") == nullptr);
    CHECK_FALSE(HasRemovedComponent(instance, childAddress, "PrefabOverridesTestData"));
    CHECK(fixture.mScene.mDirty);

    const ScenePrefabInstance &second = fixture.mScene.mInstances.at(fixture.mSecondRoot);
    REQUIRE(FindInstanceFieldPatch(second, rootAddress, "PrefabOverridesTestData", "amount") != nullptr);
    CHECK(FindInstanceFieldPatch(second, rootAddress, "PrefabOverridesTestData", "amount")->mValue == 7.0);
    CHECK(SnapshotFiles(prefabs.mPath) == filesBefore);

    fixture.mScene.mDirty = false;
    REQUIRE(RevertInstancePatch(fixture.mWorld, fixture.mRegistry, fixture.mCache, fixture.mScene, fixture.mSecondRoot,
        PrefabPatchSelection{ rootAddress, PrefabPatchKind::Field, "PrefabOverridesTestData", "amount" }));
    CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mSecondRoot, rootAddress), DataOps(), "amount") == 5.0f);
}

TEST_CASE("Apply rejects a batch with a scene-dependent or invalid entry", "[prefab][scene]")
{
    Fixture fixture(true);
    const SceneNodeAddress rootAddress{ 1 };
    const SceneNodeAddress childAddress{ 2 };
    const Entity ordinary = CreateSceneEntity(fixture.mWorld, fixture.mScene);
    const StableId ordinaryId = fixture.mScene.mNextId - 1;
    const std::vector<PrefabPatchSelection> validSelection{
        { rootAddress, PrefabPatchKind::Field, "PrefabOverridesTestData", "amount" },
    };

    REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
        "PrefabOverridesTestData", "amount", 5.0));

    TempDir prefabs;
    REQUIRE(mir::WritePrefabFile(prefabs.mPath / "instance.prefab", fixture.mDocument));
    CHECK(fixture.mRegistry.Scan(prefabs.mPath).empty());

    const auto expectRejected = [&](const std::vector<PrefabPatchSelection> &selections, std::string_view needle)
        {
            const json documentBefore = mir::PrefabToJson(*fixture.mRegistry.Find(fixture.mDocument.mId));
            const uint64_t revisionBefore = fixture.mCache.Find(fixture.mDocument.mId)->Revision();
            const std::string patchesBefore =
                PatchFingerprint(fixture.mScene.mInstances.at(fixture.mRoot));
            const std::map<std::string, std::string> filesBefore = SnapshotFiles(prefabs.mPath);
            const float worldAmount =
                FloatField(fixture.mWorld, fixture.Member(fixture.mRoot, rootAddress), DataOps(), "amount");
            const Entity worldTarget =
                EntityField(fixture.mWorld, fixture.Member(fixture.mRoot, rootAddress), DataOps(), "target");
            fixture.mScene.mDirty = false;

            const mir::PrefabApplyResult result = ApplyPrefabInstancePatches(fixture.mScene, fixture.mRegistry,
                fixture.mCache, fixture.mRoot, selections);

            CHECK_FALSE(result.mApplied);
            CHECK_FALSE(fixture.mScene.mDirty);
            const std::string diagnostics = DiagnosticText(result.mDiagnostics);
            CHECK(diagnostics.find(needle) != std::string::npos);
            CHECK(mir::PrefabToJson(*fixture.mRegistry.Find(fixture.mDocument.mId)) == documentBefore);
            CHECK(fixture.mCache.Find(fixture.mDocument.mId)->Revision() == revisionBefore);
            CHECK(PatchFingerprint(fixture.mScene.mInstances.at(fixture.mRoot)) == patchesBefore);
            CHECK(SnapshotFiles(prefabs.mPath) == filesBefore);
            CHECK(FloatField(fixture.mWorld, fixture.Member(fixture.mRoot, rootAddress), DataOps(), "amount") ==
                worldAmount);
            CHECK(EntityField(fixture.mWorld, fixture.Member(fixture.mRoot, rootAddress), DataOps(), "target") ==
                worldTarget);
        };

    SECTION("An ordinary scene entity is scene-dependent")
    {
        REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
            "PrefabOverridesTestData", "target", ordinaryId));
        CHECK(EntityField(fixture.mWorld, fixture.Member(fixture.mRoot, rootAddress), DataOps(), "target") ==
            ordinary);
        std::vector<PrefabPatchSelection> selections = validSelection;
        selections.push_back({ rootAddress, PrefabPatchKind::Field, "PrefabOverridesTestData", "target" });
        expectRejected(selections, "PrefabOverridesTestData.target");
    }
    SECTION("Another instance's member is scene-dependent")
    {
        REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
            "PrefabOverridesTestData", "target", fixture.Binding(fixture.mSecondRoot, childAddress)));
        std::vector<PrefabPatchSelection> selections = validSelection;
        selections.push_back({ rootAddress, PrefabPatchKind::Field, "PrefabOverridesTestData", "target" });
        expectRejected(selections, "PrefabOverridesTestData.target");
    }
    SECTION("A selection without a patch rejects the batch")
    {
        std::vector<PrefabPatchSelection> selections = validSelection;
        selections.push_back({ rootAddress, PrefabPatchKind::Field, "PrefabOverridesTestData", "bonus" });
        expectRejected(selections, "has no field override");
    }
    SECTION("A local component targeting a scene entity is scene-dependent")
    {
        REQUIRE(AddInstanceComponent(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot, rootAddress,
            "PrefabOverridesTestExtra"));
        REQUIRE(SetInstanceFieldOverride(fixture.mWorld, fixture.mScene, fixture.mRegistry, fixture.mRoot,
            rootAddress, "PrefabOverridesTestExtra", "link", ordinaryId));

        std::vector<PrefabPatchSelection> selections = validSelection;
        selections.push_back({ rootAddress, PrefabPatchKind::AddedComponent, "PrefabOverridesTestExtra", {} });
        expectRejected(selections, "added component \"PrefabOverridesTestExtra\"");
    }
    SECTION("An unknown field rejects the batch")
    {
        mir::SceneInstancePatch &patch =
            mir::GetOrAddInstancePatch(fixture.mScene.mInstances.at(fixture.mRoot), rootAddress);
        patch.mFields.push_back(mir::SceneFieldPatch{ "PrefabOverridesTestData", "missing", 1.0 });

        std::vector<PrefabPatchSelection> selections = validSelection;
        selections.push_back({ rootAddress, PrefabPatchKind::Field, "PrefabOverridesTestData", "missing" });
        expectRejected(selections, "PrefabOverridesTestData.missing");
    }
    SECTION("Adding a component the document already has rejects the batch")
    {
        mir::SceneInstancePatch &patch =
            mir::GetOrAddInstancePatch(fixture.mScene.mInstances.at(fixture.mRoot), rootAddress);
        patch.mAddedComponents.push_back(mir::SceneComponentRecord{ "PrefabOverridesTestData", json::object() });

        std::vector<PrefabPatchSelection> selections = validSelection;
        selections.push_back({ rootAddress, PrefabPatchKind::AddedComponent, "PrefabOverridesTestData", {} });
        expectRejected(selections, "already has");
    }
    SECTION("Removing a component the document lacks rejects the batch")
    {
        mir::SceneInstancePatch &patch =
            mir::GetOrAddInstancePatch(fixture.mScene.mInstances.at(fixture.mRoot), rootAddress);
        patch.mRemovedComponents.push_back("PrefabOverridesTestExtra");

        std::vector<PrefabPatchSelection> selections = validSelection;
        selections.push_back({ rootAddress, PrefabPatchKind::RemovedComponent, "PrefabOverridesTestExtra", {} });
        expectRejected(selections, "does not have");
    }
}

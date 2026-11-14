/**
 * @file SurfaceMaterialAsync.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/SurfaceMaterialLibrary asynchronous loads, replacements and publication.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/SurfaceMaterialLibrary.h>

#include <assets/AssetBlob.h>
#include <assets/AssetManifest.h>
#include <assets/AssetResolver.h>
#include <assets/AssetsModule.h>
#include <assets/ModelAsset.h>
#include <core/fs/FileIO.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentFieldChanges.h>
#include <core/ecs/SceneLoadGroup.h>
#include <core/ecs/TypeId.h>
#include <core/ecs/World.h>
#include <jobs/JobExecutor.h>
#include <renderer/MeshResolveSystem.h>
#include <renderer/components/MeshRenderer.h>

#include "support/CookedMeshDir.h"
#include "support/FakeMeshResolveHost.h"
#include "support/FakeSurfaceMaterialHost.h"
#include "support/FakeTextureUploader.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <cstdio>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace mir;

namespace
{
    const AssetId kMaterial = MakeAssetId("material");
    const AssetId kTexture = MakeAssetId("texture");
    const std::string kLogicalPath = "assets/test.material.json";

    struct CookedSurface : test::CookedDir
    {
        void Write(const SurfaceMaterialAsset &material, std::span<const AssetId> dependencies = {},
            uint8_t textureValue = 80)
        {
            ++mSerial;
            std::vector<AssetManifestSourceEntry> entries;
            std::string textureFile;
            if (!dependencies.empty())
            {
                TextureAsset texture;
                texture.width = texture.height = 4;
                texture.rgba.assign(64, textureValue);
                textureFile = std::format("texture-{:016x}.blob", uint64_t(textureValue));
                REQUIRE(WriteBytes(Path() / textureFile, BuildTypedAsset(texture)));
                entries.push_back({ { kTexture, kTextureAssetTypeTag, kTextureAssetContentVersion },
                    textureFile, "texture", {} });
            }
            const std::string file = std::format("material-{:016x}.blob", mSerial);
            REQUIRE(WriteBytes(Path() / file, BuildTypedAsset(material)));
            entries.push_back({ { kMaterial, kSurfaceMaterialAssetTypeTag, kSurfaceMaterialAssetContentVersion },
                file, kLogicalPath, dependencies });
            REQUIRE(WriteBytes(Path() / kAssetManifestFileName, BuildAssetManifestBlob(entries)));
        }

        void WriteBroken()
        {
            ++mSerial;
            const std::string file = std::format("material-{:016x}.blob", mSerial);
            const AssetManifestSourceEntry entry{ { kMaterial, kSurfaceMaterialAssetTypeTag,
                                                      kSurfaceMaterialAssetContentVersion },
                file, kLogicalPath, {} };
            REQUIRE(WriteBytes(Path() / kAssetManifestFileName, BuildAssetManifestBlob(std::span(&entry, 1))));
        }

        uint64_t mSerial = 0;
    };

    SurfaceMaterialAsset TestMaterial(std::string parameters = R"({"tint":0.25})")
    {
        SurfaceMaterialAsset material;
        material.shaderFile = "assets/test.slang";
        material.shaderType = "TestMaterial";
        material.frontends = { "mesh" };
        material.parametersJson = std::move(parameters);
        return material;
    }

    SurfaceMaterialRegistration MakeSurfaceRegistration(std::string frontend, std::string stem,
        std::string asset = kLogicalPath)
    {
        SurfaceMaterialRegistration registration;
        registration.stem = std::move(stem);
        registration.type = "TestMaterial";
        registration.frontend = std::move(frontend);
        registration.asset = std::move(asset);
        registration.shader = "assets/test.slang";
        registration.parametersJson = R"({"tint":0.5})";
        registration.reflectionFile = "test.layout.reflection.json";
        return registration;
    }

    MaterialLayout TintLayout()
    {
        MaterialLayout layout;
        layout.size = 16;
        layout.fields.push_back({ .name = "tint", .kind = MaterialFieldKind::Float, .offset = 0, .size = 4 });
        return layout;
    }

    MaterialLayout TextureLayout()
    {
        MaterialLayout layout = TintLayout();
        layout.fields.push_back({ .name = "albedo", .kind = MaterialFieldKind::TextureRef, .offset = 8, .size = 8 });
        return layout;
    }

    float TintOf(const MaterialRecordSnapshot &snapshot)
    {
        float value = 0.0f;
        if (snapshot.mBytes.size() >= sizeof(value))
            std::memcpy(&value, snapshot.mBytes.data(), sizeof(value));
        return value;
    }

    struct Fixture : CookedSurface
    {
        Fixture()
            : mAssets(Path()), mJobs(0), mService(mAssets, mJobs, 1)
        {
            mHost.schemaLayout = TintLayout();
            mHost.catalog.materials.push_back(MakeSurfaceRegistration("mesh", "test_mesh"));
            mLibrary.Bind(&mService, mHost, mUploader);
        }
        void Frame()
        {
            mService.DrainCompletions();
            mLibrary.Advance();
        }
        void Cook(const SurfaceMaterialAsset &material, std::span<const AssetId> dependencies = {},
            uint8_t textureValue = 80)
        {
            Write(material, dependencies, textureValue);
            mAssets.BeginReload();
            mAssets.CommitReload();
        }
        void CookBroken()
        {
            WriteBroken();
            mAssets.BeginReload();
            mAssets.CommitReload();
        }
        void Use(std::initializer_list<std::string_view> frontends)
        {
            if (mOwner == 0)
                mOwner = mLibrary.AddMaterialUseOwner();
            std::vector<SurfaceMaterialUse> uses;
            for (const std::string_view frontend : frontends)
                uses.push_back({ kLogicalPath, kMaterial, std::string(frontend) });
            mLibrary.SetMaterialUses(mOwner, uses);
        }
        SurfaceMaterialResolution Resolve(std::string_view frontend = "mesh")
        {
            return mLibrary.Resolve(kLogicalPath, kMaterial, frontend);
        }
        SurfaceMaterialResolution Settle(std::string_view frontend = "mesh", int frames = 64)
        {
            SurfaceMaterialResolution resolution = Resolve(frontend);
            for (int frame = 0; frame < frames && resolution.status == SurfaceMaterialStatus::Pending; ++frame)
            {
                Frame();
                resolution = Resolve(frontend);
            }
            return resolution;
        }
        void SettlePair(std::string_view first, std::string_view second, SurfaceMaterialResolution &a,
            SurfaceMaterialResolution &b, int frames = 64)
        {
            a = Resolve(first);
            b = Resolve(second);
            for (int frame = 0; frame < frames &&
                 (a.status == SurfaceMaterialStatus::Pending || b.status == SurfaceMaterialStatus::Pending); ++frame)
            {
                Frame();
                a = Resolve(first);
                b = Resolve(second);
            }
        }
        bool CompleteTexture()
        {
            for (const auto &[index, entry] : mUploader.mEntries)
            {
                const TextureHandle handle{ index, 1 };
                mUploader.Begin(handle);
                mUploader.CopyAll(handle);
                mUploader.Complete(handle);
                return true;
            }
            return false;
        }
        std::optional<MaterialRecordSnapshot> RecordFor(std::string_view stem) const
        {
            for (std::size_t i = mHost.createdMaterials.size(); i-- > 0;)
                if (mHost.createdMaterials[i].shaderName == stem)
                    return mHost.records.at(static_cast<uint32_t>(i));
            return std::nullopt;
        }

        AssetsModule mAssets;
        JobExecutor mJobs;
        AsyncAssetService mService;
        test::FakeTextureUploader mUploader;
        test::FakeSurfaceMaterialHost mHost;
        SurfaceMaterialLibrary mLibrary;
        uint64_t mOwner = 0;
    };

    // Several cooked material assets in one manifest, so tests can move one asset's
    // content without moving another's.
    struct MaterialCookedDir : test::CookedDir
    {
        void Set(const std::string &logicalPath, const SurfaceMaterialAsset &material)
        {
            Entry &entry = mEntries[logicalPath];
            entry.file = std::format("material-{:016x}.blob", ++mSerial);
            entry.typeTag = kSurfaceMaterialAssetTypeTag;
            entry.contentVersion = kSurfaceMaterialAssetContentVersion;
            REQUIRE(WriteBytes(Path() / entry.file, BuildTypedAsset(material)));
            Publish();
        }

        void SetRaw(const std::string &logicalPath, std::string_view json)
        {
            Entry &entry = mEntries[logicalPath];
            entry.file = std::format("material-{:016x}.blob", ++mSerial);
            entry.typeTag = kRawAssetTypeTag;
            entry.contentVersion = kRawAssetContentVersion;
            REQUIRE(WriteBytes(Path() / entry.file,
                BuildAssetBlob(kRawAssetTypeTag, kRawAssetContentVersion,
                    std::as_bytes(std::span(json.data(), json.size())))));
            Publish();
        }

        void Publish()
        {
            std::vector<AssetManifestSourceEntry> entries;
            entries.reserve(mEntries.size());
            for (const auto &[logicalPath, entry] : mEntries)
                entries.push_back({ { MakeAssetId(logicalPath), entry.typeTag, entry.contentVersion },
                    entry.file, logicalPath, {} });
            REQUIRE(WriteBytes(Path() / kAssetManifestFileName, BuildAssetManifestBlob(entries)));
        }

        struct Entry
        {
            std::string file;
            uint32_t typeTag = kSurfaceMaterialAssetTypeTag;
            uint32_t contentVersion = kSurfaceMaterialAssetContentVersion;
        };

        std::map<std::string, Entry> mEntries;
        uint64_t mSerial = 0;
    };

    struct MultiFixture : MaterialCookedDir
    {
        explicit MultiFixture(bool reverseCatalogOrder = false)
            : mAssets(Path()), mJobs(0), mService(mAssets, mJobs, 1)
        {
            mHost.schemaLayout = TintLayout();
            const std::string first = "assets/one.material.json";
            const std::string second = "assets/two.material.json";
            const auto add = [&](const std::string &asset)
            {
                mHost.catalog.materials.push_back(MakeSurfaceRegistration("mesh", "shared_stem", asset));
            };
            if (reverseCatalogOrder)
            {
                add(second);
                add(first);
            }
            else
            {
                add(first);
                add(second);
            }
            mLibrary.Bind(&mService, mHost, mUploader);
        }

        void Cook(const std::string &logicalPath, const SurfaceMaterialAsset &material)
        {
            Set(logicalPath, material);
            mAssets.BeginReload();
            mAssets.CommitReload();
        }

        void Frame()
        {
            mService.DrainCompletions();
            mLibrary.Advance();
        }

        void Use(const std::string &logicalPath, std::string_view frontend = "mesh")
        {
            if (mOwner == 0)
                mOwner = mLibrary.AddMaterialUseOwner();
            const std::string key = logicalPath + "|" + std::string(frontend);
            mUses[key] = SurfaceMaterialUse{ logicalPath, MakeAssetId(logicalPath), std::string(frontend) };
            std::vector<SurfaceMaterialUse> uses;
            for (const auto &[stored, use] : mUses)
                uses.push_back(use);
            mLibrary.SetMaterialUses(mOwner, uses);
        }

        SurfaceMaterialResolution ResolveAsset(const std::string &logicalPath, std::string_view frontend = "mesh")
        {
            return mLibrary.Resolve(logicalPath, MakeAssetId(logicalPath), frontend);
        }

        SurfaceMaterialResolution Settle(const std::string &logicalPath, std::string_view frontend = "mesh")
        {
            SurfaceMaterialResolution resolution = ResolveAsset(logicalPath, frontend);
            for (int frame = 0; frame < 64 && resolution.status == SurfaceMaterialStatus::Pending; ++frame)
            {
                Frame();
                resolution = ResolveAsset(logicalPath, frontend);
            }
            return resolution;
        }

        AssetsModule mAssets;
        JobExecutor mJobs;
        AsyncAssetService mService;
        test::FakeTextureUploader mUploader;
        test::FakeSurfaceMaterialHost mHost;
        SurfaceMaterialLibrary mLibrary;
        uint64_t mOwner = 0;
        std::map<std::string, SurfaceMaterialUse> mUses;
    };
}

TEST_CASE("A surface material is pending until its async asset publishes", "[renderer][material][async]")
{
    Fixture f;
    f.Cook(TestMaterial());
    f.Use({ "mesh" });
    REQUIRE(f.Resolve().status == SurfaceMaterialStatus::Pending);
    CHECK(f.mHost.createdMaterials.empty());
    const SurfaceMaterialResolution ready = f.Settle();
    INFO(ready.error);
    REQUIRE(ready.status == SurfaceMaterialStatus::Ready);
    REQUIRE(ready.binding);
    CHECK(ready.binding->material == MaterialHandle{ 0, 1 });
    CHECK(f.Resolve().binding->material == ready.binding->material);
}

TEST_CASE("Loading-only updates wait for and publish a scene's surface material",
    "[renderer][material][async][scene-load]")
{
    Fixture f;
    f.Cook(TestMaterial());
    AssetResolver resolver(f.mAssets, "assets");
    test::FakeMeshResolveHost meshHost;
    World world;
    CommandBuffer commands;
    SystemContext context{ world, commands, 0.0f, 0.0, 0 };
    MeshResolveSystem system(meshHost, &f.mService, resolver, &f.mLibrary);
    SceneLoadGroup &group = world.EmplaceResource<SceneLoadGroup>();
    group.Begin();
    MeshRenderer mesh;
    std::snprintf(mesh.meshName, sizeof(mesh.meshName), "%s", "Cube");
    std::snprintf(mesh.materialShader, sizeof(mesh.materialShader), "%s", "test_mesh");
    const Entity entity = world.CreateEntity();
    world.AddComponent<MeshRenderer>(entity, mesh);

    system.OnLoadingUpdate(context);
    group.EndFrame();
    CHECK_FALSE(group.Ready());
    CHECK(world.GetComponent<MeshRenderer>(entity)->material == meshHost.ErrorMaterial());
    CHECK(f.mHost.createdMaterials.empty());

    f.mService.DrainCompletions();
    system.OnLoadingUpdate(context);
    group.EndFrame();
    const SurfaceMaterialResolution ready = f.Resolve();
    REQUIRE(ready.binding);
    CHECK(world.GetComponent<MeshRenderer>(entity)->material == ready.binding->material);
    CHECK(group.Ready());
    system.ReleaseResources();
}

TEST_CASE("A failed decode is remembered as a failure rather than a pending load", "[renderer][material][async]")
{
    Fixture f;
    f.CookBroken();
    f.Use({ "mesh" });
    REQUIRE(f.Resolve().status == SurfaceMaterialStatus::Pending);
    const SurfaceMaterialResolution failed = f.Settle();
    CHECK(failed.status == SurfaceMaterialStatus::Failed);
    CHECK(failed.error.find("cannot read") != std::string::npos);
    f.Frame();
    CHECK(f.Resolve().status == SurfaceMaterialStatus::Failed);
    CHECK(f.mHost.createdMaterials.empty());
}

TEST_CASE("A reload keeps the last good material until the replacement is ready and owners acknowledge it",
    "[renderer][material][async]")
{
    Fixture f;
    f.Cook(TestMaterial(R"({"tint":0.25})"));
    f.Use({ "mesh" });
    const SurfaceMaterialResolution initial = f.Settle();
    REQUIRE(initial.binding);
    const MaterialHandle first = initial.binding->material;

    f.Cook(TestMaterial(R"({"tint":0.75})"));
    f.mLibrary.CommitReload();
    const SurfaceMaterialResolution during = f.Resolve();
    REQUIRE(during.status == SurfaceMaterialStatus::Ready);
    CHECK(during.binding->material == first);

    f.Frame();
    const SurfaceMaterialResolution after = f.Resolve();
    REQUIRE(after.binding);
    CHECK(after.binding->material != first);
    CHECK(f.mHost.releasedMaterials.empty());

    f.mLibrary.AcknowledgeGeneration(f.mOwner, f.mLibrary.Generation());
    CHECK(f.mHost.releasedMaterials.size() == 1);
    CHECK(f.mHost.releasedMaterials[0] == first);
}

TEST_CASE("A superseded replacement cannot publish over a newer reload", "[renderer][material][async]")
{
    Fixture f;
    f.Cook(TestMaterial(R"({"tint":0.25})"));
    f.Use({ "mesh" });
    const SurfaceMaterialResolution initial = f.Settle();
    REQUIRE(initial.binding);
    const MaterialHandle first = initial.binding->material;

    f.Cook(TestMaterial(R"({"tint":0.50})"));
    f.mLibrary.CommitReload();
    f.Cook(TestMaterial(R"({"tint":0.75})"));
    f.mLibrary.CommitReload();

    f.Frame();
    const SurfaceMaterialResolution latest = f.Resolve();
    REQUIRE(latest.binding);
    CHECK(latest.binding->material != first);
    CHECK(f.mHost.createdMaterials.size() == 2);
    const auto record = f.RecordFor("test_mesh");
    REQUIRE(record);
    CHECK(TintOf(*record) == 0.75f);
}

TEST_CASE("A failed replacement keeps the last good handle and is not retried until a later reload",
    "[renderer][material][async]")
{
    Fixture f;
    f.Cook(TestMaterial(R"({"tint":0.25})"));
    f.Use({ "mesh" });
    const SurfaceMaterialResolution initial = f.Settle();
    REQUIRE(initial.binding);
    const MaterialHandle first = initial.binding->material;

    f.mHost.rejectMaterials = true;
    f.Cook(TestMaterial(R"({"tint":0.75})"));
    f.mLibrary.CommitReload();
    f.Frame();
    const SurfaceMaterialResolution failed = f.Resolve();
    REQUIRE(failed.status == SurfaceMaterialStatus::Ready);
    CHECK(failed.binding->material == first);
    const std::size_t attempts = f.mHost.createdMaterials.size();
    f.Frame();
    f.Frame();
    const SurfaceMaterialResolution repeated = f.Resolve();
    REQUIRE(repeated.binding);
    CHECK(repeated.binding->material == first);
    CHECK(f.mHost.createdMaterials.size() == attempts);

    f.mHost.rejectMaterials = false;
    f.mLibrary.CommitReload();
    f.Frame();
    const SurfaceMaterialResolution repaired = f.Resolve();
    REQUIRE(repaired.binding);
    CHECK(repaired.binding->material != first);
}

TEST_CASE("Both frontends of one asset publish in the same completion batch", "[renderer][material][async]")
{
    Fixture f;
    f.mHost.catalog.materials.push_back(MakeSurfaceRegistration("terrain", "test_terrain"));
    SurfaceMaterialAsset material = TestMaterial();
    material.frontends = { "mesh", "terrain" };
    f.Cook(material);
    f.Use({ "mesh", "terrain" });

    SurfaceMaterialResolution mesh, terrain;
    f.SettlePair("mesh", "terrain", mesh, terrain);
    REQUIRE(mesh.status == SurfaceMaterialStatus::Ready);
    REQUIRE(terrain.status == SurfaceMaterialStatus::Ready);
    CHECK(f.mHost.createdMaterials.size() == 2);
}

TEST_CASE("A failed frontend keeps the entire asset on its previous generation", "[renderer][material][async]")
{
    Fixture f;
    f.mHost.catalog.materials.push_back(MakeSurfaceRegistration("terrain", "test_terrain"));
    SurfaceMaterialAsset material = TestMaterial(R"({"tint":0.25})");
    material.frontends = { "mesh", "terrain" };
    f.Cook(material);
    f.Use({ "mesh", "terrain" });
    SurfaceMaterialResolution mesh, terrain;
    f.SettlePair("mesh", "terrain", mesh, terrain);
    REQUIRE(mesh.binding);
    REQUIRE(terrain.binding);
    const MaterialHandle oldMesh = mesh.binding->material;
    const MaterialHandle oldTerrain = terrain.binding->material;

    f.mHost.rejectStem = "test_terrain";
    SurfaceMaterialAsset replacement = TestMaterial(R"({"tint":0.75})");
    replacement.frontends = { "mesh", "terrain" };
    f.Cook(replacement);
    f.mLibrary.CommitReload();
    f.Frame();
    const SurfaceMaterialResolution meshAfter = f.Resolve("mesh");
    const SurfaceMaterialResolution terrainAfter = f.Resolve("terrain");
    REQUIRE(meshAfter.binding);
    REQUIRE(terrainAfter.binding);
    CHECK(meshAfter.binding->material == oldMesh);
    CHECK(terrainAfter.binding->material == oldTerrain);
    CHECK(std::find(f.mHost.releasedMaterials.begin(), f.mHost.releasedMaterials.end(), oldMesh) ==
        f.mHost.releasedMaterials.end());
    CHECK(std::find(f.mHost.releasedMaterials.begin(), f.mHost.releasedMaterials.end(), oldTerrain) ==
        f.mHost.releasedMaterials.end());

    f.mHost.rejectStem.clear();
    f.mLibrary.CommitReload();
    f.Frame();
    f.SettlePair("mesh", "terrain", mesh, terrain);
    REQUIRE(mesh.binding);
    REQUIRE(terrain.binding);
    CHECK(mesh.binding->material != oldMesh);
    CHECK(terrain.binding->material != oldTerrain);
    const auto record = f.RecordFor("test_mesh");
    REQUIRE(record);
    CHECK(TintOf(*record) == 0.75f);
}

TEST_CASE("A texture shared by both frontends is uploaded once and stays leased", "[renderer][material][async]")
{
    Fixture f;
    f.mHost.schemaLayout = TextureLayout();
    f.mHost.catalog.materials.clear();
    f.mHost.catalog.materials.push_back(MakeSurfaceRegistration("mesh", "test_mesh"));
    f.mHost.catalog.materials.push_back(MakeSurfaceRegistration("terrain", "test_terrain"));
    SurfaceMaterialAsset material = TestMaterial();
    material.frontends = { "mesh", "terrain" };
    material.albedoTexture = kTexture;
    material.normalTexture = kTexture;
    material.surfaceTexture = kTexture;
    f.Cook(material, std::span<const AssetId>(&kTexture, 1));
    f.Use({ "mesh", "terrain" });

    REQUIRE(f.Resolve("mesh").status == SurfaceMaterialStatus::Pending);
    REQUIRE(f.Resolve("terrain").status == SurfaceMaterialStatus::Pending);
    for (int frame = 0; frame < 8 && f.mUploader.mEntries.empty(); ++frame)
        f.Frame();
    REQUIRE(f.mUploader.mEntries.size() == 1);
    CHECK(f.mUploader.mQueued == 1);
    CHECK(f.mHost.createdMaterials.empty());
    REQUIRE(f.CompleteTexture());
    SurfaceMaterialResolution mesh, terrain;
    f.SettlePair("mesh", "terrain", mesh, terrain);
    REQUIRE(mesh.status == SurfaceMaterialStatus::Ready);
    REQUIRE(terrain.status == SurfaceMaterialStatus::Ready);
    CHECK(f.mHost.createdMaterials.size() == 2);
}

TEST_CASE("A failed texture keeps the whole first load failed", "[renderer][material][async]")
{
    Fixture f;
    f.mHost.schemaLayout = TextureLayout();
    SurfaceMaterialAsset material = TestMaterial();
    material.albedoTexture = kTexture;
    material.normalTexture = kTexture;
    material.surfaceTexture = kTexture;
    f.Cook(material, std::span<const AssetId>(&kTexture, 1));
    f.Use({ "mesh" });

    REQUIRE(f.Resolve().status == SurfaceMaterialStatus::Pending);
    for (int frame = 0; frame < 8 && f.mUploader.mEntries.empty(); ++frame)
        f.Frame();
    REQUIRE(f.mUploader.mEntries.size() == 1);
    const TextureHandle handle{ f.mUploader.mEntries.begin()->first, 1 };
    f.mUploader.Fail(handle);
    const SurfaceMaterialResolution failed = f.Settle();
    CHECK(failed.status == SurfaceMaterialStatus::Failed);
    CHECK(f.mHost.createdMaterials.empty());
    const SurfaceMaterialPublication publication = f.mLibrary.Publication(kLogicalPath);
    CHECK(publication.status == SurfaceMaterialPublicationStatus::Failed);
    CHECK_FALSE(publication.error.empty());
}

TEST_CASE("Removing the final material use releases its texture claim", "[renderer][material][async]")
{
    Fixture f;
    f.mHost.schemaLayout = TextureLayout();
    SurfaceMaterialAsset material = TestMaterial();
    material.albedoTexture = kTexture;
    material.normalTexture = kTexture;
    material.surfaceTexture = kTexture;
    f.Cook(material, std::span<const AssetId>(&kTexture, 1));
    f.Use({ "mesh" });
    REQUIRE(f.Resolve().status == SurfaceMaterialStatus::Pending);
    for (int frame = 0; frame < 8 && f.mUploader.mEntries.empty(); ++frame)
        f.Frame();
    REQUIRE(f.mUploader.mEntries.size() == 1);
    const TextureHandle texture{ f.mUploader.mEntries.begin()->first, 1 };
    REQUIRE(f.CompleteTexture());
    const SurfaceMaterialResolution ready = f.Settle();
    REQUIRE(ready.binding);
    const MaterialHandle accepted = ready.binding->material;
    f.mUploader.TextureLoads(&f.mService)->SetBudget(0);
    CHECK(f.mUploader.TextureState(texture) == TextureUploadState::Ready);

    f.Use({});
    f.mLibrary.AcknowledgeGeneration(f.mOwner, f.mLibrary.Generation());
    f.Frame();
    CHECK_FALSE(f.mLibrary.IsActiveAsset(kLogicalPath));
    CHECK(f.mUploader.TextureState(texture) == TextureUploadState::Stale);
    CHECK(std::find(f.mHost.releasedMaterials.begin(), f.mHost.releasedMaterials.end(), accepted) !=
        f.mHost.releasedMaterials.end());
}

TEST_CASE("Removing the final pending use abandons its decoded result", "[renderer][material][async]")
{
    Fixture f;
    f.Cook(TestMaterial());
    f.Use({ "mesh" });
    REQUIRE(f.Resolve().status == SurfaceMaterialStatus::Pending);
    f.Use({});
    f.Frame();
    f.mService.EvictUnused();
    CHECK(f.mService.DecodedBytes() == 0);
    CHECK(f.mHost.createdMaterials.empty());
    CHECK(f.mUploader.mQueued == 0);
}

TEST_CASE("A failed first frontend batch releases its unpublished material", "[renderer][material][async]")
{
    Fixture f;
    f.mHost.catalog.materials.push_back(MakeSurfaceRegistration("terrain", "test_terrain"));
    f.mHost.rejectStem = "test_terrain";
    SurfaceMaterialAsset material = TestMaterial();
    material.frontends = { "mesh", "terrain" };
    f.Cook(material);
    f.Use({ "mesh", "terrain" });
    SurfaceMaterialResolution mesh, terrain;
    f.SettlePair("mesh", "terrain", mesh, terrain);
    REQUIRE(mesh.status == SurfaceMaterialStatus::Failed);
    REQUIRE(terrain.status == SurfaceMaterialStatus::Failed);
    REQUIRE(f.mHost.createdMaterials.size() == 1);
    CHECK(f.mHost.releasedMaterials == std::vector<MaterialHandle>{ MaterialHandle{ 0, 1 } });
}

TEST_CASE("Existing owners still guard replaced resources after a library reset", "[renderer][material][async]")
{
    Fixture f;
    f.Cook(TestMaterial());
    f.Use({});
    const uint64_t secondOwner = f.mLibrary.AddMaterialUseOwner();
    f.mLibrary.Reset();
    f.Use({ "mesh" });
    const SurfaceMaterialUse use{ kLogicalPath, kMaterial, "mesh" };
    f.mLibrary.SetMaterialUses(secondOwner, std::span(&use, 1));
    const SurfaceMaterialResolution first = f.Settle();
    REQUIRE(first.binding);
    const MaterialHandle previous = first.binding->material;

    f.Cook(TestMaterial(R"({"tint":0.75})"));
    f.mLibrary.CommitReload();
    SurfaceMaterialResolution next;
    for (int frame = 0; frame < 8; ++frame)
    {
        f.Frame();
        next = f.Resolve();
        if (next.binding && next.binding->material != previous)
            break;
    }
    REQUIRE(next.binding);
    REQUIRE(next.binding->material != previous);
    CHECK(f.mHost.releasedMaterials.empty());
    f.mLibrary.AcknowledgeGeneration(f.mOwner, f.mLibrary.Generation());
    CHECK(f.mHost.releasedMaterials.empty());
    f.mLibrary.AcknowledgeGeneration(secondOwner, f.mLibrary.Generation());
    CHECK(f.mHost.releasedMaterials == std::vector<MaterialHandle>{ previous });
}

TEST_CASE("A material publication is per asset and unchanged content confirms reuse",
    "[renderer][material][async][publication]")
{
    MultiFixture f;
    const std::string one = "assets/one.material.json";
    const std::string two = "assets/two.material.json";
    f.Cook(one, TestMaterial(R"({"tint":0.25})"));
    f.Cook(two, TestMaterial(R"({"tint":0.75})"));
    f.Use(one);
    f.Use(two);
    REQUIRE(f.Settle(one).status == SurfaceMaterialStatus::Ready);
    REQUIRE(f.Settle(two).status == SurfaceMaterialStatus::Ready);
    CHECK(f.mLibrary.Publication(one).status == SurfaceMaterialPublicationStatus::Published);
    CHECK(f.mLibrary.Publication(two).status == SurfaceMaterialPublicationStatus::Published);
    CHECK(f.mLibrary.Publication("assets/unused.material.json").status ==
        SurfaceMaterialPublicationStatus::Inactive);

    const std::size_t created = f.mHost.createdMaterials.size();
    f.Cook(one, TestMaterial(R"({"tint":0.5})"));
    f.mLibrary.CommitReload();
    CHECK(f.mLibrary.Publication(one).status == SurfaceMaterialPublicationStatus::Pending);
    CHECK(f.mLibrary.Publication(two).status == SurfaceMaterialPublicationStatus::Published);

    f.mLibrary.NotifyShaderCommit();
    CHECK(f.mLibrary.Publication(one).status == SurfaceMaterialPublicationStatus::Pending);
    CHECK(f.mLibrary.Publication(two).status == SurfaceMaterialPublicationStatus::Published);

    f.Frame();
    CHECK(f.mLibrary.Publication(one).status == SurfaceMaterialPublicationStatus::Published);
    REQUIRE(f.mHost.createdMaterials.size() == created + 1);

    f.mAssets.BeginReload();
    f.mAssets.CommitReload();
    f.mLibrary.CommitReload();
    CHECK(f.mLibrary.Publication(one).status == SurfaceMaterialPublicationStatus::Published);
    CHECK(f.mLibrary.Publication(two).status == SurfaceMaterialPublicationStatus::Published);
    CHECK(f.mHost.createdMaterials.size() == created + 1);
}

TEST_CASE("A failed replacement reports its publication while the last good binding stays ready",
    "[renderer][material][async][publication]")
{
    Fixture f;
    f.Cook(TestMaterial(R"({"tint":0.25})"));
    f.Use({ "mesh" });
    const SurfaceMaterialResolution initial = f.Settle();
    REQUIRE(initial.binding);
    const MaterialHandle first = initial.binding->material;
    CHECK(f.mLibrary.Publication(kLogicalPath).status == SurfaceMaterialPublicationStatus::Published);

    f.mHost.rejectMaterials = true;
    f.Cook(TestMaterial(R"({"tint":0.75})"));
    f.mLibrary.CommitReload();
    CHECK(f.mLibrary.Publication(kLogicalPath).status == SurfaceMaterialPublicationStatus::Pending);
    f.Frame();
    const SurfaceMaterialPublication failed = f.mLibrary.Publication(kLogicalPath);
    CHECK(failed.status == SurfaceMaterialPublicationStatus::Failed);
    CHECK_FALSE(failed.error.empty());
    const SurfaceMaterialResolution during = f.Resolve();
    REQUIRE(during.binding);
    CHECK(during.binding->material == first);

    f.mHost.rejectMaterials = false;
    f.mLibrary.CommitReload();
    f.Frame();
    CHECK(f.mLibrary.Publication(kLogicalPath).status == SurfaceMaterialPublicationStatus::Published);
    const SurfaceMaterialResolution repaired = f.Resolve();
    REQUIRE(repaired.binding);
    CHECK(repaired.binding->material != first);
}

TEST_CASE("A late schema failure keeps the last good binding and reports its publication",
    "[renderer][material][async][publication]")
{
    Fixture f;
    f.Cook(TestMaterial(R"({"tint":0.25})"));
    f.Use({ "mesh" });
    const SurfaceMaterialResolution initial = f.Settle();
    REQUIRE(initial.binding);
    const MaterialHandle first = initial.binding->material;

    f.mHost.schemaLayout = std::nullopt;
    f.Cook(TestMaterial(R"({"tint":0.75})"));
    f.mLibrary.CommitReload();
    f.Frame();
    const SurfaceMaterialPublication publication = f.mLibrary.Publication(kLogicalPath);
    CHECK(publication.status == SurfaceMaterialPublicationStatus::Failed);
    CHECK(publication.error.find("schema") != std::string::npos);
    const SurfaceMaterialResolution after = f.Resolve();
    REQUIRE(after.binding);
    CHECK(after.binding->material == first);
}

TEST_CASE("A failed first load publishes a failure instead of staying pending",
    "[renderer][material][async][publication]")
{
    Fixture f;
    f.CookBroken();
    f.Use({ "mesh" });
    REQUIRE(f.Resolve().status == SurfaceMaterialStatus::Pending);
    const SurfaceMaterialResolution failed = f.Settle();
    REQUIRE(failed.status == SurfaceMaterialStatus::Failed);
    const SurfaceMaterialPublication publication = f.mLibrary.Publication(kLogicalPath);
    CHECK(publication.status == SurfaceMaterialPublicationStatus::Failed);
    CHECK(publication.error == failed.error);
}

TEST_CASE("A material name cleared while the library republishes clears the draw binding and use",
    "[renderer][material][async][rebind]")
{
    Fixture f;
    f.Cook(TestMaterial(R"({"tint":0.25})"));
    AssetResolver resolver(f.mAssets, "assets");
    test::FakeMeshResolveHost meshHost;
    World world;
    CommandBuffer commands;
    SystemContext context{ world, commands, 0.0f, 0.0, 0 };
    MeshResolveSystem system(meshHost, &f.mService, resolver, &f.mLibrary);
    MeshRenderer mesh;
    std::snprintf(mesh.meshName, sizeof(mesh.meshName), "%s", "Cube");
    std::snprintf(mesh.materialShader, sizeof(mesh.materialShader), "%s", "test_mesh");
    const Entity entity = world.CreateEntity();
    world.AddComponent<MeshRenderer>(entity, mesh);

    MaterialHandle accepted;
    for (int frame = 0; frame < 16; ++frame)
    {
        f.mService.DrainCompletions();
        system.OnUpdate(context);
        const MaterialHandle current = world.GetComponent<MeshRenderer>(entity)->material;
        if (!current.IsNull() && current != meshHost.ErrorMaterial())
        {
            accepted = current;
            break;
        }
    }
    REQUIRE_FALSE(accepted.IsNull());
    REQUIRE(f.mLibrary.IsActiveAsset(kLogicalPath));

    f.Cook(TestMaterial(R"({"tint":0.75})"));
    f.mLibrary.CommitReload();
    std::snprintf(world.GetComponent<MeshRenderer>(entity)->materialShader,
        sizeof(MeshRenderer::materialShader), "%s", "");
    f.mService.DrainCompletions();
    system.OnUpdate(context);

    CHECK(world.GetComponent<MeshRenderer>(entity)->material.IsNull());
    CHECK(std::none_of(f.mLibrary.ActiveMaterials().begin(), f.mLibrary.ActiveMaterials().end(),
        [](const SurfaceMaterialUse &use) { return use.logicalPath == kLogicalPath; }));

    for (int frame = 0; frame < 4; ++frame)
    {
        f.mService.DrainCompletions();
        system.OnUpdate(context);
    }
    CHECK_FALSE(f.mLibrary.IsActiveAsset(kLogicalPath));
    CHECK(std::find(f.mHost.releasedMaterials.begin(), f.mHost.releasedMaterials.end(), accepted) !=
        f.mHost.releasedMaterials.end());
}

TEST_CASE("A system reload with an empty material name clears the stale draw handle",
    "[renderer][material][async][rebind]")
{
    Fixture f;
    f.Cook(TestMaterial(R"({"tint":0.25})"));
    AssetResolver resolver(f.mAssets, "assets");
    test::FakeMeshResolveHost meshHost;
    World world;
    CommandBuffer commands;
    SystemContext context{ world, commands, 0.0f, 0.0, 0 };
    MeshResolveSystem system(meshHost, &f.mService, resolver, &f.mLibrary);
    MeshRenderer mesh;
    std::snprintf(mesh.meshName, sizeof(mesh.meshName), "%s", "Cube");
    std::snprintf(mesh.materialShader, sizeof(mesh.materialShader), "%s", "test_mesh");
    const Entity entity = world.CreateEntity();
    world.AddComponent<MeshRenderer>(entity, mesh);

    MaterialHandle accepted;
    for (int frame = 0; frame < 16; ++frame)
    {
        f.mService.DrainCompletions();
        system.OnUpdate(context);
        const MaterialHandle current = world.GetComponent<MeshRenderer>(entity)->material;
        if (!current.IsNull() && current != meshHost.ErrorMaterial())
        {
            accepted = current;
            break;
        }
    }
    REQUIRE_FALSE(accepted.IsNull());

    f.Cook(TestMaterial(R"({"tint":0.75})"));
    f.mLibrary.CommitReload();
    system.CommitReload();
    std::snprintf(world.GetComponent<MeshRenderer>(entity)->materialShader,
        sizeof(MeshRenderer::materialShader), "%s", "");
    f.mService.DrainCompletions();
    system.OnUpdate(context);

    CHECK(world.GetComponent<MeshRenderer>(entity)->material.IsNull());
    CHECK(std::none_of(f.mLibrary.ActiveMaterials().begin(), f.mLibrary.ActiveMaterials().end(),
        [](const SurfaceMaterialUse &use) { return use.logicalPath == kLogicalPath; }));

    for (int frame = 0; frame < 4; ++frame)
    {
        f.mService.DrainCompletions();
        system.OnUpdate(context);
    }
    CHECK(std::find(f.mHost.releasedMaterials.begin(), f.mHost.releasedMaterials.end(), accepted) !=
        f.mHost.releasedMaterials.end());
}

TEST_CASE("A field-change reset with an empty material name clears the stale draw handle",
    "[renderer][material][async][rebind]")
{
    Fixture f;
    f.Cook(TestMaterial(R"({"tint":0.25})"));
    AssetResolver resolver(f.mAssets, "assets");
    test::FakeMeshResolveHost meshHost;
    World world;
    ComponentFieldListeners &listeners = world.EmplaceResource<ComponentFieldListeners>();
    CommandBuffer commands;
    SystemContext context{ world, commands, 0.0f, 0.0, 0 };
    MeshResolveSystem system(meshHost, &f.mService, resolver, &f.mLibrary);
    system.OnStart(context);
    MeshRenderer mesh;
    std::snprintf(mesh.meshName, sizeof(mesh.meshName), "%s", "Cube");
    std::snprintf(mesh.materialShader, sizeof(mesh.materialShader), "%s", "test_mesh");
    const Entity entity = world.CreateEntity();
    world.AddComponent<MeshRenderer>(entity, mesh);

    MaterialHandle accepted;
    for (int frame = 0; frame < 16; ++frame)
    {
        f.mService.DrainCompletions();
        system.OnUpdate(context);
        const MaterialHandle current = world.GetComponent<MeshRenderer>(entity)->material;
        if (!current.IsNull() && current != meshHost.ErrorMaterial())
        {
            accepted = current;
            break;
        }
    }
    REQUIRE_FALSE(accepted.IsNull());

    const ComponentFieldChange change{ entity, TypeIdOf<MeshRenderer>(), { "materialShader" } };
    listeners.NotifyBeforeChange(world, std::span(&change, 1));
    std::snprintf(world.GetComponent<MeshRenderer>(entity)->materialShader,
        sizeof(MeshRenderer::materialShader), "%s", "");
    f.mService.DrainCompletions();
    system.OnUpdate(context);

    CHECK(world.GetComponent<MeshRenderer>(entity)->material.IsNull());
    CHECK(std::none_of(f.mLibrary.ActiveMaterials().begin(), f.mLibrary.ActiveMaterials().end(),
        [](const SurfaceMaterialUse &use) { return use.logicalPath == kLogicalPath; }));
    system.OnStop(context);
}

TEST_CASE("Two recipes sharing one shader stem bind through their explicit material assets",
    "[renderer][material][async][identity]")
{
    MultiFixture f;
    const std::string one = "assets/one.material.json";
    const std::string two = "assets/two.material.json";
    f.Cook(one, TestMaterial(R"({"tint":0.25})"));
    f.Cook(two, TestMaterial(R"({"tint":0.75})"));
    AssetResolver resolver(f.mAssets, "assets");
    test::FakeMeshResolveHost meshHost;
    World world;
    CommandBuffer commands;
    SystemContext context{ world, commands, 0.0f, 0.0, 0 };
    MeshResolveSystem system(meshHost, &f.mService, resolver, &f.mLibrary);
    MeshRenderer firstMesh;
    std::snprintf(firstMesh.meshName, sizeof(firstMesh.meshName), "%s", "Cube");
    std::snprintf(firstMesh.materialShader, sizeof(firstMesh.materialShader), "%s", "one.material.json");
    MeshRenderer secondMesh = firstMesh;
    std::snprintf(secondMesh.materialShader, sizeof(secondMesh.materialShader), "%s", two.c_str());
    const Entity first = world.CreateEntity();
    const Entity second = world.CreateEntity();
    world.AddComponent<MeshRenderer>(first, firstMesh);
    world.AddComponent<MeshRenderer>(second, secondMesh);

    for (int frame = 0; frame < 16; ++frame)
    {
        f.mService.DrainCompletions();
        system.OnUpdate(context);
    }
    const MaterialHandle firstHandle = world.GetComponent<MeshRenderer>(first)->material;
    const MaterialHandle secondHandle = world.GetComponent<MeshRenderer>(second)->material;
    REQUIRE_FALSE(firstHandle.IsNull());
    REQUIRE_FALSE(secondHandle.IsNull());
    CHECK(firstHandle != meshHost.ErrorMaterial());
    CHECK(secondHandle != meshHost.ErrorMaterial());
    CHECK(firstHandle != secondHandle);

    const SurfaceMaterialResolution firstBinding = f.ResolveAsset(one);
    const SurfaceMaterialResolution secondBinding = f.ResolveAsset(two);
    REQUIRE(firstBinding.binding);
    REQUIRE(secondBinding.binding);
    CHECK(firstBinding.binding->material == firstHandle);
    CHECK(secondBinding.binding->material == secondHandle);
    CHECK(f.mLibrary.Publication(one).status == SurfaceMaterialPublicationStatus::Published);
    CHECK(f.mLibrary.Publication(two).status == SurfaceMaterialPublicationStatus::Published);
    system.ReleaseResources();
}

TEST_CASE("An ambiguous shared shader stem fails instead of taking the first recipe",
    "[renderer][material][async][identity]")
{
    const bool reverse = GENERATE(false, true);
    CAPTURE(reverse);
    MultiFixture f(reverse);
    f.Cook("assets/one.material.json", TestMaterial(R"({"tint":0.25})"));
    f.Cook("assets/two.material.json", TestMaterial(R"({"tint":0.75})"));
    AssetResolver resolver(f.mAssets, "assets");
    test::FakeMeshResolveHost meshHost;
    World world;
    CommandBuffer commands;
    SystemContext context{ world, commands, 0.0f, 0.0, 0 };
    MeshResolveSystem system(meshHost, &f.mService, resolver, &f.mLibrary);
    MeshRenderer ambiguous;
    std::snprintf(ambiguous.meshName, sizeof(ambiguous.meshName), "%s", "Cube");
    std::snprintf(ambiguous.materialShader, sizeof(ambiguous.materialShader), "%s", "shared_stem");
    MeshRenderer raw = ambiguous;
    std::snprintf(raw.materialShader, sizeof(raw.materialShader), "%s", "legacy_raw");
    MeshRenderer missing = ambiguous;
    std::snprintf(missing.materialShader, sizeof(missing.materialShader), "%s", "missing.material.json");
    const Entity ambiguousEntity = world.CreateEntity();
    const Entity rawEntity = world.CreateEntity();
    const Entity missingEntity = world.CreateEntity();
    world.AddComponent<MeshRenderer>(ambiguousEntity, ambiguous);
    world.AddComponent<MeshRenderer>(rawEntity, raw);
    world.AddComponent<MeshRenderer>(missingEntity, missing);

    for (int frame = 0; frame < 8; ++frame)
    {
        f.mService.DrainCompletions();
        system.OnUpdate(context);
    }

    CHECK(world.GetComponent<MeshRenderer>(ambiguousEntity)->material == meshHost.ErrorMaterial());
    CHECK(world.GetComponent<MeshRenderer>(missingEntity)->material == meshHost.ErrorMaterial());
    CHECK_FALSE(world.GetComponent<MeshRenderer>(rawEntity)->material.IsNull());
    CHECK(world.GetComponent<MeshRenderer>(rawEntity)->material != meshHost.ErrorMaterial());
    CHECK(std::none_of(meshHost.createdMaterials.begin(), meshHost.createdMaterials.end(),
        [](const MaterialDesc &desc) { return desc.shaderName == "shared_stem"; }));
    CHECK(std::any_of(meshHost.createdMaterials.begin(), meshHost.createdMaterials.end(),
        [](const MaterialDesc &desc) { return desc.shaderName == "legacy_raw"; }));
    system.ReleaseResources();
}

TEST_CASE("Numeric preview publishes both frontends and a rejected overlay keeps their bytes",
    "[renderer][material][async]")
{
    Fixture f;
    f.mHost.catalog.materials.push_back(MakeSurfaceRegistration("terrain", "test_terrain"));
    SurfaceMaterialAsset material = TestMaterial();
    material.frontends = { "mesh", "terrain" };
    f.Cook(material);
    f.Use({ "mesh", "terrain" });
    SurfaceMaterialResolution mesh, terrain;
    f.SettlePair("mesh", "terrain", mesh, terrain);
    REQUIRE(mesh.status == SurfaceMaterialStatus::Ready);
    REQUIRE(terrain.status == SurfaceMaterialStatus::Ready);

    std::string error;
    REQUIRE(f.mLibrary.PreviewParameters(kLogicalPath, "mesh", R"({"tint":0.9})", &error));
    const uint64_t generation = f.mLibrary.Generation();
    const auto meshRecord = f.RecordFor("test_mesh");
    const auto terrainRecord = f.RecordFor("test_terrain");
    REQUIRE(meshRecord);
    REQUIRE(terrainRecord);
    CHECK(TintOf(*meshRecord) == 0.9f);
    CHECK(TintOf(*terrainRecord) == 0.9f);

    CHECK_FALSE(f.mLibrary.PreviewParameters(kLogicalPath, "mesh", R"({"unknown":1})", &error));
    CHECK(TintOf(*f.RecordFor("test_mesh")) == 0.9f);
    CHECK(TintOf(*f.RecordFor("test_terrain")) == 0.9f);
    CHECK(f.mLibrary.Generation() == generation);
}

namespace
{
    const std::string kLayered = "assets/materials/layers.terrainmaterial.json";
    const std::string kSand = "assets/materials/sand.material.json";
    const std::string kDirt = "assets/materials/dirt.material.json";
    const std::string kLayeredJson =
        R"({"version":1,"blend":"height","heightContrast":0.1,"layers":[)"
        R"({"name":"sand","material":"materials/sand.material.json"},)"
        R"({"name":"dirt","material":"materials/dirt.material.json"}]})";

    MaterialLayout LayeredHeaderLayout()
    {
        MaterialLayout layout;
        layout.fields = {
            { .name = "blendMode", .kind = MaterialFieldKind::Uint, .offset = 0, .size = 4 },
            { .name = "heightContrast", .kind = MaterialFieldKind::Float, .offset = 4, .size = 4 },
            { .name = "layerOffset0", .kind = MaterialFieldKind::Uint, .offset = 8, .size = 4 },
            { .name = "layerOffset1", .kind = MaterialFieldKind::Uint, .offset = 12, .size = 4 },
        };
        layout.size = 16;
        return layout;
    }

    uint32_t UintAt(const std::vector<std::byte> &bytes, std::size_t offset)
    {
        uint32_t value = 0;
        if (bytes.size() >= offset + sizeof(value))
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    float FloatAt(const std::vector<std::byte> &bytes, std::size_t offset)
    {
        float value = 0.0f;
        if (bytes.size() >= offset + sizeof(value))
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    SurfaceMaterialAsset TerrainMaterial(std::string parameters)
    {
        SurfaceMaterialAsset material = TestMaterial(std::move(parameters));
        material.frontends = { "terrain" };
        return material;
    }

    struct LayeredFixture : MaterialCookedDir
    {
        LayeredFixture()
            : mAssets(Path()), mJobs(0), mService(mAssets, mJobs, 1)
        {
            mHost.schemaLayout = TintLayout();
            mHost.stemLayouts["test_layered"] = LayeredHeaderLayout();
            mHost.catalog.materials.push_back(MakeSurfaceRegistration("terrain", "test_sand", kSand));
            mHost.catalog.materials.push_back(MakeSurfaceRegistration("terrain", "test_dirt", kDirt));
            mHost.catalog.materials.push_back(MakeSurfaceRegistration("terrain", "test_layered", kLayered));
            mLibrary.Bind(&mService, mHost, mUploader);
        }

        void Cook(float sandTint = 0.25f, float dirtTint = 0.75f)
        {
            Set(kSand, TerrainMaterial(std::format(R"({{"tint":{}}})", sandTint)));
            Set(kDirt, TerrainMaterial(std::format(R"({{"tint":{}}})", dirtTint)));
            SetRaw(kLayered, kLayeredJson);
            mAssets.BeginReload();
            mAssets.CommitReload();
        }

        void CookSand(float sandTint)
        {
            Set(kSand, TerrainMaterial(std::format(R"({{"tint":{}}})", sandTint)));
            mAssets.BeginReload();
            mAssets.CommitReload();
        }

        void CookSandTilePeriod(float period)
        {
            Set(kSand, TerrainMaterial(std::format(R"({{"tint":0.25,"tileSizeMeters":{}}})", period)));
            Set(kDirt, TerrainMaterial(R"({"tint":0.75})"));
            SetRaw(kLayered, kLayeredJson);
            mAssets.BeginReload();
            mAssets.CommitReload();
        }

        void Frame()
        {
            mService.DrainCompletions();
            mLibrary.Advance();
        }

        void Use()
        {
            if (mOwner == 0)
                mOwner = mLibrary.AddMaterialUseOwner();
            mLibrary.SetMaterialUses(mOwner, std::vector<SurfaceMaterialUse>{
                { kLayered, MakeAssetId(kLayered), "terrain" } });
        }

        SurfaceMaterialResolution Resolve()
        {
            return mLibrary.Resolve(kLayered, MakeAssetId(kLayered), "terrain");
        }

        SurfaceMaterialResolution Settle(int frames = 64)
        {
            SurfaceMaterialResolution resolution = Resolve();
            for (int frame = 0; frame < frames && resolution.status == SurfaceMaterialStatus::Pending; ++frame)
            {
                Frame();
                resolution = Resolve();
            }
            return resolution;
        }

        std::optional<MaterialRecordSnapshot> RecordFor(std::string_view stem) const
        {
            for (std::size_t i = mHost.createdMaterials.size(); i-- > 0;)
                if (mHost.createdMaterials[i].shaderName == stem)
                    return mHost.records.at(static_cast<uint32_t>(i));
            return std::nullopt;
        }

        AssetsModule mAssets;
        JobExecutor mJobs;
        AsyncAssetService mService;
        test::FakeTextureUploader mUploader;
        test::FakeSurfaceMaterialHost mHost;
        SurfaceMaterialLibrary mLibrary;
        uint64_t mOwner = 0;
    };
}

TEST_CASE("A layered material publishes one composite record over its child records",
    "[renderer][material][async][layered]")
{
    LayeredFixture f;
    f.Cook();
    f.Use();
    const SurfaceMaterialResolution resolution = f.Settle();
    INFO(resolution.error);
    REQUIRE(resolution.status == SurfaceMaterialStatus::Ready);
    REQUIRE(resolution.binding);
    CHECK_FALSE(resolution.binding->material.IsNull());
    CHECK(resolution.binding->layers == std::vector<std::string>{ "sand", "dirt" });

    const auto sand = f.RecordFor("test_sand");
    const auto dirt = f.RecordFor("test_dirt");
    const auto layered = f.RecordFor("test_layered");
    REQUIRE(sand);
    REQUIRE(dirt);
    REQUIRE(layered);
    CHECK(layered->mBytes.size() == 48);
    CHECK(UintAt(layered->mBytes, 0) == 1);
    CHECK(FloatAt(layered->mBytes, 4) == Catch::Approx(0.1f));
    const uint32_t sandOffset = UintAt(layered->mBytes, 8);
    const uint32_t dirtOffset = UintAt(layered->mBytes, 12);
    CHECK(sandOffset == 16);
    CHECK(dirtOffset == 32);
    CHECK(std::equal(sand->mBytes.begin(), sand->mBytes.end(), layered->mBytes.begin() + sandOffset));
    CHECK(std::equal(dirt->mBytes.begin(), dirt->mBytes.end(), layered->mBytes.begin() + dirtOffset));
}

TEST_CASE("A failed layered child fails the whole layered material and names the layer",
    "[renderer][material][async][layered]")
{
    LayeredFixture f;
    f.mHost.rejectStem = "test_dirt";
    f.Cook();
    f.Use();
    const SurfaceMaterialResolution resolution = f.Settle();
    CHECK(resolution.status == SurfaceMaterialStatus::Failed);
    CHECK(resolution.error.find("dirt") != std::string::npos);
    CHECK(f.mHost.createdMaterials.size() == 1);
    CHECK(f.mHost.releasedMaterials == std::vector<MaterialHandle>{ MaterialHandle{ 0, 1 } });
}

TEST_CASE("A layer failing the terrain parameter rules fails the layered material and names the layer",
    "[renderer][material][async][layered]")
{
    LayeredFixture f;
    MaterialLayout sandLayout = TintLayout();
    sandLayout.fields.push_back(
        { .name = "tileSizeMeters", .kind = MaterialFieldKind::Float, .offset = 4, .size = 4 });
    f.mHost.stemLayouts["test_sand"] = sandLayout;
    f.mLibrary.SetMaterialValidator([](const SurfaceMaterialBinding &binding, std::string &error)
    {
        if (binding.tilePeriodMeters)
        {
            error = "tileSizeMeters is not a supported terrain period";
            return false;
        }
        return true;
    });
    f.CookSandTilePeriod(3.0f);
    f.Use();
    const SurfaceMaterialResolution resolution = f.Settle();
    CHECK(resolution.status == SurfaceMaterialStatus::Failed);
    CHECK(resolution.error.find("sand") != std::string::npos);
    CHECK_FALSE(resolution.binding.has_value());
    CHECK(f.mHost.createdMaterials.empty());
}

TEST_CASE("Republishing a layered child rebuilds the layered record", "[renderer][material][async][layered]")
{
    LayeredFixture f;
    f.Cook();
    f.Use();
    const SurfaceMaterialResolution initial = f.Settle();
    REQUIRE(initial.binding);
    const MaterialHandle first = initial.binding->material;

    f.CookSand(0.9f);
    f.mLibrary.CommitReload();
    SurfaceMaterialResolution next;
    for (int frame = 0; frame < 32; ++frame)
    {
        f.Frame();
        next = f.Resolve();
        if (next.binding && next.binding->material != first)
            break;
    }
    REQUIRE(next.binding);
    CHECK(next.binding->material != first);

    const auto sand = f.RecordFor("test_sand");
    const auto layered = f.RecordFor("test_layered");
    REQUIRE(sand);
    REQUIRE(layered);
    CHECK(FloatAt(sand->mBytes, 0) == Catch::Approx(0.9f));
    const uint32_t sandOffset = UintAt(layered->mBytes, 8);
    CHECK(FloatAt(layered->mBytes, sandOffset) == Catch::Approx(0.9f));
}

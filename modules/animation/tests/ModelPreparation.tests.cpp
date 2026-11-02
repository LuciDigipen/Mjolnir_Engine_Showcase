/**
 * @file ModelPreparation.tests.cpp
 * @author sumin.park
 * @brief Background model preparation, texture settling, error appearance and release tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <animation/ModelLibrary.h>
#include <animation/ModelPreparation.h>

#include <assets/AssetsModule.h>
#include <assets/ModelAsset.h>
#include <core/fs/FileIO.h>
#include <jobs/JobExecutor.h>

#include "support/CookedMeshDir.h"
#include "support/FakeMeshResolveHost.h"
#include "support/FakeTextureUploader.h"

#include <catch2/catch_test_macros.hpp>

#include <format>

using namespace mir;

namespace
{
    const AssetId kModel = MakeAssetId("model");
    const AssetId kMesh = MakeAssetId("mesh");
    const AssetId kMaterial = MakeAssetId("material");
    const AssetId kTexture = MakeAssetId("texture");
    const TextureHandle kError{ 777, 1 };

    struct CookedModel : test::CookedDir
    {
        explicit CookedModel(bool withTexture = true) { Write(withTexture); }

        // Blob names carry a content hash like a cook's, a mesh variant changes the mesh blob alone.
        void Write(bool withTexture = true, uint32_t meshVariant = 0)
        {
            ModelAsset model;
            model.nodes.resize(1);
            model.nodes[0].key = "root";
            model.nodes[0].mesh = kMesh;
            model.scenes = { { "scene", { 0 } } };
            MeshPrimitive primitive;
            primitive.vertices.resize(3);
            primitive.vertices[0].position.x = static_cast<float>(meshVariant);
            primitive.indices = { 0, 1, 2 };
            primitive.material = kMaterial;
            primitive.baseColorTexture = kTexture;
            MeshAsset mesh;
            mesh.primitives.push_back(primitive);
            MaterialAsset material;
            material.baseColor = glm::vec4(0.25f, 0.5f, 0.75f, 1);
            material.baseColorTexture = kTexture;
            TextureAsset texture;
            texture.width = texture.height = 4;
            texture.rgba.assign(64, 80);

            const std::string modelFile = "model-0000000000000011.blob",
                meshFile = std::format("mesh-{:016x}.blob", 0x12 + meshVariant),
                materialFile = "material-0000000000000013.blob", textureFile = "texture-0000000000000014.blob";
            const std::vector<std::pair<std::string, std::vector<std::byte>>> blobs{
                { modelFile, BuildTypedAsset(model) }, { meshFile, BuildTypedAsset(mesh) },
                { materialFile, BuildTypedAsset(material) }, { textureFile, BuildTypedAsset(texture) } };
            for (const auto &[file, bytes] : blobs)
                REQUIRE(WriteBytes(Path() / file, bytes));
            const AssetId modelEdges[]{ kMesh }, meshEdges[]{ kMaterial }, materialEdges[]{ kTexture };
            std::vector<AssetManifestSourceEntry> entries{
                { { kModel, kModelAssetTypeTag, kModelAssetContentVersion }, modelFile, "model", modelEdges },
                { { kMesh, kMeshAssetTypeTag, kMeshAssetContentVersion }, meshFile, "mesh", meshEdges },
                { { kMaterial, kMaterialAssetTypeTag, kMaterialAssetContentVersion }, materialFile, "material",
                  materialEdges },
                { { kTexture, kTextureAssetTypeTag, kTextureAssetContentVersion }, textureFile, "texture", {} } };
            // a texture the catalog does not list, its material still names it
            if (!withTexture)
            {
                entries.pop_back();
                entries.back().dependencies = {};
            }
            REQUIRE(WriteBytes(Path() / kAssetManifestFileName, BuildAssetManifestBlob(entries)));
        }
    };

    struct Fixture : CookedModel
    {
        explicit Fixture(bool withTexture = true)
            : CookedModel(withTexture), mAssets(Path()), mJobs(0), mService(mAssets, mJobs, 1),
              mPreparation(mHost, mTextures, kError, &mService, kModel, "model")
        {}

        ModelPreparation::State Frame()
        {
            mService.DrainCompletions();
            return mPreparation.Advance();
        }

        // Frames until the texture upload is queued
        void QueueTexture()
        {
            for (int frame = 0; frame < 8 && mTextures.mEntries.empty(); ++frame)
                Frame();
            REQUIRE(mTextures.mEntries.size() == 1);
        }

        AssetsModule mAssets;
        JobExecutor mJobs;
        AsyncAssetService mService;
        test::FakeMeshResolveHost mHost;
        test::FakeTextureUploader mTextures;
        ModelPreparation mPreparation;
    };
}

TEST_CASE("A model publishes after its texture and meshes are ready and uses both", "[animation][model-preparation]")
{
    Fixture f;
    f.QueueTexture();
    const TextureHandle texture{ 0, 1 };
    CHECK(f.Frame() == ModelPreparation::State::Textures);
    CHECK(f.mHost.createdMaterials.empty());
    f.mTextures.Begin(texture);
    f.mTextures.CopyAll(texture);
    f.mTextures.Complete(texture);
    // the materials exist and the mesh upload is queued, the model waits for it
    REQUIRE(f.Frame() == ModelPreparation::State::Meshes);
    REQUIRE(f.mHost.createdMaterials.size() == 2);
    CHECK(f.mHost.createdMaterials[0].baseColorTexture == texture);
    CHECK(f.mHost.createdMaterials[0].frontFace != f.mHost.createdMaterials[1].frontFace);
    REQUIRE(f.mHost.uploader.queued == 1);
    CHECK(f.mHost.syncMeshes == 0);
    CHECK(f.mPreparation.Take() == nullptr);
    const MeshHandle mesh{ 0, 1 };
    f.mHost.uploader.Record(mesh);
    CHECK(f.Frame() == ModelPreparation::State::Meshes);
    f.mHost.uploader.Complete(mesh);
    REQUIRE(f.Frame() == ModelPreparation::State::Ready);

    auto model = f.mPreparation.Take();
    REQUIRE(model);
    const auto &primitives = model->meshes.at(kMesh.value);
    REQUIRE(primitives.size() == 1);
    CHECK(primitives[0].mesh == mesh);
    CHECK(primitives[0].factor == glm::vec4(0.25f, 0.5f, 0.75f, 1));
    CHECK(f.mPreparation.Take() == nullptr);

    // dropping the claims keeps the records for the next consumer
    model.reset();
    CHECK(f.mHost.releasedMaterials.empty());
    CHECK(f.mHost.uploader.retired.empty());
    CHECK(f.mTextures.TextureState(texture) == TextureUploadState::Ready);
    f.mHost.MaterialRecords()->EvictUnused();
    CHECK(f.mHost.releasedMaterials.size() == 2);
}

TEST_CASE("A second preparation of the same model shares the mesh and material records",
    "[animation][model-preparation]")
{
    Fixture f;
    f.QueueTexture();
    const TextureHandle texture{ 0, 1 };
    f.mTextures.Begin(texture);
    f.mTextures.CopyAll(texture);
    f.mTextures.Complete(texture);
    f.Frame();
    const MeshHandle mesh{ 0, 1 };
    f.mHost.uploader.Record(mesh);
    f.mHost.uploader.Complete(mesh);
    REQUIRE(f.Frame() == ModelPreparation::State::Ready);
    auto first = f.mPreparation.Take();

    ModelPreparation other(f.mHost, f.mTextures, kError, &f.mService, kModel, "model");
    for (int frame = 0; frame < 8 && other.GetState() != ModelPreparation::State::Ready; ++frame)
    {
        f.mService.DrainCompletions();
        other.Advance();
    }
    REQUIRE(other.GetState() == ModelPreparation::State::Ready);
    CHECK(f.mHost.uploader.queued == 1);
    CHECK(f.mHost.createdMaterials.size() == 2);
    CHECK(f.mTextures.mQueued == 1);
    auto second = other.Take();
    CHECK(second->meshes.at(kMesh.value)[0].mesh == first->meshes.at(kMesh.value)[0].mesh);
    CHECK(second->meshes.at(kMesh.value)[0].material == first->meshes.at(kMesh.value)[0].material);

    // the records stay while either model holds them
    first.reset();
    f.mHost.MaterialRecords()->EvictUnused();
    CHECK(f.mHost.releasedMaterials.empty());
    second.reset();
    f.mHost.MaterialRecords()->EvictUnused();
    CHECK(f.mHost.releasedMaterials.size() == 2);
}

TEST_CASE("A failed mesh upload fails the model and drops its claims", "[animation][model-preparation]")
{
    Fixture f;
    f.QueueTexture();
    const TextureHandle texture{ 0, 1 };
    f.mTextures.Fail(texture);
    REQUIRE(f.Frame() == ModelPreparation::State::Meshes);
    f.mHost.uploader.Fail(MeshHandle{ 0, 1 });
    CHECK(f.Frame() == ModelPreparation::State::Failed);
    CHECK(f.mPreparation.Error() == "GPU mesh creation failed");
    CHECK(f.mPreparation.Take() == nullptr);
    // an unused failed record leaves the cache, the next request uploads again
    CHECK(f.mHost.uploader.retired.size() == 1);
}

TEST_CASE("A failed texture publishes the magenta error appearance", "[animation][model-preparation]")
{
    Fixture f;
    f.QueueTexture();
    const TextureHandle texture{ 0, 1 };
    f.mTextures.Fail(texture);
    REQUIRE(f.Frame() == ModelPreparation::State::Meshes);
    REQUIRE(f.mHost.createdMaterials.size() == 2);
    CHECK(f.mHost.createdMaterials[0].baseColorTexture == kError);
    f.mHost.uploader.Complete(MeshHandle{ 0, 1 });
    REQUIRE(f.Frame() == ModelPreparation::State::Ready);
    const auto model = f.mPreparation.Take();
    REQUIRE(model);
    CHECK(model->meshes.at(kMesh.value)[0].factor == glm::vec4(1, 0, 1, 1));
}

TEST_CASE("A missing texture blob settles as an error appearance", "[animation][model-preparation]")
{
    Fixture f(false);
    for (int frame = 0; frame < 4 && f.mPreparation.GetState() != ModelPreparation::State::Meshes; ++frame)
        f.Frame();
    // an unreachable texture is a texture failure, the model still publishes
    REQUIRE(f.mPreparation.GetState() == ModelPreparation::State::Meshes);
    f.mHost.uploader.Complete(MeshHandle{ 0, 1 });
    REQUIRE(f.Frame() == ModelPreparation::State::Ready);
    const auto model = f.mPreparation.Take();
    CHECK(model->meshes.at(kMesh.value)[0].factor == glm::vec4(1, 0, 1, 1));
}

TEST_CASE("A preparation without an asset service fails", "[animation][model-preparation]")
{
    test::FakeMeshResolveHost host;
    test::FakeTextureUploader textures;
    ModelPreparation preparation(host, textures, kError, nullptr, kModel, "model");
    CHECK(preparation.Advance() == ModelPreparation::State::Failed);
    CHECK_FALSE(preparation.Error().empty());
    CHECK(preparation.Take() == nullptr);
}

TEST_CASE("Abandoning a preparation drops its texture claim", "[animation][model-preparation]")
{
    CookedModel cooked;
    AssetsModule assets(cooked.Path());
    JobExecutor jobs(0);
    AsyncAssetService service(assets, jobs, 1);
    test::FakeMeshResolveHost host;
    test::FakeTextureUploader textures;
    {
        ModelPreparation preparation(host, textures, kError, &service, kModel, "model");
        for (int frame = 0; frame < 8 && textures.mEntries.empty(); ++frame)
        {
            service.DrainCompletions();
            preparation.Advance();
        }
        REQUIRE(textures.mEntries.size() == 1);
    }
    CHECK(textures.mRetired.size() == 1);
    CHECK(host.createdMaterials.empty());
}

TEST_CASE("A reload with unchanged content republishes from the existing records", "[animation][model-preparation]")
{
    Fixture f;
    f.QueueTexture();
    const TextureHandle texture{ 0, 1 };
    f.mTextures.Begin(texture);
    f.mTextures.CopyAll(texture);
    f.mTextures.Complete(texture);
    f.Frame();
    const MeshHandle mesh{ 0, 1 };
    f.mHost.uploader.Record(mesh);
    f.mHost.uploader.Complete(mesh);
    REQUIRE(f.Frame() == ModelPreparation::State::Ready);
    const auto before = f.mPreparation.Take();
    const auto resources = before->resources;

    SECTION("an identical cook")
    {
        f.Write();
        f.mAssets.BeginReload();
        f.mAssets.CommitReload();
        ModelPreparation after(f.mHost, f.mTextures, kError, &f.mService, kModel, "model");
        // the decoded model is adopted, so nothing waits on the lane
        REQUIRE(after.Advance() == ModelPreparation::State::Ready);
        const auto replacement = after.Take();
        CHECK(replacement->resources == resources);
        CHECK(replacement->meshes.at(kMesh.value)[0].mesh == mesh);
        CHECK(replacement->meshes.at(kMesh.value)[0].material == before->meshes.at(kMesh.value)[0].material);
        CHECK(f.mHost.uploader.queued == 1);
        CHECK(f.mTextures.mQueued == 1);
        CHECK(f.mHost.createdMaterials.size() == 2);
    }
    SECTION("a cook that changes only the mesh")
    {
        f.Write(true, 1);
        f.mAssets.BeginReload();
        f.mAssets.CommitReload();
        ModelPreparation after(f.mHost, f.mTextures, kError, &f.mService, kModel, "model");
        for (int frame = 0; frame < 8 && after.GetState() != ModelPreparation::State::Meshes; ++frame)
        {
            f.mService.DrainCompletions();
            after.Advance();
        }
        REQUIRE(after.GetState() == ModelPreparation::State::Meshes);
        // a new mesh upload, the texture and both materials are the same records
        CHECK(f.mHost.uploader.queued == 2);
        CHECK(f.mTextures.mQueued == 1);
        CHECK(f.mHost.createdMaterials.size() == 2);
        f.mHost.uploader.Complete(MeshHandle{ 1, 1 });
        REQUIRE(after.Advance() == ModelPreparation::State::Ready);
        const auto replacement = after.Take();
        CHECK(replacement->meshes.at(kMesh.value)[0].mesh == MeshHandle{ 1, 1 });
        CHECK(replacement->meshes.at(kMesh.value)[0].material == before->meshes.at(kMesh.value)[0].material);
        // the old model still draws its own mesh until it is released
        CHECK(before->meshes.at(kMesh.value)[0].mesh == mesh);
    }
}

namespace
{
    // The renderer-wide library with one World's worth of fakes behind it
    struct LibraryFixture : CookedModel
    {
        LibraryFixture() : mAssets(Path()), mJobs(0), mService(mAssets, mJobs, 1), mLibrary(mHost, mTextures, kError) {}

        // Steps frames, finishing every upload the fakes hold, until the entry settles
        void Settle(const ModelLibrary::Claim &claim)
        {
            for (int frame = 0; frame < 16 && !claim->ready && claim->failure.empty(); ++frame)
            {
                mService.DrainCompletions();
                mLibrary.Advance();
                for (const auto &[index, texture] : std::vector(mTextures.mEntries.begin(), mTextures.mEntries.end()))
                    if (texture.mState != TextureUploadState::Ready)
                        mTextures.Complete(TextureHandle{ index, 1 });
                for (const auto &[index, mesh] : std::vector(mHost.uploader.meshes.begin(), mHost.uploader.meshes.end()))
                    if (mesh.state != MeshUploadState::Ready)
                    {
                        if (mesh.state == MeshUploadState::Pending)
                            mHost.uploader.Record(MeshHandle{ index, 1 });
                        mHost.uploader.Complete(MeshHandle{ index, 1 });
                    }
            }
        }

        void Reload()
        {
            mAssets.BeginReload();
            mAssets.CommitReload();
        }

        AssetsModule mAssets;
        JobExecutor mJobs;
        AsyncAssetService mService;
        test::FakeMeshResolveHost mHost;
        test::FakeTextureUploader mTextures;
        ModelLibrary mLibrary;
    };
}

TEST_CASE("Worlds that ask for one model share one prepared model", "[animation][model-library]")
{
    LibraryFixture f;
    const ModelLibrary::Claim first = f.mLibrary.Acquire(kModel, &f.mService, "model");
    const ModelLibrary::Claim second = f.mLibrary.Acquire(kModel, &f.mService, "model");
    CHECK(first == second);
    f.Settle(first);
    REQUIRE(first->ready);
    CHECK(second->ready == first->ready);
    CHECK(f.mHost.uploader.queued == 1);
    CHECK(f.mTextures.mQueued == 1);
    CHECK(f.mHost.createdMaterials.size() == 2);
}

TEST_CASE("The last claim frees the prepared model and the records stay for the next one", "[animation][model-library]")
{
    LibraryFixture f;
    {
        const ModelLibrary::Claim claim = f.mLibrary.Acquire(kModel, &f.mService, "model");
        f.Settle(claim);
        REQUIRE(claim->ready);
    }
    // nothing holds the entry, the renderer still holds its GPU records
    CHECK(f.mHost.releasedMaterials.empty());
    const ModelLibrary::Claim again = f.mLibrary.Acquire(kModel, &f.mService, "model");
    f.Settle(again);
    REQUIRE(again->ready);
    CHECK(f.mHost.uploader.queued == 1);
    CHECK(f.mHost.createdMaterials.size() == 2);
}

TEST_CASE("A reload with the same content is the same entry and changed content is another", "[animation][model-library]")
{
    LibraryFixture f;
    const ModelLibrary::Claim before = f.mLibrary.Acquire(kModel, &f.mService, "model");
    f.Settle(before);
    REQUIRE(before->ready);

    SECTION("an identical cook")
    {
        f.Write();
        f.Reload();
        CHECK(f.mLibrary.Acquire(kModel, &f.mService, "model") == before);
    }
    SECTION("a cook that changes a mesh")
    {
        f.Write(true, 1);
        f.Reload();
        const ModelLibrary::Claim after = f.mLibrary.Acquire(kModel, &f.mService, "model");
        REQUIRE(after != before);
        CHECK(after->content != before->content);
        f.Settle(after);
        REQUIRE(after->ready);
        // the old model still draws its own mesh while the new one is ready
        CHECK(before->ready);
        CHECK(after->ready->meshes.at(kMesh.value)[0].mesh != before->ready->meshes.at(kMesh.value)[0].mesh);
        CHECK(after->ready->meshes.at(kMesh.value)[0].material == before->ready->meshes.at(kMesh.value)[0].material);
    }
}

TEST_CASE("A failed entry is prepared again by the next claim", "[animation][model-library]")
{
    LibraryFixture f;
    const ModelLibrary::Claim claim = f.mLibrary.Acquire(kModel, &f.mService, "model");
    // the mesh upload fails, which fails the model
    for (int frame = 0; frame < 16 && claim->failure.empty(); ++frame)
    {
        f.mService.DrainCompletions();
        f.mLibrary.Advance();
        for (const auto &[index, texture] : std::vector(f.mTextures.mEntries.begin(), f.mTextures.mEntries.end()))
            f.mTextures.Complete(TextureHandle{ index, 1 });
        for (const auto &[index, mesh] : std::vector(f.mHost.uploader.meshes.begin(), f.mHost.uploader.meshes.end()))
            f.mHost.uploader.Fail(MeshHandle{ index, 1 });
    }
    REQUIRE_FALSE(claim->failure.empty());
    const ModelLibrary::Claim retry = f.mLibrary.Acquire(kModel, &f.mService, "model");
    CHECK(retry != claim);
    f.Settle(retry);
    CHECK(retry->ready);
}

TEST_CASE("The renderer releasing its resources empties every entry", "[animation][model-library]")
{
    LibraryFixture f;
    const ModelLibrary::Claim claim = f.mLibrary.Acquire(kModel, &f.mService, "model");
    f.Settle(claim);
    REQUIRE(claim->ready);
    f.mLibrary.ReleaseAll();
    CHECK_FALSE(claim->ready);
    CHECK_FALSE(claim->failure.empty());
    // a later claim starts from nothing
    CHECK(f.mLibrary.Acquire(kModel, &f.mService, "model") != claim);
}

/**
 * @file ModelPreparation.tests.cpp
 * @author sumin.park
 * @brief Background model preparation, texture settling, error appearance and release tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <animation/ModelLibrary.h>
#include <animation/ModelPlayer.h>
#include <animation/ModelPreparation.h>
#include <animation/ModelSystem.h>
#include <assets/AssetResolver.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/SceneLoadGroup.h>
#include <core/ecs/World.h>

#include <assets/AssetsModule.h>
#include <assets/ModelAsset.h>
#include <core/fs/FileIO.h>
#include <jobs/JobExecutor.h>

#include "support/CookedMeshDir.h"
#include "support/FakeMeshResolveHost.h"
#include "support/FakeTextureUploader.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <format>

using namespace mir;

namespace
{
    const AssetId kModel = MakeAssetId("model");
    const AssetId kModel2 = MakeAssetId("model-2");
    const AssetId kMesh = MakeAssetId("mesh");
    const AssetId kMaterial = MakeAssetId("material");
    const AssetId kTexture = MakeAssetId("texture");
    const TextureHandle kError{777, 1};

    struct CookedModel : test::CookedDir
    {
        explicit CookedModel(bool withTexture = true, bool withPbr = false)
        {
            Write(withTexture, 0, 0, false, false, withPbr);
        }

        // Blob names carry a content hash like a cook's, a variant changes that blob alone. A second model names
        // the same mesh.
        void Write(bool withTexture = true, uint32_t meshVariant = 0, uint32_t textureVariant = 0,
                   bool secondModel = false, bool withMorph = false, bool withPbr = false)
        {
            ModelAsset model;
            model.nodes.resize(1);
            model.nodes[0].key = "root";
            model.nodes[0].mesh = kMesh;
            model.scenes = {{"scene", {0}}};
            MeshPrimitive primitive;
            primitive.vertices.resize(3);
            primitive.vertices[0].position.x = static_cast<float>(meshVariant);
            primitive.indices = {0, 1, 2};
            primitive.material = kMaterial;
            primitive.baseColorTexture = kTexture;
            if (withMorph)
            {
                model.nodes[0].weights = {0};
                primitive.morphTargets.push_back({std::vector<glm::vec3>(3, glm::vec3(1, 0, 0)),
                                                  std::vector<glm::vec3>(3), std::vector<glm::vec3>(3)});
            }
            MeshAsset mesh;
            if (withMorph)
                mesh.weights = {0};
            mesh.primitives.push_back(primitive);
            MaterialAsset material;
            material.baseColor = glm::vec4(0.25f, 0.5f, 0.75f, 1);
            material.baseColorTexture = kTexture;
            if (withPbr)
            {
                material.metallic = .3f;
                material.roughness = .7f;
                material.specular = .5f;
                material.clearcoat = .8f;
                material.textures[0].texture = kTexture;
                material.textures[0].minFilter = 9987;
                material.textures[1].texture = kTexture;
                material.textures[1].wrapS = 33071;
            }
            TextureAsset texture;
            texture.width = texture.height = 4;
            texture.rgba.assign(64, static_cast<uint8_t>(80 + textureVariant));

            const std::string modelFile = "model-0000000000000011.blob",
                              meshFile = std::format("mesh-{:016x}.blob", 0x12 + meshVariant),
                              materialFile = "material-0000000000000013.blob",
                              textureFile = std::format("texture-{:016x}.blob", 0x14 + textureVariant),
                              model2File = "model2-0000000000000015.blob";
            const std::vector<std::pair<std::string, std::vector<std::byte>>> blobs{
                {modelFile, BuildTypedAsset(model)},
                {meshFile, BuildTypedAsset(mesh)},
                {materialFile, BuildTypedAsset(material)},
                {textureFile, BuildTypedAsset(texture)}};
            for (const auto &[file, bytes] : blobs)
                REQUIRE(WriteBytes(Path() / file, bytes));
            REQUIRE(WriteBytes(Path() / model2File, BuildTypedAsset(model)));
            const AssetId modelEdges[]{kMesh}, meshEdges[]{kMaterial}, materialEdges[]{kTexture};
            std::vector<AssetManifestSourceEntry> entries{
                {{kModel, kModelAssetTypeTag, kModelAssetContentVersion}, modelFile, "model", modelEdges},
                {{kMesh, kMeshAssetTypeTag, kMeshAssetContentVersion}, meshFile, "mesh", meshEdges},
                {{kMaterial, kMaterialAssetTypeTag, kMaterialAssetContentVersion},
                 materialFile,
                 "material",
                 materialEdges},
                {{kTexture, kTextureAssetTypeTag, kTextureAssetContentVersion}, textureFile, "texture", {}}};
            if (secondModel)
                entries.push_back(
                    {{kModel2, kModelAssetTypeTag, kModelAssetContentVersion}, model2File, "model-2", modelEdges});
            // a texture the manifest does not list, its material still names it
            if (!withTexture)
            {
                entries.erase(entries.begin() + 3);
                entries[2].dependencies = {};
            }
            REQUIRE(WriteBytes(Path() / kAssetManifestFileName, BuildAssetManifestBlob(entries)));
        }
    };

    struct Fixture : CookedModel
    {
        explicit Fixture(bool withTexture = true, bool withPbr = false)
            : CookedModel(withTexture, withPbr), mAssets(Path()), mJobs(0), mService(mAssets, mJobs, 1),
              mPreparation(mHost, mTextures, kError, &mService, kModel, "model")
        {
        }

        ModelPreparation::State Frame()
        {
            mService.DrainCompletions();
            return mPreparation.Advance();
        }

        void QueueTexture()
        {
            for (int frame = 0; frame < 16 && mTextures.mEntries.empty(); ++frame)
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

TEST_CASE("Model PBR bindings share texture uploads but preserve samplers and factors",
          "[animation][model-preparation][pbr]")
{
    Fixture f(true, true);
    f.QueueTexture();
    REQUIRE(f.mTextures.mEntries.size() == 1);
    const TextureHandle texture{0, 1};
    f.mTextures.Begin(texture);
    f.mTextures.CopyAll(texture);
    f.mTextures.Complete(texture);
    REQUIRE(f.Frame() == ModelPreparation::State::Meshes);
    REQUIRE(f.mHost.createdMaterials.size() == 2);
    for (const auto &desc : f.mHost.createdMaterials)
    {
        CHECK(desc.model.textures[0].texture == texture);
        CHECK(desc.model.textures[1].texture == texture);
        CHECK(desc.model.textures[0].sampler.useMips);
        CHECK(desc.model.textures[1].sampler.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        CHECK(desc.model.factors[0].x == .3f);
        CHECK(desc.model.factors[0].y == .7f);
        CHECK(desc.model.factors[2].w == .5f);
        CHECK(desc.model.factors[3].y == .8f);
    }
}

TEST_CASE("A model publishes after its texture and meshes are ready and uses both", "[animation][model-preparation]")
{
    Fixture f;
    f.QueueTexture();
    const TextureHandle texture{0, 1};
    CHECK(f.Frame() == ModelPreparation::State::Textures);
    CHECK(f.mHost.createdMaterials.empty());
    f.mTextures.Begin(texture);
    f.mTextures.CopyAll(texture);
    f.mTextures.Complete(texture);
    REQUIRE(f.Frame() == ModelPreparation::State::Meshes);
    REQUIRE(f.mHost.createdMaterials.size() == 2);
    CHECK(f.mHost.createdMaterials[0].baseColorTexture == texture);
    CHECK(f.mHost.createdMaterials[0].frontFace != f.mHost.createdMaterials[1].frontFace);
    REQUIRE(f.mHost.uploader.queued == 1);
    CHECK(f.mHost.syncMeshes == 0);
    CHECK(f.mPreparation.Take() == nullptr);
    const MeshHandle mesh{0, 1};
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
    const TextureHandle texture{0, 1};
    f.mTextures.Begin(texture);
    f.mTextures.CopyAll(texture);
    f.mTextures.Complete(texture);
    f.Frame();
    const MeshHandle mesh{0, 1};
    f.mHost.uploader.Record(mesh);
    f.mHost.uploader.Complete(mesh);
    REQUIRE(f.Frame() == ModelPreparation::State::Ready);
    auto first = f.mPreparation.Take();

    ModelPreparation other(f.mHost, f.mTextures, kError, &f.mService, kModel, "model");
    for (int frame = 0; frame < 16 && other.GetState() != ModelPreparation::State::Ready; ++frame)
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
    const TextureHandle texture{0, 1};
    f.mTextures.Fail(texture);
    REQUIRE(f.Frame() == ModelPreparation::State::Meshes);
    f.mHost.uploader.Fail(MeshHandle{0, 1});
    CHECK(f.Frame() == ModelPreparation::State::Failed);
    CHECK(f.mPreparation.Error() == "GPU mesh creation failed");
    CHECK(f.mPreparation.Take() == nullptr);
    CHECK(f.mHost.uploader.retired.size() == 1);
}

TEST_CASE("A failed texture publishes the magenta error appearance", "[animation][model-preparation]")
{
    Fixture f;
    f.QueueTexture();
    const TextureHandle texture{0, 1};
    f.mTextures.Fail(texture);
    REQUIRE(f.Frame() == ModelPreparation::State::Meshes);
    REQUIRE(f.mHost.createdMaterials.size() == 2);
    CHECK(f.mHost.createdMaterials[0].baseColorTexture == kError);
    f.mHost.uploader.Complete(MeshHandle{0, 1});
    REQUIRE(f.Frame() == ModelPreparation::State::Ready);
    const auto model = f.mPreparation.Take();
    REQUIRE(model);
    CHECK(model->meshes.at(kMesh.value)[0].factor == glm::vec4(1, 0, 1, 1));
}

TEST_CASE("A missing texture blob settles as an error appearance", "[animation][model-preparation]")
{
    Fixture f(false);
    for (int frame = 0; frame < 12 && f.mPreparation.GetState() != ModelPreparation::State::Meshes; ++frame)
        f.Frame();
    REQUIRE(f.mPreparation.GetState() == ModelPreparation::State::Meshes);
    f.mHost.uploader.Complete(MeshHandle{0, 1});
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
        for (int frame = 0; frame < 16 && textures.mEntries.empty(); ++frame)
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
    const TextureHandle texture{0, 1};
    f.mTextures.Begin(texture);
    f.mTextures.CopyAll(texture);
    f.mTextures.Complete(texture);
    f.Frame();
    const MeshHandle mesh{0, 1};
    f.mHost.uploader.Record(mesh);
    f.mHost.uploader.Complete(mesh);
    REQUIRE(f.Frame() == ModelPreparation::State::Ready);
    const auto before = f.mPreparation.Take();
    const auto resources = before->resources;
    const auto frames = [&](ModelPreparation &preparation) {
        for (int frame = 0; frame < 12 && preparation.GetState() != ModelPreparation::State::Ready; ++frame)
        {
            f.mService.DrainCompletions();
            preparation.Advance();
        }
    };

    SECTION("an identical cook")
    {
        f.Write();
        f.mAssets.BeginReload();
        f.mAssets.CommitReload();
        ModelPreparation after(f.mHost, f.mTextures, kError, &f.mService, kModel, "model");
        frames(after);
        REQUIRE(after.GetState() == ModelPreparation::State::Ready);
        const auto replacement = after.Take();
        CHECK(replacement->resources->meshes.at(kMesh.value) == resources->meshes.at(kMesh.value));
        CHECK(replacement->resources->materials.at(kMaterial.value) == resources->materials.at(kMaterial.value));
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
        for (int frame = 0; frame < 16 && after.GetState() != ModelPreparation::State::Meshes; ++frame)
        {
            f.mService.DrainCompletions();
            after.Advance();
        }
        REQUIRE(after.GetState() == ModelPreparation::State::Meshes);
        CHECK(f.mHost.uploader.queued == 2);
        CHECK(f.mTextures.mQueued == 1);
        CHECK(f.mHost.createdMaterials.size() == 2);
        f.mHost.uploader.Complete(MeshHandle{1, 1});
        REQUIRE(after.Advance() == ModelPreparation::State::Ready);
        const auto replacement = after.Take();
        CHECK(replacement->meshes.at(kMesh.value)[0].mesh == MeshHandle{1, 1});
        CHECK(replacement->meshes.at(kMesh.value)[0].material == before->meshes.at(kMesh.value)[0].material);
        CHECK(before->meshes.at(kMesh.value)[0].mesh == mesh);
    }
}

namespace
{
    // The renderer-wide library with one World's worth of fakes behind it
    struct LibraryFixture : CookedModel
    {
        LibraryFixture() : mAssets(Path()), mJobs(0), mService(mAssets, mJobs, 1), mLibrary(mHost, mTextures, kError)
        {
        }

        void Settle(const ModelLibrary::Claim &claim)
        {
            for (int frame = 0; frame < 40 && !claim->ready && claim->failure.empty(); ++frame)
            {
                mService.DrainCompletions();
                mLibrary.Advance();
                for (const auto &[index, texture] : std::vector(mTextures.mEntries.begin(), mTextures.mEntries.end()))
                    if (texture.mState != TextureUploadState::Ready)
                        mTextures.Complete(TextureHandle{index, 1});
                for (const auto &[index, mesh] :
                     std::vector(mHost.uploader.meshes.begin(), mHost.uploader.meshes.end()))
                    if (mesh.state != MeshUploadState::Ready)
                    {
                        if (mesh.state == MeshUploadState::Pending)
                            mHost.uploader.Record(MeshHandle{index, 1});
                        mHost.uploader.Complete(MeshHandle{index, 1});
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

TEST_CASE("Model draws retain the preceding morph sample and discard history after a visibility gap",
          "[animation][model-system][motion]")
{
    RegisterModelPlayer();
    mir::RegisterCoreComponents();
    LibraryFixture f;
    f.Write(true, 0, 0, false, true);
    const auto library = std::make_shared<ModelLibrary>(f.mHost, f.mTextures, kError);
    AssetResolver resolver(f.mAssets, "game");
    ModelSystem system(library, &f.mService, resolver);
    World world;
    CommandBuffer commands;
    world.EmplaceResource<EngineModeState>();
    auto &draws = world.EmplaceResource<ModelDrawItems>();
    auto &overrides = world.EmplaceResource<ModelMorphOverrides>();
    SystemContext context{world, commands, 0.016f, 0.0, 0};
    const auto frame = [&] {
        f.mService.DrainCompletions();
        system.OnUpdate(context);
        for (const auto &[index, texture] : std::vector(f.mTextures.mEntries.begin(), f.mTextures.mEntries.end()))
            f.mTextures.Complete(TextureHandle{index, 1});
        for (const auto &[index, mesh] : std::vector(f.mHost.uploader.meshes.begin(), f.mHost.uploader.meshes.end()))
            if (mesh.state != MeshUploadState::Ready)
            {
                if (mesh.state == MeshUploadState::Pending)
                    f.mHost.uploader.Record(MeshHandle{index, 1});
                f.mHost.uploader.Complete(MeshHandle{index, 1});
            }
    };
    const auto weight = [](std::span<const glm::vec4> sample) {
        REQUIRE(sample.size() >= 3);
        const auto offset = static_cast<uint32_t>(sample[1].z);
        REQUIRE(offset < sample.size());
        return sample[offset].x;
    };
    const Entity entity = world.CreateEntity();
    mir::AddTransform(world, entity);
    ModelPlayer player;
    player.SetModelAsset(kModel);
    world.AddComponent<ModelPlayer>(entity, player);
    for (int step = 0; step < 40 && draws.items.empty(); ++step)
        frame();
    REQUIRE(draws.items.size() == 1);
    CHECK(draws.items[0].previousDeformation.empty());
    CHECK(weight(draws.items[0].deformation) == 0.0f);

    overrides.instances[PackEntity(entity)]["root"] = {1.0f};
    frame();
    REQUIRE(draws.items.size() == 1);
    REQUIRE(draws.items[0].previousPose);
    CHECK(draws.items[0].previousPose->linear == draws.items[0].pose.linear);
    CHECK(weight(draws.items[0].deformation) == 1.0f);
    CHECK(weight(draws.items[0].previousDeformation) == 0.0f);
    overrides.instances[PackEntity(entity)]["root"] = {0.25f};
    frame();
    CHECK(weight(draws.items[0].deformation) == 0.25f);
    CHECK(weight(draws.items[0].previousDeformation) == 1.0f);

    world.RemoveComponent<ModelPlayer>(entity);
    frame();
    CHECK(draws.items.empty());
    world.AddComponent<ModelPlayer>(entity, player);
    for (int step = 0; step < 40 && draws.items.empty(); ++step)
        frame();
    REQUIRE(draws.items.size() == 1);
    CHECK(draws.items[0].previousDeformation.empty());
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
    CHECK(f.mHost.releasedMaterials.empty());
    const ModelLibrary::Claim again = f.mLibrary.Acquire(kModel, &f.mService, "model");
    f.Settle(again);
    REQUIRE(again->ready);
    CHECK(f.mHost.uploader.queued == 1);
    CHECK(f.mHost.createdMaterials.size() == 2);
}

TEST_CASE("A reload with the same content is the same entry and changed content is another",
          "[animation][model-library]")
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
        CHECK(before->ready);
        CHECK(after->ready->meshes.at(kMesh.value)[0].mesh != before->ready->meshes.at(kMesh.value)[0].mesh);
        CHECK(after->ready->meshes.at(kMesh.value)[0].material == before->ready->meshes.at(kMesh.value)[0].material);
    }
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
    CHECK(f.mLibrary.Acquire(kModel, &f.mService, "model") != claim);
}

namespace
{
    void StepUntil(Fixture &f, ModelPreparation &preparation, ModelPreparation::State state, int limit = 40)
    {
        for (int frame = 0; frame < limit && preparation.GetState() != state; ++frame)
        {
            f.mService.DrainCompletions();
            preparation.Advance();
        }
    }
}

TEST_CASE("A reload that lands during preparation supersedes it instead of mixing two catalogs",
          "[animation][model-preparation]")
{
    SECTION("a mesh changed after the model was decoded")
    {
        Fixture f;
        // the model is decoded under the first manifest, the mesh is still to be requested
        StepUntil(f, f.mPreparation, ModelPreparation::State::Records);
        REQUIRE(f.mPreparation.GetState() == ModelPreparation::State::Records);
        f.Write(true, 1);
        f.mAssets.BeginReload();
        f.mAssets.CommitReload();
        StepUntil(f, f.mPreparation, ModelPreparation::State::Superseded);
        CHECK(f.mPreparation.GetState() == ModelPreparation::State::Superseded);
        // the old manifest's vertices were never queued under the new content's key
        CHECK(f.mHost.uploader.queued == 0);
        CHECK(f.mHost.createdMaterials.empty());
        CHECK(f.mPreparation.Take() == nullptr);

        ModelPreparation fresh(f.mHost, f.mTextures, kError, &f.mService, kModel, "model");
        f.mTextures.mQueued = 0;
        for (int frame = 0; frame < 40 && fresh.GetState() != ModelPreparation::State::Meshes; ++frame)
        {
            f.mService.DrainCompletions();
            fresh.Advance();
            for (const auto &[index, texture] : std::vector(f.mTextures.mEntries.begin(), f.mTextures.mEntries.end()))
                f.mTextures.Complete(TextureHandle{index, 1});
        }
        REQUIRE(fresh.GetState() == ModelPreparation::State::Meshes);
        REQUIRE(f.mHost.uploader.queued == 1);
        CHECK(f.mHost.uploader.meshes.begin()->second.vertices == 3);
    }
    SECTION("a texture changed while the textures were loading")
    {
        Fixture f;
        StepUntil(f, f.mPreparation, ModelPreparation::State::Textures);
        REQUIRE(f.mPreparation.GetState() == ModelPreparation::State::Textures);
        f.Write(true, 0, 1);
        f.mAssets.BeginReload();
        f.mAssets.CommitReload();
        StepUntil(f, f.mPreparation, ModelPreparation::State::Superseded);
        CHECK(f.mPreparation.GetState() == ModelPreparation::State::Superseded);
        CHECK(f.mHost.createdMaterials.empty());
        CHECK(f.mHost.uploader.queued == 0);
    }
    SECTION("a reload that changes nothing the model names does not stop it")
    {
        Fixture f;
        StepUntil(f, f.mPreparation, ModelPreparation::State::Records);
        f.Write();
        f.mAssets.BeginReload();
        f.mAssets.CommitReload();
        f.QueueTexture();
        CHECK(f.mPreparation.GetState() != ModelPreparation::State::Superseded);
        CHECK(f.mPreparation.GetState() != ModelPreparation::State::Failed);
    }
}

TEST_CASE("The library gives a changed texture another entry although the model's records are the same",
          "[animation][model-library]")
{
    LibraryFixture f;
    const ModelLibrary::Claim before = f.mLibrary.Acquire(kModel, &f.mService, "model");
    f.Settle(before);
    REQUIRE(before->ready);
    f.Write(true, 0, 1);
    f.Reload();
    const ModelLibrary::Claim after = f.mLibrary.Acquire(kModel, &f.mService, "model");
    CHECK(after != before);
    CHECK(after->content != before->content);
}

TEST_CASE("Two models that name one mesh share its decoded record and its upload", "[animation][model-preparation]")
{
    Fixture f;
    f.Write(true, 0, 0, true);
    f.mAssets.BeginReload();
    f.mAssets.CommitReload();
    ModelPreparation first(f.mHost, f.mTextures, kError, &f.mService, kModel, "model");
    ModelPreparation second(f.mHost, f.mTextures, kError, &f.mService, kModel2, "model-2");
    const auto settle = [&] {
        for (int frame = 0; frame < 60; ++frame)
        {
            f.mService.DrainCompletions();
            first.Advance();
            second.Advance();
            for (const auto &[index, texture] : std::vector(f.mTextures.mEntries.begin(), f.mTextures.mEntries.end()))
                f.mTextures.Complete(TextureHandle{index, 1});
            for (const auto &[index, mesh] :
                 std::vector(f.mHost.uploader.meshes.begin(), f.mHost.uploader.meshes.end()))
                if (mesh.state != MeshUploadState::Ready)
                {
                    if (mesh.state == MeshUploadState::Pending)
                        f.mHost.uploader.Record(MeshHandle{index, 1});
                    f.mHost.uploader.Complete(MeshHandle{index, 1});
                }
            if (first.GetState() == ModelPreparation::State::Ready &&
                second.GetState() == ModelPreparation::State::Ready)
                return;
        }
    };
    settle();
    REQUIRE(first.GetState() == ModelPreparation::State::Ready);
    REQUIRE(second.GetState() == ModelPreparation::State::Ready);
    const auto a = first.Take();
    const auto b = second.Take();
    CHECK(a->resources->meshes.at(kMesh.value) == b->resources->meshes.at(kMesh.value));
    CHECK(a->resources->materials.at(kMaterial.value) == b->resources->materials.at(kMaterial.value));
    CHECK(a->meshes.at(kMesh.value)[0].mesh == b->meshes.at(kMesh.value)[0].mesh);
    CHECK(f.mHost.uploader.queued == 1);
}

TEST_CASE("A model nobody names any more is released while the scene stays open", "[animation][model-system]")
{
    RegisterModelPlayer();
    LibraryFixture f;
    const auto library = std::make_shared<ModelLibrary>(f.mHost, f.mTextures, kError);
    AssetResolver resolver(f.mAssets, "game");
    ModelSystem system(library, &f.mService, resolver);
    World world;
    CommandBuffer commands;
    world.EmplaceResource<SceneLoadGroup>();
    SystemContext context{world, commands, 0.016f, 0.0, 0};

    const auto frame = [&] {
        f.mService.DrainCompletions();
        system.OnLoadingUpdate(context);
        for (const auto &[index, texture] : std::vector(f.mTextures.mEntries.begin(), f.mTextures.mEntries.end()))
            f.mTextures.Complete(TextureHandle{index, 1});
        for (const auto &[index, mesh] : std::vector(f.mHost.uploader.meshes.begin(), f.mHost.uploader.meshes.end()))
            if (mesh.state != MeshUploadState::Ready)
            {
                if (mesh.state == MeshUploadState::Pending)
                    f.mHost.uploader.Record(MeshHandle{index, 1});
                f.mHost.uploader.Complete(MeshHandle{index, 1});
            }
    };

    const Entity first = world.CreateEntity();
    ModelPlayer player;
    player.SetModelAsset(kModel);
    world.AddComponent<ModelPlayer>(first, player);
    const Entity second = world.CreateEntity();
    world.AddComponent<ModelPlayer>(second, player);

    SceneLoadGroup &group = world.Resource<SceneLoadGroup>();
    group.Begin();
    for (int step = 0; step < 40 && !group.Ready(); ++step)
    {
        frame();
        group.EndFrame();
    }
    REQUIRE(group.Ready());
    CHECK(library->LiveEntries() == 1);

    world.DestroyEntity(first);
    frame();
    CHECK(library->LiveEntries() == 1);

    world.DestroyEntity(second);
    frame();
    CHECK(library->LiveEntries() == 0);
    CHECK(f.mHost.uploader.retired.empty());
    CHECK(f.mHost.releasedMaterials.empty());

    const Entity third = world.CreateEntity();
    world.AddComponent<ModelPlayer>(third, player);
    for (int step = 0; step < 40 && library->LiveEntries() == 0; ++step)
        frame();
    for (int step = 0; step < 20; ++step)
        frame();
    CHECK(library->LiveEntries() == 1);
    CHECK(f.mHost.uploader.queued == 1);
}

TEST_CASE("Clearing an entity's model selection releases the claim like deleting the entity",
          "[animation][model-system]")
{
    const bool fullUpdate = GENERATE(false, true);
    INFO(std::string(fullUpdate ? "the frame update" : "the loading tick"));
    RegisterModelPlayer();
    mir::RegisterCoreComponents();
    LibraryFixture f;
    const auto library = std::make_shared<ModelLibrary>(f.mHost, f.mTextures, kError);
    AssetResolver resolver(f.mAssets, "game");
    ModelSystem system(library, &f.mService, resolver);
    World world;
    CommandBuffer commands;
    world.EmplaceResource<EngineModeState>();
    world.EmplaceResource<ModelDrawItems>();
    world.EmplaceResource<ModelMorphOverrides>();
    SystemContext context{world, commands, 0.016f, 0.0, 0};
    const auto frame = [&] {
        f.mService.DrainCompletions();
        if (fullUpdate)
            system.OnUpdate(context);
        else
            system.OnLoadingUpdate(context);
        for (const auto &[index, texture] : std::vector(f.mTextures.mEntries.begin(), f.mTextures.mEntries.end()))
            f.mTextures.Complete(TextureHandle{index, 1});
        for (const auto &[index, mesh] : std::vector(f.mHost.uploader.meshes.begin(), f.mHost.uploader.meshes.end()))
            if (mesh.state != MeshUploadState::Ready)
            {
                if (mesh.state == MeshUploadState::Pending)
                    f.mHost.uploader.Record(MeshHandle{index, 1});
                f.mHost.uploader.Complete(MeshHandle{index, 1});
            }
    };

    const Entity entity = world.CreateEntity();
    mir::AddTransform(world, entity);
    ModelPlayer player;
    player.SetModelAsset(kModel);
    world.AddComponent<ModelPlayer>(entity, player);
    for (int step = 0; step < 40; ++step)
        frame();
    REQUIRE(library->LiveEntries() == 1);
    if (fullUpdate)
        REQUIRE(world.Resource<ModelDrawItems>().items.size() == 1);

    ModelPlayer *held = world.GetComponent<ModelPlayer>(entity);
    REQUIRE(held != nullptr);
    held->SetModelAsset(AssetId{});
    held->SetModelName("");
    frame();
    CHECK(library->LiveEntries() == 0);
    if (fullUpdate)
        CHECK(world.Resource<ModelDrawItems>().items.empty());

    held->SetModelAsset(kModel);
    for (int step = 0; step < 40; ++step)
        frame();
    CHECK(library->LiveEntries() == 1);
}

TEST_CASE("The model's own record is shared with the prepared model and counted by the asset service",
          "[animation][model-preparation]")
{
    LibraryFixture f;
    const ModelLibrary::Claim claim = f.mLibrary.Acquire(kModel, &f.mService, "model");
    f.Settle(claim);
    REQUIRE(claim->ready);
    // nothing but the prepared model holds the root record once the service lets its own copy go
    f.mService.EvictUnused();
    const uint64_t modelBytes = ModelRecordDecoder().sizeBytes(*claim->ready->resources->model);
    CHECK(modelBytes > sizeof(ModelAsset));
    CHECK(f.mService.DecodedBytes() >= modelBytes);
}

TEST_CASE("A texture that failed to upload is tried again by the next reload", "[animation][model-library]")
{
    LibraryFixture f;
    // the upload cannot be queued, so the texture record fails and the model draws magenta
    f.mTextures.mReject = true;
    const ModelLibrary::Claim before = f.mLibrary.Acquire(kModel, &f.mService, "model");
    f.Settle(before);
    REQUIRE(before->ready);
    REQUIRE(before->ready->meshes.at(kMesh.value)[0].factor == glm::vec4(1, 0, 1, 1));
    f.mTextures.mReject = false;

    SECTION("within the revision the failure is remembered")
    {
        const ModelLibrary::Claim again = f.mLibrary.Acquire(kModel, &f.mService, "model");
        CHECK(again == before);
        f.Settle(again);
        CHECK(f.mTextures.mQueued == 0);
    }
    SECTION("an identical cook")
    {
        f.Write();
        f.Reload();
        const ModelLibrary::Claim after = f.mLibrary.Acquire(kModel, &f.mService, "model");
        CHECK(after != before);
        f.Settle(after);
        REQUIRE(after->ready);
        CHECK(f.mTextures.mQueued == 1);
        CHECK(after->ready->meshes.at(kMesh.value)[0].factor == glm::vec4(0.25f, 0.5f, 0.75f, 1));
    }
    SECTION("a cook that changes only the mesh")
    {
        f.Write(true, 1);
        f.Reload();
        const ModelLibrary::Claim after = f.mLibrary.Acquire(kModel, &f.mService, "model");
        f.Settle(after);
        REQUIRE(after->ready);
        CHECK(f.mTextures.mQueued == 1);
        CHECK(after->ready->meshes.at(kMesh.value)[0].factor == glm::vec4(0.25f, 0.5f, 0.75f, 1));
    }
}

TEST_CASE("A model that failed is remembered within a revision and tried again after a reload",
          "[animation][model-library]")
{
    LibraryFixture f;
    const ModelLibrary::Claim claim = f.mLibrary.Acquire(kModel, &f.mService, "model");
    for (int frame = 0; frame < 40 && claim->failure.empty(); ++frame)
    {
        f.mService.DrainCompletions();
        f.mLibrary.Advance();
        for (const auto &[index, texture] : std::vector(f.mTextures.mEntries.begin(), f.mTextures.mEntries.end()))
            f.mTextures.Complete(TextureHandle{index, 1});
        for (const auto &[index, mesh] : std::vector(f.mHost.uploader.meshes.begin(), f.mHost.uploader.meshes.end()))
            f.mHost.uploader.Fail(MeshHandle{index, 1});
    }
    REQUIRE_FALSE(claim->failure.empty());

    // another World asking in the same revision gets the failure, not another attempt
    CHECK(f.mLibrary.Acquire(kModel, &f.mService, "model") == claim);
    // a reload allows another attempt although the files are the same
    f.Write();
    f.Reload();
    const ModelLibrary::Claim retry = f.mLibrary.Acquire(kModel, &f.mService, "model");
    CHECK(retry != claim);
    f.Settle(retry);
    CHECK(retry->ready);
}

TEST_CASE("A full material table makes the model wait for the slots instead of taking the error material",
          "[animation][model-preparation]")
{
    LibraryFixture f;
    f.mHost.materialLimit = 2;
    // the first model's records are all unused once its claim goes, and they hold both slots
    {
        const ModelLibrary::Claim first = f.mLibrary.Acquire(kModel, &f.mService, "model");
        f.Settle(first);
        REQUIRE(first->ready);
    }
    REQUIRE(f.mHost.createdMaterials.size() == 2);

    // a changed texture needs two materials of its own
    f.Write(true, 0, 1);
    f.Reload();
    const ModelLibrary::Claim second = f.mLibrary.Acquire(kModel, &f.mService, "model");
    f.Settle(second);
    // the unused records were released, and the slots stay taken until their frame completes
    CHECK_FALSE(second->ready);
    CHECK(second->failure.empty());
    CHECK(f.mHost.releasedMaterials.size() == 2);
    CHECK(f.mHost.createdMaterials.size() == 2);
    // no mesh was queued for a model that cannot be built yet
    CHECK(f.mHost.uploader.queued == 1);

    f.mHost.CompleteFrame();
    f.Settle(second);
    REQUIRE(second->ready);
    CHECK(f.mHost.createdMaterials.size() == 4);
    // real materials, not the error material, and the texture's own color
    CHECK(second->ready->meshes.at(kMesh.value)[0].material != f.mHost.ErrorMaterial());
    CHECK(second->ready->meshes.at(kMesh.value)[0].factor == glm::vec4(0.25f, 0.5f, 0.75f, 1));
    CHECK_FALSE(second->ready->degraded);
}

TEST_CASE("A material table that stays full past the wait gives the primitive the error material",
          "[animation][model-preparation]")
{
    Fixture f;
    f.mPreparation.SetMaterialWait(std::chrono::milliseconds(0));
    f.mHost.materialLimit = 0;
    f.QueueTexture();
    f.mTextures.Complete(TextureHandle{0, 1});
    for (int frame = 0; frame < 4 && f.mPreparation.GetState() != ModelPreparation::State::Meshes; ++frame)
        f.Frame();
    REQUIRE(f.mPreparation.GetState() == ModelPreparation::State::Meshes);
    f.mHost.uploader.Complete(MeshHandle{0, 1});
    REQUIRE(f.Frame() == ModelPreparation::State::Ready);
    const auto model = f.mPreparation.Take();
    REQUIRE(model);
    const PreparedPrimitive &primitive = model->meshes.at(kMesh.value)[0];
    CHECK(primitive.material == f.mHost.ErrorMaterial());
    CHECK(primitive.mirrored == f.mHost.ErrorMaterial());
    CHECK(primitive.factor == glm::vec4(1, 0, 1, 1));
    CHECK(model->degraded);
}

TEST_CASE("A material the renderer cannot make takes the error material at once", "[animation][model-preparation]")
{
    Fixture f;
    f.mHost.rejectMaterials = true;
    f.QueueTexture();
    f.mTextures.Complete(TextureHandle{0, 1});
    for (int frame = 0; frame < 4 && f.mPreparation.GetState() != ModelPreparation::State::Meshes; ++frame)
        f.Frame();
    // no wait, the default wait is seconds long
    REQUIRE(f.mPreparation.GetState() == ModelPreparation::State::Meshes);
    f.mHost.uploader.Complete(MeshHandle{0, 1});
    REQUIRE(f.Frame() == ModelPreparation::State::Ready);
    CHECK(f.mPreparation.Take()->meshes.at(kMesh.value)[0].material == f.mHost.ErrorMaterial());
}

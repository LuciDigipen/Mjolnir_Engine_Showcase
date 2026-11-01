/**
 * @file GpuRecords.tests.cpp
 * @author sumin.park
 * @brief Residency accounting, in-memory mesh records and value-keyed material records.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/GpuResidency.h>
#include <renderer/IMeshResolveHost.h>
#include <renderer/MaterialRecordCache.h>
#include <renderer/ModelMeshTracker.h>

#include <assets/AssetsModule.h>
#include <assets/ModelAsset.h>
#include <core/fs/FileIO.h>
#include <jobs/JobExecutor.h>

#include "support/CookedMeshDir.h"
#include "support/FakeMeshResolveHost.h"
#include "support/FakeTextureUploader.h"

#include <catch2/catch_test_macros.hpp>

using namespace mir;

namespace
{
    const AssetId kMeshAsset = MakeAssetId("model-mesh");
    const AssetId kTexture = MakeAssetId("texture");

    std::shared_ptr<const DecodedMesh> Triangle()
    {
        auto mesh = std::make_shared<DecodedMesh>();
        mesh->vertices.resize(3);
        mesh->indices = { 0, 1, 2 };
        return mesh;
    }

    GpuRecordKey KeyOf(uint32_t part, uint32_t revision = 1, bool rayTracing = true)
    {
        return { kMeshAsset, part, ContentIdentity::ForRevision(revision), MeshUploadOptions{ .rayTracing = rayTracing } };
    }

    // Records the upload and finishes it, as the frames would.
    void Finish(test::FakeMeshResolveHost &host, ModelMeshTracker &tracker, MeshHandle handle)
    {
        host.uploader.Record(handle);
        host.uploader.Complete(handle);
        tracker.Update();
    }
}

TEST_CASE("Residency counts held and retiring bytes against the total target and category limits",
    "[renderer][residency]")
{
    GpuResidency residency;
    uint64_t meshes = 100, textures = 300, retiring = 50;
    const uint32_t meshHolder = residency.AddHolder(GpuCategory::Mesh, [&] { return meshes; });
    residency.AddHolder(GpuCategory::Texture, [&] { return textures; });
    residency.SetRetiring(GpuCategory::Mesh, [&] { return retiring; });
    residency.SetTotalTarget(1000);

    CHECK(residency.Used(GpuCategory::Mesh) == 150);
    CHECK(residency.UsedTotal() == 450);
    CHECK(residency.Excess(GpuCategory::Mesh) == 0);

    SECTION("a total over the target is every category's excess")
    {
        residency.SetTotalTarget(400);
        CHECK(residency.Excess(GpuCategory::Mesh) == 50);
        CHECK(residency.Excess(GpuCategory::Texture) == 50);
        // retiring bytes count until the GPU releases them
        retiring = 0;
        CHECK(residency.Excess(GpuCategory::Mesh) == 0);
    }
    SECTION("a category limit applies to that category alone")
    {
        residency.SetCategoryLimit(GpuCategory::Texture, 100);
        CHECK(residency.Excess(GpuCategory::Texture) == 200);
        CHECK(residency.Excess(GpuCategory::Mesh) == 0);
    }
    SECTION("a removed holder reports nothing")
    {
        residency.RemoveHolder(meshHolder);
        CHECK(residency.Held(GpuCategory::Mesh) == 0);
        CHECK(residency.Used(GpuCategory::Mesh) == 50);
    }
}

TEST_CASE("A model mesh record uploads once for equal keys and splits on content or options",
    "[renderer][model-mesh]")
{
    test::FakeMeshResolveHost host;
    const auto tracker = host.ModelMeshes(nullptr);
    int made = 0;
    const auto make = [&made] { ++made; return Triangle(); };

    const AssetId first = tracker->Request(KeyOf(0), make, "a");
    const AssetId again = tracker->Request(KeyOf(0), make, "a");
    CHECK(first == again);
    CHECK(made == 1);
    // another primitive, catalog revision or ray tracing option is another record
    const AssetId part = tracker->Request(KeyOf(1), make, "b");
    const AssetId revision = tracker->Request(KeyOf(0, 2), make, "c");
    const AssetId options = tracker->Request(KeyOf(0, 1, false), make, "d");
    CHECK(part != first);
    CHECK(revision != first);
    CHECK(options != first);
    CHECK(made == 4);

    ModelMeshLease one(tracker, first), two(tracker, again);
    tracker->Update();
    REQUIRE(one.Get() != nullptr);
    CHECK(one.Get()->phase == AssetUploadPhase::Uploading);
    // the source is the only owner of the vertices once the data is recorded
    CHECK(host.uploader.queued == 4);
    Finish(host, *tracker, one.Get()->mesh);
    CHECK(two.Get()->phase == AssetUploadPhase::Ready);
    CHECK(two.Get()->mesh == one.Get()->mesh);
}

TEST_CASE("Unused mesh records leave oldest first when the renderer is over its target", "[renderer][model-mesh]")
{
    test::FakeMeshResolveHost host;
    const auto tracker = host.ModelMeshes(nullptr);
    // a triangle holds 3 * 64 + 3 * 4 bytes
    host.Residency().SetTotalTarget(300);

    const auto upload = [&](uint32_t part)
    {
        const AssetId id = tracker->Request(KeyOf(part), [] { return Triangle(); }, "mesh");
        ModelMeshLease lease(tracker, id);
        tracker->Update();
        Finish(host, *tracker, lease.Get()->mesh);
        return lease;
    };
    ModelMeshLease active = upload(0);
    {
        ModelMeshLease released = upload(1);
        CHECK(host.uploader.retired.empty());
    }
    // the second record is unused and the total is over the target, the active one is protected
    CHECK(host.uploader.retired.size() == 1);
    CHECK(active.Get() != nullptr);
    CHECK(active.Get()->phase == AssetUploadPhase::Ready);
}

namespace
{
    struct CookedTexture : test::CookedDir
    {
        CookedTexture()
        {
            TextureAsset texture;
            texture.width = texture.height = 4;
            texture.rgba.assign(64, 80);
            REQUIRE(WriteBytes(Path() / "texture.blob", BuildTypedAsset(texture)));
            const AssetManifestSourceEntry entry{ { kTexture, kTextureAssetTypeTag, kTextureAssetContentVersion },
                "texture.blob", "texture", {} };
            REQUIRE(WriteBytes(Path() / kAssetManifestFileName, BuildAssetManifestBlob(std::span(&entry, 1))));
        }
    };

    struct MaterialFixture : CookedTexture
    {
        MaterialFixture()
            : mAssets(Path()), mJobs(0), mService(mAssets, mJobs, 1), mTracker(mTextures.TextureLoads(&mService)),
              mCache(mHost.MaterialRecords())
        {}

        // A texture claim on a finished upload.
        TextureHandle ReadyTexture()
        {
            mTracker->Request(kTexture, "texture");
            TextureLoadLease claim(mTracker, kTexture);
            for (int frame = 0; frame < 8 && mTextures.mEntries.empty(); ++frame)
            {
                mService.DrainCompletions();
                mTracker->Update();
            }
            const TextureHandle handle = claim.Get()->texture;
            mTextures.Complete(handle);
            mTracker->Update();
            return handle;
        }

        std::vector<TextureLoadLease> Claims()
        {
            std::vector<TextureLoadLease> claims;
            claims.emplace_back(mTracker, kTexture);
            return claims;
        }

        AssetsModule mAssets;
        JobExecutor mJobs;
        AsyncAssetService mService;
        test::FakeMeshResolveHost mHost;
        test::FakeTextureUploader mTextures;
        std::shared_ptr<TextureLoadTracker> mTracker;
        std::shared_ptr<MaterialRecordCache> mCache;
    };
}

TEST_CASE("Materials equal by value share one record and any resolved difference splits it",
    "[renderer][material-records]")
{
    test::FakeMeshResolveHost host;
    const auto cache = host.MaterialRecords();
    std::vector<std::byte> bytesA(8, std::byte{ 3 }), bytesB(8, std::byte{ 3 });

    MaterialDesc desc;
    desc.shaderName = "model";
    desc.baseColorTexture = TextureHandle{ 4, 1 };
    desc.record = bytesA;
    const uint64_t first = cache->Acquire(desc, {});
    REQUIRE(first != 0);

    SECTION("equal values from different storage")
    {
        MaterialDesc same = desc;
        same.shaderName = std::string("mod") + "el";
        same.record = bytesB;
        CHECK(cache->Acquire(same, {}) == first);
        CHECK(host.createdMaterials.size() == 1);
    }
    SECTION("any field that changes the material")
    {
        const auto differs = [&](auto change)
        {
            MaterialDesc other = desc;
            change(other);
            return cache->Acquire(other, {}) != first;
        };
        CHECK(differs([](MaterialDesc &d) { d.frontFace = VK_FRONT_FACE_CLOCKWISE; }));
        CHECK(differs([](MaterialDesc &d) { d.alphaCutoff = 0.25f; }));
        CHECK(differs([](MaterialDesc &d) { d.alphaMode = MaterialAlphaMode::Blend; }));
        CHECK(differs([](MaterialDesc &d) { d.cullMode = VK_CULL_MODE_NONE; }));
        CHECK(differs([](MaterialDesc &d) { d.sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; }));
        CHECK(differs([](MaterialDesc &d) { d.shaderName = "default"; }));
        // the same slot with another generation is another texture
        CHECK(differs([](MaterialDesc &d) { d.baseColorTexture = TextureHandle{ 4, 2 }; }));
        CHECK(differs([&](MaterialDesc &d) { bytesB[0] = std::byte{ 9 }; d.record = bytesB; }));
    }
}

TEST_CASE("A material record keeps its texture claim until the record is evicted", "[renderer][material-records]")
{
    MaterialFixture f;
    const TextureHandle texture = f.ReadyTexture();

    MaterialDesc desc;
    desc.shaderName = "model";
    desc.baseColorTexture = texture;
    const uint64_t id = f.mCache->Acquire(desc, f.Claims());
    REQUIRE(id != 0);
    // the texture cache holds nothing back, so a texture leaves with its last claim
    f.mTracker->SetBudget(0);
    {
        MaterialLease lease(f.mCache, id);
        CHECK_FALSE(lease.Get().IsNull());
    }
    // unused records stay while the renderer is under its target, and so does the texture they sample
    CHECK(f.mCache->Size() == 1);
    CHECK(f.mTextures.mRetired.empty());
    CHECK(f.mHost.releasedMaterials.empty());

    uint64_t held = 1;
    f.mHost.Residency().AddHolder(GpuCategory::Mesh, [&] { return held; });
    f.mHost.Residency().SetTotalTarget(0);
    f.mCache->Trim();
    CHECK(f.mCache->Size() == 0);
    CHECK(f.mHost.releasedMaterials.size() == 1);
    // the last claim went with the record, so the texture left with it
    CHECK(f.mTextures.mRetired.size() == 1);
}

TEST_CASE("A claimed material record is never evicted under pressure", "[renderer][material-records]")
{
    test::FakeMeshResolveHost host;
    const auto cache = host.MaterialRecords();
    MaterialDesc desc;
    desc.shaderName = "model";
    MaterialLease lease(cache, cache->Acquire(desc, {}));
    host.Residency().SetTotalTarget(0);
    uint64_t held = 1;
    host.Residency().AddHolder(GpuCategory::Mesh, [&] { return held; });
    cache->Trim();
    CHECK(cache->Size() == 1);
    CHECK_FALSE(lease.Get().IsNull());
    lease.Reset();
    CHECK(cache->Size() == 0);
}

TEST_CASE("Releasing the renderer's materials leaves leases pointing at nothing", "[renderer][material-records]")
{
    test::FakeMeshResolveHost host;
    const auto cache = host.MaterialRecords();
    MaterialDesc desc;
    MaterialLease lease(cache, cache->Acquire(desc, {}));
    REQUIRE_FALSE(lease.Get().IsNull());
    host.ReleaseCachedMaterials();
    CHECK(lease.Get().IsNull());
    lease.Reset();
    CHECK(host.releasedMaterials.empty());
}

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
    const AssetId kTextureB = MakeAssetId("texture-b");

    std::shared_ptr<const DecodedMesh> Triangle()
    {
        auto mesh = std::make_shared<DecodedMesh>();
        mesh->vertices.resize(3);
        mesh->indices = {0, 1, 2};
        return mesh;
    }

    GpuRecordKey KeyOf(uint32_t part, uint32_t revision = 1, bool rayTracing = true)
    {
        return {kMeshAsset, part, ContentIdentity::ForRevision(revision), MeshUploadOptions{.rayTracing = rayTracing}};
    }

    void Finish(test::FakeMeshResolveHost &host, ModelMeshTracker &tracker, MeshHandle handle)
    {
        host.uploader.Record(handle);
        host.uploader.Complete(handle);
        tracker.Update();
    }
}

TEST_CASE("Residency occupies retiring bytes but only asks to free what is held beyond the target and limits",
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
        residency.SetTotalTarget(350);
        CHECK(residency.TotalExcess() == 50);
        CHECK(residency.Excess(GpuCategory::Mesh) == 50);
        CHECK(residency.Excess(GpuCategory::Texture) == 50);
        // the retiring bytes are already on their way out, so they do not change what is asked
        retiring = 0;
        CHECK(residency.Excess(GpuCategory::Mesh) == 50);
        retiring = 500;
        CHECK(residency.Excess(GpuCategory::Mesh) == 50);
        CHECK(residency.Used(GpuCategory::Mesh) == 600);
    }
    SECTION("memory that is only retiring asks for nothing")
    {
        residency.SetTotalTarget(400);
        CHECK(residency.UsedTotal() == 450);
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

TEST_CASE("A model mesh record uploads once for equal keys and splits on content or options", "[renderer][model-mesh]")
{
    test::FakeMeshResolveHost host;
    const auto tracker = host.ModelMeshes(nullptr);
    int made = 0;
    const auto make = [&made] {
        ++made;
        return Triangle();
    };

    const AssetId first = tracker->Request(KeyOf(0), make, "a");
    const AssetId again = tracker->Request(KeyOf(0), make, "a");
    CHECK(first == again);
    CHECK(made == 1);
    // another primitive, manifest revision or ray tracing option is another record
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
    CHECK(host.uploader.queued == 4);
    Finish(host, *tracker, one.Get()->mesh);
    CHECK(two.Get()->phase == AssetUploadPhase::Ready);
    CHECK(two.Get()->mesh == one.Get()->mesh);
}

TEST_CASE("Unused mesh records leave oldest first when the renderer is over its target", "[renderer][model-mesh]")
{
    test::FakeMeshResolveHost host;
    const auto tracker = host.ModelMeshes(nullptr);
    host.Residency().SetTotalTarget(300);

    const auto upload = [&](uint32_t part) {
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
            texture.rgba.assign(64, 90);
            REQUIRE(WriteBytes(Path() / "texture-b.blob", BuildTypedAsset(texture)));
            const AssetManifestSourceEntry entries[]{
                {{kTexture, kTextureAssetTypeTag, kTextureAssetContentVersion}, "texture.blob", "texture", {}},
                {{kTextureB, kTextureAssetTypeTag, kTextureAssetContentVersion}, "texture-b.blob", "texture-b", {}}};
            REQUIRE(WriteBytes(Path() / kAssetManifestFileName, BuildAssetManifestBlob(entries)));
        }
    };

    struct MaterialFixture : CookedTexture
    {
        MaterialFixture()
            : mAssets(Path()), mJobs(0), mService(mAssets, mJobs, 1), mTracker(mTextures.TextureLoads(&mService)),
              mCache(mHost.MaterialRecords())
        {
        }

        TextureHandle ReadyTexture(AssetId id = kTexture)
        {
            const size_t before = mTextures.mEntries.size();
            mTracker->Request(id, "texture");
            TextureLoadLease claim(mTracker, id);
            for (int frame = 0; frame < 8 && mTextures.mEntries.size() == before; ++frame)
            {
                mService.DrainCompletions();
                mTracker->Update();
            }
            const TextureHandle handle = claim.Get()->texture;
            mTextures.Complete(handle);
            mTracker->Update();
            return handle;
        }

        std::vector<TextureLoadLease> Claims(AssetId id = kTexture)
        {
            std::vector<TextureLoadLease> claims;
            claims.emplace_back(mTracker, id);
            return claims;
        }

        uint64_t Material(const char *shader, TextureHandle texture, AssetId id = kTexture)
        {
            MaterialDesc desc;
            desc.shaderName = shader;
            desc.baseColorTexture = texture;
            return mCache->Acquire(desc, Claims(id));
        }

        void Pressure(uint64_t bytes)
        {
            mHolderBytes = bytes;
            mHost.Residency().AddHolder(GpuCategory::Mesh, [this] { return mHolderBytes; });
            mHost.Residency().SetTotalTarget(0);
        }

        uint64_t mHolderBytes = 0;

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
    std::vector<std::byte> bytesA(8, std::byte{3}), bytesB(8, std::byte{3});

    MaterialDesc desc;
    desc.shaderName = "model";
    desc.baseColorTexture = TextureHandle{4, 1};
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
        const auto differs = [&](auto change) {
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
        CHECK(differs([](MaterialDesc &d) { d.baseColorTexture = TextureHandle{4, 2}; }));
        CHECK(differs([&](MaterialDesc &d) {
            bytesB[0] = std::byte{9};
            d.record = bytesB;
        }));
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
    // Model PBR records now own 224 bytes even without textures, so pressure
    // can reclaim the unclaimed record immediately.
    CHECK(cache->RecordBytes() == 224);
    lease.Reset();
    CHECK(cache->Size() == 0);
    cache->EvictUnused();
    CHECK(cache->Size() == 0);
}

TEST_CASE("Pressure releases only the unused records that give something back, oldest first",
          "[renderer][material-records]")
{
    MaterialFixture f;
    const TextureHandle first = f.ReadyTexture(kTexture);
    const TextureHandle second = f.ReadyTexture(kTextureB);
    // both textures are held by nothing but records, and a bare record holds neither a texture nor table bytes
    const uint64_t a = f.Material("a", first, kTexture);
    MaterialDesc bareDesc;
    bareDesc.shaderName = "bare";
    const uint64_t bare = f.mCache->Acquire(bareDesc, {});
    const uint64_t b = f.Material("b", second, kTextureB);
    for (const uint64_t id : {a, bare, b})
        MaterialLease(f.mCache, id).Reset();
    REQUIRE(f.mCache->Size() == 3);
    CHECK(f.mCache->PinnedTextureBytes() > 0);

    f.Pressure(1);
    f.mCache->Trim();
    // the pressure needed one byte, the oldest record that frees a texture is enough
    CHECK(f.mCache->Size() == 2);
    CHECK(f.mHost.releasedMaterials.size() == 1);
    CHECK(f.mHost.createdMaterials[0].shaderName == "a");
}

TEST_CASE("A texture another holder still claims is not freed by releasing a record", "[renderer][material-records]")
{
    MaterialFixture f;
    const TextureHandle texture = f.ReadyTexture(kTexture);
    f.mTextures.mRetired.clear();
    // the texture is also claimed by something that stays
    TextureLoadLease holder(f.mTracker, UploadKey{kTexture});
    const uint64_t id = f.Material("a", texture, kTexture);
    MaterialLease(f.mCache, id).Reset();
    CHECK(f.mCache->PinnedTextureBytes() == 0);
    f.Pressure(1);
    f.mCache->Trim();
    CHECK(f.mCache->Size() == 1);
    CHECK(f.mHost.releasedMaterials.empty());
}

TEST_CASE("Records that share a texture go together when nothing else claims it", "[renderer][material-records]")
{
    MaterialFixture f;
    const TextureHandle texture = f.ReadyTexture(kTexture);
    const uint64_t one = f.Material("one", texture, kTexture), two = f.Material("two", texture, kTexture);
    MaterialLease(f.mCache, one).Reset();
    MaterialLease(f.mCache, two).Reset();
    REQUIRE(f.mCache->Size() == 2);
    const uint64_t bytes = f.mCache->PinnedTextureBytes();
    CHECK(bytes > 0);
    f.Pressure(bytes);
    f.mCache->Trim();
    CHECK(f.mCache->Size() == 0);
}

TEST_CASE("A full material table is waited out, not failed, until the released slots come back",
          "[renderer][material-records]")
{
    test::FakeMeshResolveHost host;
    const auto cache = host.MaterialRecords();
    host.materialLimit = 2;
    MaterialDesc desc;
    desc.shaderName = "one";
    MaterialLease keep(cache, cache->Acquire(desc, {}));
    desc.shaderName = "two";
    MaterialLease(cache, cache->Acquire(desc, {})).Reset();
    REQUIRE(cache->Size() == 2);

    // the table is full: the unused record is released, but its slot is held until its frame completes
    desc.shaderName = "three";
    MaterialAcquire status = MaterialAcquire::Ready;
    CHECK(cache->Acquire(desc, {}, &status) == 0);
    CHECK(status == MaterialAcquire::Waiting);
    CHECK(cache->Size() == 1);
    CHECK(host.releasedMaterials.size() == 1);
    // still waiting on the next ask in the same frame, nothing more to release
    CHECK(cache->Acquire(desc, {}, &status) == 0);
    CHECK(status == MaterialAcquire::Waiting);

    // a later frame: the slot is back
    host.CompleteFrame();
    const uint64_t id = cache->Acquire(desc, {}, &status);
    CHECK(status == MaterialAcquire::Ready);
    CHECK(id != 0);
    CHECK_FALSE(keep.Get().IsNull());

    // with every slot claimed there is nothing to release, so it keeps waiting
    MaterialLease third(cache, id);
    desc.shaderName = "four";
    CHECK(cache->Acquire(desc, {}, &status) == 0);
    CHECK(status == MaterialAcquire::Waiting);
}

TEST_CASE("A material the renderer cannot make is a failure, not a wait", "[renderer][material-records]")
{
    test::FakeMeshResolveHost host;
    const auto cache = host.MaterialRecords();
    host.rejectMaterials = true;
    MaterialDesc desc;
    desc.shaderName = "broken";
    MaterialLease keepable(cache, cache->Acquire(MaterialDesc{.shaderName = "other"}, {}));
    MaterialAcquire status = MaterialAcquire::Ready;
    CHECK(cache->Acquire(desc, {}, &status) == 0);
    CHECK(status == MaterialAcquire::Failed);
    // nothing was released on its account
    CHECK(host.releasedMaterials.empty());
}

TEST_CASE("Record bytes in the shared table are counted and released oldest first over their category limit",
          "[renderer][material-records]")
{
    test::FakeMeshResolveHost host;
    const auto cache = host.MaterialRecords();
    std::vector<std::byte> bytes(40, std::byte{1});
    MaterialDesc desc;
    desc.shaderName = "surface";
    desc.record = bytes;
    desc.recordAlignment = 16;
    const uint64_t a = cache->Acquire(desc, {});
    bytes[0] = std::byte{2};
    desc.record = bytes;
    const uint64_t b = cache->Acquire(desc, {});
    MaterialLease(cache, a).Reset();
    MaterialLease keep(cache, b);
    // each record is 40 bytes rounded up to its 16 byte alignment
    CHECK(cache->RecordBytes() == 96);
    CHECK(host.Residency().Held(GpuCategory::Material) == 96);

    host.Residency().SetCategoryLimit(GpuCategory::Material, 50);
    cache->Trim();
    CHECK(cache->Size() == 1);
    CHECK(cache->RecordBytes() == 48);
    CHECK_FALSE(keep.Get().IsNull());
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

TEST_CASE("Releasing every claim at once keeps what fits the target and does not evict for retiring bytes",
          "[renderer][residency]")
{
    test::FakeMeshResolveHost host;
    const auto tracker = host.ModelMeshes(nullptr);
    // a triangle holds 3 * 64 + 3 * 4 = 204 bytes, five of them fit
    host.Residency().SetTotalTarget(5 * 204);
    std::vector<ModelMeshLease> leases;
    for (uint32_t part = 0; part < 10; ++part)
    {
        const AssetId id = tracker->Request(KeyOf(part), [] { return Triangle(); }, "mesh");
        leases.emplace_back(tracker, id);
    }
    tracker->Update();
    for (const ModelMeshLease &lease : leases)
        Finish(host, *tracker, lease.Get()->mesh);
    REQUIRE(tracker->HeldBytes() == 10 * 204);

    // a scene unload drops every claim in one frame, the retired memory stays in use until the frame completes
    leases.clear();
    CHECK(tracker->HeldBytes() == 5 * 204);
    CHECK(host.uploader.retired.size() == 5);
    // it is occupied until then, but already on its way out and not a reason to evict more
    CHECK(host.Residency().Retiring(GpuCategory::Mesh) == 5 * 204);
    host.CompleteFrame();
    CHECK(host.Residency().Retiring(GpuCategory::Mesh) == 0);
    CHECK(tracker->HeldBytes() == 5 * 204);
}

TEST_CASE("One total overage is freed once across texture and record bytes", "[renderer][material-records]")
{
    MaterialFixture f;
    const TextureHandle texture = f.ReadyTexture(kTexture);
    // the older record pins a texture, the newer holds 48 bytes of table and no texture
    const uint64_t textured = f.Material("a", texture, kTexture);
    std::vector<std::byte> bytes(40, std::byte{1});
    MaterialDesc recordDesc;
    recordDesc.shaderName = "surface";
    recordDesc.record = bytes;
    const uint64_t recorded = f.mCache->Acquire(recordDesc, {});
    MaterialLease(f.mCache, textured).Reset();
    MaterialLease(f.mCache, recorded).Reset();
    REQUIRE(f.mCache->Size() == 2);

    uint64_t other = 100;
    f.mHost.Residency().AddHolder(GpuCategory::Mesh, [&] { return other; });
    f.mHost.Residency().SetTotalTarget(f.mHost.Residency().UsedTotal() - 1);
    f.mCache->Trim();
    // the textured record alone covers a one byte overage
    CHECK(f.mCache->Size() == 1);
    CHECK(f.mCache->RecordBytes() == 48);
}

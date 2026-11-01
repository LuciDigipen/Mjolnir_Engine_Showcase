/**
 * @file TextureLoadTracker.tests.cpp
 * @author sumin.park
 * @brief Texture worker preparation, shared claims, failure and revision tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/TextureLoadTracker.h>

#include <assets/AssetsModule.h>
#include <assets/ModelAsset.h>
#include <core/fs/FileIO.h>
#include <jobs/JobExecutor.h>

#include "support/CookedMeshDir.h"
#include "support/FakeTextureUploader.h"

#include <catch2/catch_test_macros.hpp>

#include <format>

using namespace mir;

namespace
{
    const AssetId kTexture = MakeAssetId("texture");
    struct CookedTexture : test::CookedDir
    {
        std::string mFile;
        CookedTexture() { Write(); }
        void Write(uint8_t value = 80)
        {
            TextureAsset texture;
            texture.width = texture.height = 4;
            texture.rgba.assign(64, value);
            // content-addressed like a cook, so the catalog knows what the bytes are
            mFile = std::format("texture-{:016x}.blob", uint64_t(value));
            REQUIRE(WriteBytes(Path() / mFile, BuildTypedAsset(texture)));
            const AssetManifestSourceEntry entry{ { kTexture, kTextureAssetTypeTag, kTextureAssetContentVersion },
                mFile, "texture", {} };
            REQUIRE(WriteBytes(Path() / kAssetManifestFileName, BuildAssetManifestBlob(std::span(&entry, 1))));
        }
    };
    struct Fixture : CookedTexture
    {
        Fixture() : mAssets(Path()), mJobs(0), mService(mAssets, mJobs, 1), mCache(mUploader.TextureLoads(&mService)) {}
        void Frame() { mService.DrainCompletions(); mCache->Update(); }
        AssetsModule mAssets;
        JobExecutor mJobs;
        AsyncAssetService mService;
        test::FakeTextureUploader mUploader;
        std::shared_ptr<TextureLoadTracker> mCache;
    };
}

TEST_CASE("Texture consumers share worker preparation and one upload while staging owns the pixels",
    "[renderer][texture-load]")
{
    Fixture f;
    f.mCache->Request(kTexture, "texture");
    TextureLoadLease first(f.mCache, kTexture), second(f.mCache, kTexture);
    f.Frame();
    REQUIRE(second.Get());
    const auto handle = second.Get()->texture;
    REQUIRE_FALSE(handle.IsNull());
    CHECK(f.mUploader.mQueued == 1);
    CHECK(second.Get()->width == 4);
    CHECK(second.Get()->levels == 3);
    CHECK(f.mUploader.mEntries.at(handle.mIndex).mSource.Bytes() == 84);
    std::weak_ptr<const void> owner = f.mUploader.mEntries.at(handle.mIndex).mSource.keepalive;
    first.Reset();
    f.mService.EvictUnused();
    f.mUploader.Begin(handle);
    CHECK_FALSE(owner.expired());
    f.mUploader.CopyAll(handle);
    CHECK(owner.expired());
    CHECK(second.Get()->phase == AssetUploadPhase::Uploading);
    f.mUploader.Complete(handle);
    f.Frame();
    CHECK(second.Get()->phase == AssetUploadPhase::Ready);
    f.mCache->SetBudget(0);
    CHECK(f.mUploader.TextureState(handle) == TextureUploadState::Ready);
    second.Reset();
    CHECK(f.mUploader.TextureState(handle) == TextureUploadState::Stale);
    CHECK(f.mUploader.mRetired.size() == 1);
}

TEST_CASE("Abandoning a texture before publication queues no GPU work", "[renderer][texture-load]")
{
    Fixture f;
    f.mCache->Request(kTexture, "texture");
    TextureLoadLease claim(f.mCache, kTexture);
    claim.Reset();
    f.Frame();
    CHECK(f.mUploader.mQueued == 0);
    CHECK(f.mCache->Empty());
}

TEST_CASE("A failed texture upload retires exactly once", "[renderer][texture-load]")
{
    Fixture f;
    f.mCache->Request(kTexture, "texture");
    TextureLoadLease claim(f.mCache, kTexture);
    f.Frame();
    f.mUploader.Fail(claim.Get()->texture);
    f.Frame();
    CHECK(claim.Get()->phase == AssetUploadPhase::Failed);
    CHECK(claim.Get()->texture.IsNull());
    f.Frame();
    CHECK(f.mUploader.mRetired.size() == 1);
}

TEST_CASE("A new texture revision cannot invalidate a preview's old GPU claim", "[renderer][texture-load]")
{
    Fixture f;
    const UploadKey oldKey{ kTexture, UploadContent(&f.mService, kTexture) };
    f.mCache->Request(oldKey, "old texture");
    TextureLoadLease old(f.mCache, oldKey);
    f.Frame();
    const auto oldHandle = old.Get()->texture;
    f.mUploader.Begin(oldHandle);
    f.mUploader.CopyAll(oldHandle);
    f.mUploader.Complete(oldHandle);
    f.Frame();
    f.Write(120);
    f.mAssets.BeginReload();
    f.mAssets.CommitReload();
    // one cache serves every revision, the changed bytes are another key
    auto currentCache = f.mUploader.TextureLoads(&f.mService);
    REQUIRE(currentCache == f.mCache);
    const UploadKey newKey{ kTexture, UploadContent(&f.mService, kTexture) };
    REQUIRE(newKey != oldKey);
    currentCache->Request(newKey, "new texture");
    TextureLoadLease current(currentCache, newKey);
    f.mService.DrainCompletions();
    currentCache->Update();
    CHECK(current.Get()->texture != oldHandle);
    CHECK(old.Get()->texture == oldHandle);
    CHECK(f.mUploader.TextureState(oldHandle) == TextureUploadState::Ready);
    f.mUploader.ReleaseCachedTextures();
    CHECK(old.Get() == nullptr);
    CHECK(current.Get() == nullptr);
    CHECK(f.mUploader.mRetired.size() == 2);
}

TEST_CASE("A texture whose content is unchanged keeps its GPU record across a reload", "[renderer][texture-load]")
{
    Fixture f;
    const UploadKey key{ kTexture, UploadContent(&f.mService, kTexture) };
    f.mCache->Request(key, "texture");
    TextureLoadLease first(f.mCache, key);
    f.Frame();
    const auto handle = first.Get()->texture;
    f.mUploader.Begin(handle);
    f.mUploader.CopyAll(handle);
    f.mUploader.Complete(handle);
    f.Frame();
    first.Reset();
    // an identical cook, the unused record stays for the next consumer
    f.Write(80);
    f.mAssets.BeginReload();
    f.mAssets.CommitReload();
    const UploadKey again{ kTexture, UploadContent(&f.mService, kTexture) };
    REQUIRE(again == key);
    f.mCache->Request(again, "texture");
    TextureLoadLease second(f.mCache, again);
    f.Frame();
    CHECK(second.Get()->texture == handle);
    CHECK(second.Get()->phase == AssetUploadPhase::Ready);
    CHECK(f.mUploader.mQueued == 1);
}

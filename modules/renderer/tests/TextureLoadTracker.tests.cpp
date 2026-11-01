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

using namespace mir;

namespace
{
    const AssetId kTexture = MakeAssetId("texture");
    struct CookedTexture : test::CookedDir
    {
        CookedTexture() { Write(); }
        void Write(uint8_t value = 80)
        {
            TextureAsset texture;
            texture.width = texture.height = 4;
            texture.rgba.assign(64, value);
            REQUIRE(WriteBytes(Path() / "texture.blob", BuildTypedAsset(texture)));
            const AssetManifestSourceEntry entry{ { kTexture, kTextureAssetTypeTag, kTextureAssetContentVersion },
                "texture.blob", "texture", {} };
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
    f.mCache->Request(kTexture, "old texture");
    TextureLoadLease old(f.mCache, kTexture);
    f.Frame();
    const auto oldHandle = old.Get()->texture;
    f.mUploader.Begin(oldHandle);
    f.mUploader.CopyAll(oldHandle);
    f.mUploader.Complete(oldHandle);
    f.Frame();
    f.Write(120);
    f.mAssets.BeginReload();
    f.mAssets.CommitReload();
    auto currentCache = f.mUploader.TextureLoads(&f.mService);
    REQUIRE(currentCache != f.mCache);
    currentCache->Request(kTexture, "new texture");
    TextureLoadLease current(currentCache, kTexture);
    f.mService.DrainCompletions();
    currentCache->Update();
    CHECK(current.Get()->texture != oldHandle);
    CHECK(f.mUploader.TextureState(oldHandle) == TextureUploadState::Ready);
    f.mUploader.ReleaseCachedTextures();
    CHECK(old.Get() == nullptr);
    CHECK(current.Get() == nullptr);
    CHECK(f.mUploader.mRetired.size() == 2);
}

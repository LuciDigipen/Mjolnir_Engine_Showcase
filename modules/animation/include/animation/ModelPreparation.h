/**
 * @file ModelPreparation.h
 * @author sumin.park
 * @brief Loads model dependencies and waits for GPU uploads without blocking frames.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <animation/ModelAnimation.h>
#include <assets/AsyncAssetService.h>
#include <renderer/IMeshResolveHost.h>
#include <renderer/MaterialRecordCache.h>
#include <renderer/ModelMeshTracker.h>
#include <renderer/ITextureUploader.h>
#include <renderer/TextureLoadTracker.h>

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace mir
{
    struct PreparedPrimitive
    {
        MeshHandle mesh;
        MaterialHandle material, mirrored;
        glm::vec4 factor{ 1 };
        bool transparent = false;
    };

    // Read-only once ready. Member order releases meshes and materials before their textures.
    struct PreparedModel
    {
        std::shared_ptr<const ModelResources> resources;
        std::unordered_map<uint64_t, std::vector<PreparedPrimitive>> meshes;
        std::vector<TextureLoadLease> textures;
        std::vector<MaterialLease> materialClaims;
        std::vector<ModelMeshLease> meshClaims;
        bool degraded = false;
    };

    // Texture changes also require rebuilding the prepared model.
    std::span<const uint32_t> ModelIdentityTags();

    class ModelPreparation
    {
    public:
        enum class State : uint8_t { Loading, Records, Materials, Textures, Meshes, Ready, Failed, Superseded };

        // assets may be null, the preparation then fails.
        ModelPreparation(IMeshResolveHost &host, ITextureUploader &textures, TextureHandle errorTexture,
            AsyncAssetService *assets, AssetId id, std::string label);

        // Advances without waiting, call once a frame after the asset service drained its completions.
        State Advance();
        State GetState() const { return mState; }
        std::unique_ptr<PreparedModel> Take();
        const std::string &Error() const { return mError; }

        // how long material will wait to load
        void SetMaterialWait(std::chrono::milliseconds wait) { mMaterialWait = wait; }

    private:
        void Fail(std::string reason);
        // Stops preparation if a reload changes any required file.
        bool Current();
        uint64_t ContentOf(AssetId id) const;
        void RequestRecords();
        void RequestMaterials();
        void RequestTextures();
        bool TexturesSettled() const;
        void Build();
        void AwaitMeshes();

        template <class T> struct Claim
        {
            AssetId id;
            AssetLease<T> lease;
        };

        IMeshResolveHost &mHost;
        TextureHandle mErrorTexture;
        AssetId mId;
        std::string mLabel;
        std::shared_ptr<TextureLoadTracker> mTracker;
        std::shared_ptr<ModelMeshTracker> mMeshTracker;
        std::shared_ptr<MaterialRecordCache> mMaterials;
        AsyncAssetService *mService;
        // Original manifest and file identities, kept so reloads cannot mix old and new inputs.
        std::shared_ptr<const AssetManifest> mCatalog;
        uint32_t mRevision = 0;
        uint64_t mIdentity = 0;

        AssetLease<ModelAsset> mRoot;
        std::vector<Claim<MeshRecord>> mMeshClaims;
        std::vector<Claim<SkeletonAsset>> mSkeletonClaims;
        std::vector<Claim<AnimationClipAsset>> mClipClaims;
        std::vector<Claim<MaterialAsset>> mMaterialClaims;
        std::shared_ptr<ModelResources> mLoaded;

        std::unordered_map<uint64_t, TextureLoadLease> mTextureClaims;
        // Texture keys must use the original manifest, even if a reload happens before upload.
        std::unordered_map<uint64_t, UploadKey> mTextureKeys;
        // Hidden from callers until all required uploads finish.
        std::unique_ptr<PreparedModel> mPrepared;
        struct MeshSlot
        {
            uint64_t mesh = 0;
            size_t primitive = 0;
        };
        std::vector<MeshSlot> mMeshSlots;
        std::chrono::milliseconds mMaterialWait{ 5000 };
        std::chrono::steady_clock::time_point mWaitingSince;
        bool mWaitingForSlots = false;
        bool mGaveUpWaiting = false;
        // the build reports an unreadable material or an unloaded texture once, however many frames it takes
        bool mReported = false;
        std::string mError;
        State mState = State::Loading;
    };
}

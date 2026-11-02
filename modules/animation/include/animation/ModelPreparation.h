/**
 * @file ModelPreparation.h
 * @author sumin.park
 * @brief Shared immutable model resources, prepared in the background without blocking a frame.
 *
 * A preparation claims the model's record and then each record it names, the meshes, skeletons, clips
 * and materials, as shared decoded records of their own. It claims the textures, then claims shared GPU material
 * records and queues shared mesh uploads once every texture has settled, and publishes after the
 * meshes are ready. The renderer owns the GPU records, a PreparedModel only holds claims on them.
 * Playback, poses and draw extraction stay with the ModelSystem that owns the entities.
 *
 * A preparation belongs to the catalog it was requested under. Its inputs are named by that catalog's content,
 * and when a later catalog would give any of them other content the preparation stops as superseded rather
 * than mix two catalogs.
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

    // Never changes once published. The handles are valid while the claims below are held, and the claims
    // drop in reverse order, meshes and materials before the textures they sample.
    struct PreparedModel
    {
        std::shared_ptr<const ModelResources> resources;
        std::unordered_map<uint64_t, std::vector<PreparedPrimitive>> meshes;
        std::vector<TextureLoadLease> textures;
        std::vector<MaterialLease> materialClaims;
        std::vector<ModelMeshLease> meshClaims;
    };

    // The asset types whose content decides what a model is: its records and the textures its materials name.
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
        // The finished model, null unless the preparation is Ready.
        std::unique_ptr<PreparedModel> Take();
        const std::string &Error() const { return mError; }

    private:
        void Fail(std::string reason);
        // False, and Superseded, when a later catalog would give an input other content
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
        // What the model was requested under: the catalog, its revision and the content of everything it names
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
        // The upload record each texture asset names, its content decides whether an old record is reused.
        std::unordered_map<uint64_t, UploadKey> mTextureKeys;
        // Built once the textures settle, published when every mesh upload is ready.
        std::unique_ptr<PreparedModel> mPrepared;
        struct MeshSlot
        {
            uint64_t mesh = 0;
            size_t primitive = 0;
        };
        std::vector<MeshSlot> mMeshSlots;
        std::string mError;
        State mState = State::Loading;
    };
}

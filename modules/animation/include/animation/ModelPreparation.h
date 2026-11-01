/**
 * @file ModelPreparation.h
 * @author sumin.park
 * @brief Shared immutable model resources, prepared in the background without blocking a frame.
 *
 * A preparation claims a model's CPU records and its textures, then claims shared GPU material
 * records and queues shared mesh uploads once every texture has settled, and publishes after the
 * meshes are ready. The renderer owns the GPU records, a PreparedModel only holds claims on them.
 * Playback, poses and draw extraction stay with the ModelSystem that owns the entities.
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

    class ModelPreparation
    {
    public:
        enum class State : uint8_t { Loading, Textures, Meshes, Ready, Failed };

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
        void RequestTextures(const ModelResources &resources);
        bool TexturesSettled() const;
        void Build();
        void AwaitMeshes();

        IMeshResolveHost &mHost;
        TextureHandle mErrorTexture;
        AssetId mId;
        std::string mLabel;
        std::shared_ptr<TextureLoadTracker> mTracker;
        std::shared_ptr<ModelMeshTracker> mMeshTracker;
        std::shared_ptr<MaterialRecordCache> mMaterials;
        AsyncAssetService *mService;
        uint32_t mRevision = 0;
        AssetLease<ModelResources> mResources;
        std::shared_ptr<const ModelResources> mLoaded;
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

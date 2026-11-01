/**
 * @file ModelPreparation.h
 * @author sumin.park
 * @brief Shared immutable model resources, prepared in the background without blocking a frame.
 *
 * A preparation claims a model's CPU records and its textures, then builds the GPU meshes and
 * materials once every texture has settled. Playback, poses and draw extraction stay with the
 * ModelSystem that owns the entities.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <animation/ModelAnimation.h>
#include <assets/AsyncAssetService.h>
#include <renderer/IMeshResolveHost.h>
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

    // Never changes once published. The texture claims keep the textures alive for the materials.
    struct PreparedModel
    {
        std::shared_ptr<const ModelResources> resources;
        std::unordered_map<uint64_t, std::vector<PreparedPrimitive>> meshes;
        std::vector<TextureLoadLease> textures;
    };

    // Releases the model's meshes and materials, then its texture claims.
    void ReleasePreparedModel(IMeshResolveHost &host, std::unique_ptr<PreparedModel> model);

    class ModelPreparation
    {
    public:
        enum class State : uint8_t { Loading, Textures, Ready, Failed };

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

        IMeshResolveHost &mHost;
        TextureHandle mErrorTexture;
        AssetId mId;
        std::string mLabel;
        std::shared_ptr<TextureLoadTracker> mTracker;
        AssetLease<ModelResources> mResources;
        std::shared_ptr<const ModelResources> mLoaded;
        std::unordered_map<uint64_t, TextureLoadLease> mTextureClaims;
        std::unique_ptr<PreparedModel> mPrepared;
        std::string mError;
        State mState = State::Loading;
    };
}

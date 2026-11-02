/**
 * @file ModelLibrary.h
 * @author sumin.park
 * @brief Prepared models shared by every World that uses a renderer.
 *
 * A model is prepared once per decoded content, whoever asks. A World holds a claim on the prepared model
 * and keeps only playback, poses and deformations of its own. Dropping the last claim frees the prepared
 * model, and the renderer keeps its unused GPU records under the residency target.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <animation/ModelPreparation.h>
#include <renderer/RendererServices.h>

#include <map>
#include <tuple>
#include <memory>
#include <string>

namespace mir
{
    class ModelLibrary final : public IRendererService
    {
    public:
        // A model prepared for one content of one asset. Published whole or failed, never partly.
        struct Entry
        {
            AssetId asset;
            uint64_t content = 0;
            std::shared_ptr<const PreparedModel> ready;
            std::unique_ptr<ModelPreparation> pending;
            std::string failure;
        };
        using Claim = std::shared_ptr<Entry>;

        ModelLibrary(IMeshResolveHost &host, ITextureUploader &textures, TextureHandle errorTexture)
            : mHost(host), mTextures(textures), mErrorTexture(errorTexture)
        {}

        // The claim for this model's current content, preparation starts when nobody holds it yet. The same
        // content from another World is the same entry, changed content is another one.
        Claim Acquire(AssetId id, AsyncAssetService *assets, const std::string &label);

        // Moves every entry in flight one step. Call once a frame, more calls only cost time.
        void Advance();

        void ReleaseAll() override;

    private:
        struct Key
        {
            uint64_t asset = 0;
            uint64_t content = 0;
            const AsyncAssetService *service = nullptr;
            bool operator<(const Key &other) const
            {
                return std::tie(asset, content, service) < std::tie(other.asset, other.content, other.service);
            }
        };

        IMeshResolveHost &mHost;
        ITextureUploader &mTextures;
        TextureHandle mErrorTexture;
        // Worlds own the entries, the library only finds them
        std::map<Key, std::weak_ptr<Entry>> mEntries;
    };
}

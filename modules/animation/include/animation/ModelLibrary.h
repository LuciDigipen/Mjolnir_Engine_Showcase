/**
 * @file ModelLibrary.h
 * @author sumin.park
 * @brief Prepared models shared by scene and prefab Worlds.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <animation/ModelPreparation.h>

#include <map>
#include <tuple>
#include <memory>
#include <string>

namespace mir
{
    class ModelLibrary
    {
    public:
        // Ready models are complete, callers never receive a partially prepared model.
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

        // Worlds requesting the same asset and file contents share one entry.
        Claim Acquire(AssetId id, AsyncAssetService *assets, const std::string &label);

        void Advance();

        // Drops every entry's model, for a renderer about to destroy the resources they name.
        void ReleaseAll();

        size_t LiveEntries() const;

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

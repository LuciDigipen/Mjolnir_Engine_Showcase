/**
 * @file UITextures.h
 * @author Quy Dong Ta
 * @brief The game UI's textures: cooked texture assets copied into textures
 *        the backend uploads itself, handed out as sprites.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UITypes.h"

#include <assets/AssetId.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mir
{
    class AssetCache;
    class IUIBackend;
    struct TextureAsset;
    struct UIBackendTexture;

    class UITextures
    {
    public:
        UITextures();
        ~UITextures();

        UITextures(const UITextures &) = delete;
        UITextures &operator=(const UITextures &) = delete;
        UITextures(UITextures &&) = delete;
        UITextures &operator=(UITextures &&) = delete;


        UISprite Load(AssetCache &assets, AssetId id);

        UISprite Add(const TextureAsset &texture);

        size_t Count() const { return mTextures.size(); }

        // the backend's texture, or null: no backend, or it could not make one
        void *Find(UITexture texture) const;

        // makes every texture with the backend, from the pixels kept here. Null stops
        // making new copies; the copies made stay until the textures go, since they
        // release themselves
        void SetBackend(IUIBackend *backend);

    private:
        struct Texture
        {
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<uint8_t> rgba;
            void *backend = nullptr;
            void (*release)(void *) = nullptr;
        };

        void Make(Texture &texture);

        // [id - 1]
        std::vector<Texture> mTextures;
        IUIBackend *mBackend = nullptr;

        // by AssetId, failed loads included, so a missing asset is reported once
        std::unordered_map<uint64_t, UISprite> mLoaded;
    };
} // namespace mir

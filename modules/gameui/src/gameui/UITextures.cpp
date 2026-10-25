/**
 * @file UITextures.cpp
 * @author Quy Dong Ta
 * @brief The game UI's textures: cooked texture assets copied into textures
 *        the backend uploads itself, handed out as sprites.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UITextures.h>

#include <gameui/UIBackend.h>

#include <assets/AssetCache.h>
#include <assets/ModelAsset.h>
#include <core/log/Log.h>

#include <string>

namespace mir
{
    UITextures::UITextures() = default;

    UITextures::~UITextures()
    {
        for (Texture &texture : mTextures)
        {
            if (texture.release != nullptr)
                texture.release(texture.backend);
        }
    }

    UISprite UITextures::Load(AssetCache &assets, AssetId id)
    {
        if (const auto loaded = mLoaded.find(id.value); loaded != mLoaded.end())
            return loaded->second;

        UISprite sprite;
        TextureAsset texture;
        std::string error;
        if (!LoadTypedAsset(assets, id, texture, error))
            MIR_LOG_ERROR("gameui: texture {:016x}: {}", id.value, error);
        else
            sprite = Add(texture);

        mLoaded.emplace(id.value, sprite);
        return sprite;
    }

    UISprite UITextures::Add(const TextureAsset &texture)
    {
        if (texture.width == 0 || texture.height == 0 ||
            texture.rgba.size() != static_cast<size_t>(texture.width) * texture.height * 4)
        {
            MIR_LOG_ERROR("gameui: a {}x{} UI texture needs {} bytes of RGBA, got {}", texture.width, texture.height,
                static_cast<size_t>(texture.width) * texture.height * 4, texture.rgba.size());
            return {};
        }

        Make(mTextures.emplace_back(Texture{ texture.width, texture.height, texture.rgba }));
        const float width = static_cast<float>(texture.width);
        const float height = static_cast<float>(texture.height);
        return UISprite{.mTexture = UITexture{static_cast<uint32_t>(mTextures.size())},
                        .mTextureWidth = width,
                        .mTextureHeight = height,
                        .mSource = UIRect{0.0f, 0.0f, width, height}};
    }

    void *UITextures::Find(UITexture texture) const
    {
        if (!texture.IsValid() || texture.mId > mTextures.size())
            return nullptr;
        return mTextures[texture.mId - 1].backend;
    }

    void UITextures::SetBackend(IUIBackend *backend)
    {
        mBackend = backend;
        for (Texture &texture : mTextures)
            Make(texture);
    }

    void UITextures::Make(Texture &texture)
    {
        if (mBackend == nullptr || texture.backend != nullptr)
            return;

        const UIBackendTexture made = mBackend->CreateTexture(texture.width, texture.height, texture.rgba);
        texture.backend = made.texture;
        texture.release = made.release;
    }
} // namespace mir

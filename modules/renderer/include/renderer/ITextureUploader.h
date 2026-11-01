/**
 * @file ITextureUploader.h
 * @author sumin.park
 * @brief The asynchronous texture transfer and retirement boundary.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/TextureUpload.h>
#include <renderer/AssetUploadCache.h>

namespace mir
{
    class TextureLoadTracker;
    class ITextureUploader
    {
    public:
        virtual ~ITextureUploader() = default;
        virtual TextureHandle QueueTexture(TextureUploadSource source) = 0;
        virtual TextureUploadState TextureState(TextureHandle handle) const = 0;
        virtual bool RetireTexture(TextureHandle handle) = 0;
        virtual uint64_t TextureFootprint(TextureHandle handle) const = 0;
        std::shared_ptr<TextureLoadTracker> TextureLoads(AsyncAssetService *assets);
        void ReleaseCachedTextures();
    private:
        AssetUploadRegistry<TextureLoadTracker> mTextureCaches;
    };
}

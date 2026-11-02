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
#include <renderer/GpuResidency.h>

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
        // Texture memory released but still in use by a frame in flight, it counts against the residency target.
        virtual uint64_t TextureRetiringBytes() const { return 0; }
        std::shared_ptr<TextureLoadTracker> TextureLoads(AsyncAssetService *assets);
        void ReleaseCachedTextures();
        // The residency target the uploader's caches share, one per renderer.
        virtual std::shared_ptr<GpuResidency> SharedResidency() { return mResidency; }
        GpuResidency &Residency() { return *SharedResidency(); }
    private:
        std::shared_ptr<GpuResidency> mResidency = std::make_shared<GpuResidency>();
        AssetUploadRegistry<TextureLoadTracker> mTextureCaches;
    };
}

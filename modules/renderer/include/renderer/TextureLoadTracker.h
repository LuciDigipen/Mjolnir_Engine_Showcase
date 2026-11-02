/**
 * @file TextureLoadTracker.h
 * @author sumin.park
 * @brief Texture-specific decode and upload behavior on the shared resource lifecycle.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/AssetUploadCache.h>
#include <renderer/TextureUpload.h>

namespace mir
{
    class ITextureUploader;
    struct TextureLoad
    {
        AssetUploadPhase phase = AssetUploadPhase::Decoding;
        TextureHandle texture;
        uint32_t width = 0, height = 0, levels = 0;
    };
    class TextureLoadTracker : public AssetUploadCache<TextureUploadSource, TextureHandle, TextureLoad, &TextureLoad::texture>
    {
        using Base = AssetUploadCache<TextureUploadSource, TextureHandle, TextureLoad, &TextureLoad::texture>;
    public:
        TextureLoadTracker(ITextureUploader &uploader, AsyncAssetService *assets);
    };
    using TextureLoadLease = AssetUploadLease<TextureLoadTracker>;
}

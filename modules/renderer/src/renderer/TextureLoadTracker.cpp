/**
 * @file TextureLoadTracker.cpp
 * @author sumin.park
 * @brief Worker texture preparation and the shared texture upload cache adapter.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/TextureLoadTracker.h"

#include <assets/ModelAsset.h>
#include <renderer/ITextureUploader.h>

namespace mir 
{
    namespace
    {
        bool DecodeTexture(const AssetBlobView &root, const AssetDependencies &, TextureUploadSource &out, std::string &error)
        {
            TextureAsset texture;
            if (!ReadTypedAsset(root, texture, error))
                return false;
            TextureDesc desc{ .width = texture.width, .height = texture.height,
                .pixels = std::as_bytes(std::span(texture.rgba)), .srgb = texture.format == TextureFormat::Rgba8Srgb,
                .generateMips = texture.mips.empty() };
            for (const auto &mip : texture.mips)
                desc.mips.push_back({ mip.width, mip.height,
                    std::as_bytes(std::span(texture.mipData).subspan(mip.offset, mip.length)) });
            out = PrepareTextureUpload(desc);
            if (out.desc.width != 0)
                return true;
            error = "invalid texture dimensions or mip chain";
            return false;
        }
        uint64_t TextureBytes(const TextureUploadSource &source) { return sizeof(source) + source.Bytes(); }
        const AssetDecoder<TextureUploadSource> kTextureDecoder{ DecodeTexture, {}, TextureBytes };
    }

    TextureLoadTracker::TextureLoadTracker(ITextureUploader &uploader, AsyncAssetService *assets)
        : Base(assets, kTextureDecoder,
            [&uploader](UploadKey, std::shared_ptr<const TextureUploadSource> source)
                { return uploader.QueueTexture({ source->desc, source }); },
            [&uploader](TextureHandle handle) { return uploader.TextureState(handle); },
            [&uploader](TextureHandle handle) { uploader.RetireTexture(handle); },
            [&uploader](UploadKey, TextureHandle handle, const TextureUploadSource &) { return uploader.TextureFootprint(handle); },
            [](TextureLoad &view, const TextureUploadSource &source)
                {
                    view.width = source.desc.width;
                    view.height = source.desc.height;
                    view.levels = static_cast<uint32_t>(source.desc.mips.size()) + 1;
                },
            [&uploader] { return uploader.TextureRetiringBytes(); }, uploader.SharedResidency(), GpuCategory::Texture,
            [&uploader] { return uploader.EnsureTextureSlot(); })
    {}

    std::shared_ptr<TextureLoadTracker> ITextureUploader::TextureLoads(AsyncAssetService *assets)
    {
        return mTextureCaches.Get(*this, assets, true);
    }

    void ITextureUploader::ReleaseCachedTextures()
    {
        mTextureCaches.Release([this](TextureHandle handle) { RetireTexture(handle); });
    }
}

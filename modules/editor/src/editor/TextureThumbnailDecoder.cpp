/**
 * @file TextureThumbnailDecoder.cpp
 * @author sumin.park
 * @brief Worker-side cooked texture thumbnails.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "editor/TextureThumbnailDecoder.h"
#include "editor/ImageDecode.h"

#include <assets/ModelAsset.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace mir
{
    namespace
    {
        TextureUploadSource Filter(uint32_t width, uint32_t height, std::span<const uint8_t> source, bool srgb)
        {
            const uint32_t longest = std::max(width, height);
            const uint32_t targetWidth = longest <= kThumbnailSize ? width :
                std::max(1u, uint32_t(uint64_t(width) * kThumbnailSize / longest));
            const uint32_t targetHeight = longest <= kThumbnailSize ? height :
                std::max(1u, uint32_t(uint64_t(height) * kThumbnailSize / longest));
            auto pixels = std::make_shared<std::vector<std::byte>>(size_t(targetWidth) * targetHeight * 4);
            const auto linear = [srgb](float v) { return !srgb ? v :
                (v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f)); };
            const auto encoded = [srgb](float v) { return !srgb ? v :
                (v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f); };
            for (uint32_t y = 0; y < targetHeight; ++y)
                for (uint32_t x = 0; x < targetWidth; ++x)
                {
                    float rgb[3]{}, alpha = 0;
                    uint32_t count = 0;
                    for (uint32_t sy = uint64_t(y) * height / targetHeight;
                         sy < uint64_t(y + 1) * height / targetHeight; ++sy)
                        for (uint32_t sx = uint64_t(x) * width / targetWidth;
                             sx < uint64_t(x + 1) * width / targetWidth; ++sx)
                        {
                            const size_t offset = (size_t(sy) * width + sx) * 4;
                            const float a = source[offset + 3] / 255.0f;
                            for (uint32_t c = 0; c < 3; ++c)
                                rgb[c] += linear(source[offset + c] / 255.0f) * a;
                            alpha += a;
                            ++count;
                        }
                    for (uint32_t c = 0; c < 4; ++c)
                    {
                        const float value = c == 3 ? alpha / count : encoded(alpha > 0 ? rgb[c] / alpha : 0);
                        (*pixels)[(size_t(y) * targetWidth + x) * 4 + c] =
                            static_cast<std::byte>(std::clamp<long>(std::lround(value * 255), 0, 255));
                    }
                }
            return { { .width = targetWidth, .height = targetHeight, .pixels = *pixels, .srgb = srgb }, pixels };
        }
        bool Decode(const AssetBlobView &root, const AssetDependencies &, TextureUploadSource &out, std::string &error)
        {
            TextureAsset texture;
            if (!ReadTypedAsset(root, texture, error))
                return false;
            uint32_t width = texture.width, height = texture.height, level = 0;
            // Keep the smallest authored level still large enough for the preview.
            for (uint32_t i = 0; i < texture.mips.size(); ++i)
            {
                const auto &mip = texture.mips[i];
                if (std::max(mip.width, mip.height) < kThumbnailSize)
                    break;
                width = mip.width;
                height = mip.height;
                level = i + 1;
            }
            const auto source = TextureMipPixels(texture, level);
            out = Filter(width, height, source, texture.format == TextureFormat::Rgba8Srgb);
            return true;
        }
        uint64_t Bytes(const TextureUploadSource &source) { return sizeof(source) + source.Bytes(); }
    }

    TextureUploadSource LoadSourceThumbnail(const std::filesystem::path &path, std::string &error) noexcept
    {
        try
        {
            DecodedImage image;
            if (!ReadImage(path, 16384, image, error))
                return {};
            return Filter(image.mWidth, image.mHeight,
                { image.mPixels.get(), size_t(image.mWidth) * image.mHeight * 4 }, true);
        }
        catch (const std::bad_alloc &) { std::terminate(); }
        catch (const std::exception &exception) { error = exception.what(); return {}; }
    }

    const AssetDecoder<TextureUploadSource> &TextureThumbnailDecoder()
    {
        static const AssetDecoder<TextureUploadSource> decoder{ Decode, {}, Bytes };
        return decoder;
    }
}

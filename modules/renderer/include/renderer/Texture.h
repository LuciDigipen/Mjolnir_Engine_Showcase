/**
 * @file Texture.h
 * @author Rahul Nair
 * @brief Texture handle and the CPU-side description that uploads one.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mir
{
    struct TextureHandle
    {
        static constexpr uint32_t kNullIndex = UINT32_MAX;

        uint32_t mIndex = kNullIndex;
        uint32_t mGeneration = 0;

        constexpr bool IsNull() const { return mIndex == kNullIndex; }
        constexpr bool operator==(const TextureHandle &) const = default;
    };

    inline constexpr TextureHandle kNullTexture{};

    // One level above 0, already filtered by the producer.
    struct TextureMipView
    {
        uint32_t width = 0;
        uint32_t height = 0;
        // width * height * 4 bytes, row-major.
        std::span<const std::byte> pixels;
    };

    // CPU-side pixels for CreateTexture.
    struct TextureDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;

        // width * height * 4 bytes, row-major starting at the top-left texel.
        std::span<const std::byte> pixels;

        // Base color textures are authored in sRGB.
        bool srgb = true;

        // Build the whole mip chain on the CPU (box filter in linear light)
        // instead of uploading level 0 alone. Off by default: a material only
        // samples the chain when its SamplerDesc asks for mips.
        bool generateMips = false;

        // Levels 1..N supplied by the producer. When non-empty they are
        // uploaded as-is and generateMips is ignored.
        std::vector<TextureMipView> mips;
    };

    inline bool TextureMipLayoutValid(const TextureDesc &desc)
    {
        if (desc.width == 0 || desc.height == 0 || desc.pixels.size_bytes() != size_t(desc.width) * desc.height * 4)
            return false;
        uint32_t width = desc.width, height = desc.height, level = 0;
        while (width > 1 || height > 1)
        {
            width = width > 1 ? width / 2 : 1;
            height = height > 1 ? height / 2 : 1;
            if (level >= desc.mips.size())
                return false;
            const TextureMipView &mip = desc.mips[level++];
            if (mip.width != width || mip.height != height ||
                mip.pixels.size_bytes() != size_t(width) * height * 4)
                return false;
        }
        return level == desc.mips.size();
    }
}

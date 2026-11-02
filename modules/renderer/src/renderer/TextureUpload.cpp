/**
 * @file TextureUpload.cpp
 * @author sumin.park
 * @brief Shared linear-light mip preparation for synchronous and asynchronous texture uploads.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/TextureUpload.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace mir
{
    uint64_t TextureUploadSource::Bytes() const
    {
        uint64_t bytes = desc.pixels.size_bytes();
        for (const auto &mip : desc.mips)
            bytes += mip.pixels.size_bytes();
        return bytes;
    }

    TextureUploadSource PrepareTextureUpload(const TextureDesc &desc)
    {
        const uint64_t texels = uint64_t(desc.width) * desc.height;
        if (desc.width == 0 || desc.height == 0 || texels > SIZE_MAX / 4 ||
            desc.pixels.size_bytes() != texels * 4 || (!desc.mips.empty() && !TextureMipLayoutValid(desc)))
            return {};
        struct Payload { std::vector<std::vector<std::byte>> levels; };
        auto payload = std::make_shared<Payload>();
        payload->levels.emplace_back(desc.pixels.begin(), desc.pixels.end());
        if (!desc.mips.empty())
            for (const auto &mip : desc.mips)
                payload->levels.emplace_back(mip.pixels.begin(), mip.pixels.end());
        else if (desc.generateMips)
        {
            const auto toLinear = [](float v)
                { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
            const auto toSrgb = [](float v)
                { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; };
            uint32_t width = desc.width, height = desc.height;
            while (width > 1 || height > 1)
            {
                const auto &level = payload->levels.back();
                const uint32_t nextWidth = std::max(width / 2, 1u), nextHeight = std::max(height / 2, 1u);
                std::vector<std::byte> next(size_t(nextWidth) * nextHeight * 4);
                for (uint32_t y = 0; y < nextHeight; ++y)
                    for (uint32_t x = 0; x < nextWidth; ++x)
                        for (uint32_t c = 0; c < 4; ++c)
                        {
                            float sum = 0;
                            uint32_t samples = 0;
                            for (uint32_t sy = y * height / nextHeight; sy < (y + 1) * height / nextHeight; ++sy)
                                for (uint32_t sx = x * width / nextWidth; sx < (x + 1) * width / nextWidth; ++sx)
                                {
                                    const float v = std::to_integer<uint8_t>(level[(size_t(sy) * width + sx) * 4 + c]) / 255.0f;
                                    sum += c < 3 && desc.srgb ? toLinear(v) : v;
                                    ++samples;
                                }
                            float v = sum / samples;
                            if (c < 3 && desc.srgb)
                                v = toSrgb(v);
                            next[(size_t(y) * nextWidth + x) * 4 + c] =
                                static_cast<std::byte>(std::clamp<long>(std::lround(v * 255), 0, 255));
                        }
                payload->levels.push_back(std::move(next));
                width = nextWidth;
                height = nextHeight;
            }
        }
        TextureDesc prepared{ .width = desc.width, .height = desc.height, .pixels = payload->levels[0],
            .srgb = desc.srgb, .generateMips = false };
        uint32_t width = desc.width, height = desc.height;
        for (size_t i = 1; i < payload->levels.size(); ++i)
        {
            width = std::max(width / 2, 1u);
            height = std::max(height / 2, 1u);
            prepared.mips.push_back({ width, height, payload->levels[i] });
        }
        return { std::move(prepared), std::move(payload) };
    }
}

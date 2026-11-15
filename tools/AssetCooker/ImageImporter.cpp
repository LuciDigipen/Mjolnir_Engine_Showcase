/**
 * @file ImageImporter.cpp
 * @author Quy Dong Ta
 * @brief PNG and JPEG decoding to the RGBA8 texture payload, shared by the
 *        glTF importer and the cooker's standalone images.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ImageImporter.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <memory>
#include <stb_image.h>
#include <stdexcept>
#include <string>

namespace mir::cook
{
    void BuildNormalMipChain(TextureAsset &texture)
    {
        texture.mips.clear();
        texture.mipData.clear();
        texture.format = TextureFormat::Rgba8Unorm;
        texture.colorSpace = TextureColorSpace::Linear;
        auto normalize = [](std::array<float, 3> n) {
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            return length > 1e-5f ? std::array<float, 3>{n[0] / length, n[1] / length, n[2] / length}
                                  : std::array<float, 3>{0, 0, 1};
        };
        auto encode = [&](uint8_t *p, std::array<float, 3> n) {
            n = normalize(n);
            for (int k = 0; k < 3; ++k)
                p[k] = uint8_t(std::clamp(std::lround((n[k] * .5f + .5f) * 255), 0l, 255l));
            p[3] = 255;
        };
        for (size_t i = 0; i < texture.rgba.size(); i += 4)
            encode(texture.rgba.data() + i,
                   {texture.rgba[i] / 127.5f - 1, texture.rgba[i + 1] / 127.5f - 1, texture.rgba[i + 2] / 127.5f - 1});
        auto level = texture.rgba;
        uint32_t width = texture.width, height = texture.height;
        while (width > 1 || height > 1)
        {
            const uint32_t w = std::max(width / 2, 1u), h = std::max(height / 2, 1u);
            std::vector<uint8_t> next(size_t(w) * h * 4);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    std::array<float, 3> sum{};
                    for (uint32_t sy = y * height / h; sy < (y + 1) * height / h; ++sy)
                        for (uint32_t sx = x * width / w; sx < (x + 1) * width / w; ++sx)
                            for (int k = 0; k < 3; ++k)
                                sum[k] += level[(size_t(sy) * width + sx) * 4 + k] / 127.5f - 1;
                    encode(next.data() + (size_t(y) * w + x) * 4, sum);
                }
            texture.mips.push_back({w, h, uint32_t(texture.mipData.size()), uint32_t(next.size())});
            texture.mipData.insert(texture.mipData.end(), next.begin(), next.end());
            level = std::move(next);
            width = w;
            height = h;
        }
    }
    namespace
    {
        float ToLinear(float v)
        {
            return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        }
        float ToSrgb(float v)
        {
            return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        }
    } // namespace

    TextureAsset DecodeImage(std::span<const std::byte> bytes, TextureColorSpace colorSpace, bool generateMips)
    {
        if (bytes.size() > INT_MAX)
            throw std::runtime_error("encoded image too large");
        const auto *data = reinterpret_cast<const stbi_uc *>(bytes.data());
        const int size = static_cast<int>(bytes.size());
        int w = 0, h = 0, n = 0;
        if (stbi_info_from_memory(data, size, &w, &h, &n) == 0 || w <= 0 || h <= 0 ||
            uint64_t(w) * h > 64 * 1024 * 1024)
            throw std::runtime_error("invalid/oversized PNG or JPEG");
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load_from_memory(data, size, &w, &h, &n, 4),
                                                                    stbi_image_free);
        if (pixels == nullptr)
            throw std::runtime_error("image decode failed");
        TextureAsset texture;
        texture.width = static_cast<uint32_t>(w);
        texture.height = static_cast<uint32_t>(h);
        texture.colorSpace = colorSpace;
        texture.format = colorSpace == TextureColorSpace::Srgb ? TextureFormat::Rgba8Srgb : TextureFormat::Rgba8Unorm;
        texture.rgba.assign(pixels.get(), pixels.get() + size_t(w) * h * 4);
        if (generateMips)
            BuildTextureMipChain(texture);
        return texture;
    }

    void BuildTextureMipChain(TextureAsset &texture)
    {
        texture.mips.clear();
        texture.mipData.clear();
        const bool srgb = texture.colorSpace == TextureColorSpace::Srgb;
        std::vector<uint8_t> level = texture.rgba;
        uint32_t width = texture.width, height = texture.height;
        while (width > 1 || height > 1)
        {
            const uint32_t nextWidth = std::max(width / 2, 1u);
            const uint32_t nextHeight = std::max(height / 2, 1u);
            std::vector<uint8_t> next(static_cast<size_t>(nextWidth) * nextHeight * 4);
            for (uint32_t y = 0; y < nextHeight; ++y)
                for (uint32_t x = 0; x < nextWidth; ++x)
                    for (uint32_t c = 0; c < 4; ++c)
                    {
                        float sum = 0;
                        uint32_t samples = 0;
                        for (uint32_t sy = y * height / nextHeight; sy < (y + 1) * height / nextHeight; ++sy)
                            for (uint32_t sx = x * width / nextWidth; sx < (x + 1) * width / nextWidth; ++sx)
                            {
                                const float v = level[(static_cast<size_t>(sy) * width + sx) * 4 + c] / 255.0f;
                                sum += c < 3 && srgb ? ToLinear(v) : v;
                                ++samples;
                            }
                        float v = sum / static_cast<float>(samples);
                        if (c < 3 && srgb)
                            v = ToSrgb(v);
                        next[(static_cast<size_t>(y) * nextWidth + x) * 4 + c] =
                            static_cast<uint8_t>(std::clamp<long>(std::lround(v * 255.0f), 0, 255));
                    }
            TextureMip mip;
            mip.width = nextWidth;
            mip.height = nextHeight;
            mip.offset = static_cast<uint32_t>(texture.mipData.size());
            mip.length = static_cast<uint32_t>(next.size());
            texture.mips.push_back(mip);
            texture.mipData.insert(texture.mipData.end(), next.begin(), next.end());
            level = std::move(next);
            width = nextWidth;
            height = nextHeight;
        }
    }

    bool IsImagePath(const std::filesystem::path &path)
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg";
    }
} // namespace mir::cook

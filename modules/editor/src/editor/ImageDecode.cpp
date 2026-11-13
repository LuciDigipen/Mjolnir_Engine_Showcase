/**
 * @file ImageDecode.cpp
 * @author sumin.park
 * @brief Shared CPU image decoding for editor backgrounds and source previews.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "editor/ImageDecode.h"

#include <core/fs/FileIO.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <climits>

namespace mir
{
    void DecodedImage::Free::operator()(uint8_t *pixels) const { stbi_image_free(pixels); }

    bool ReadImage(const std::filesystem::path &path, int maxSide, DecodedImage &out, std::string &error)
    {
        const auto bytes = ReadBytes(path);
        if (!bytes || bytes->empty() || bytes->size() > static_cast<size_t>(INT_MAX))
        {
            error = "cannot read the file";
            return false;
        }
        const auto *data = reinterpret_cast<const stbi_uc *>(bytes->data());
        const int size = static_cast<int>(bytes->size());
        int width = 0, height = 0, channels = 0;
        if (!stbi_info_from_memory(data, size, &width, &height, &channels) || width <= 0 || height <= 0)
        {
            error = std::string("not a supported image (") + stbi_failure_reason() + ")";
            return false;
        }
        if (width > maxSide || height > maxSide)
        {
            error = std::to_string(width) + "x" + std::to_string(height) + " is over " +
                std::to_string(maxSide) + " px per side";
            return false;
        }
        out.mPixels.reset(stbi_load_from_memory(data, size, &width, &height, &channels, 4));
        if (!out.mPixels)
        {
            error = std::string("cannot decode the image (") + stbi_failure_reason() + ")";
            return false;
        }
        out.mWidth = width;
        out.mHeight = height;
        return true;
    }
}

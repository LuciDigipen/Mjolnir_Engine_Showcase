/**
 * @file ImageDecode.h
 * @author sumin.park
 * @brief Shared CPU image decoding for editor backgrounds and source previews.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace mir
{
    struct DecodedImage
    {
        struct Free { void operator()(uint8_t *pixels) const; };
        std::unique_ptr<uint8_t, Free> mPixels;
        int mWidth = 0, mHeight = 0;
    };

    // Owns only CPU pixels, safe to call from an editor worker.
    bool ReadImage(const std::filesystem::path &path, int maxSide, DecodedImage &out, std::string &error);
}

/**
 * @file TextureThumbnailDecoder.h
 * @author sumin.park
 * @brief Worker-side cooked texture thumbnails.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetDecoder.h>
#include <renderer/TextureUpload.h>

#include <filesystem>

namespace mir
{
    inline constexpr uint32_t kThumbnailSize = 128;
    inline constexpr uint32_t kThumbnailGeneratorVersion = 1;
    const AssetDecoder<TextureUploadSource> &TextureThumbnailDecoder();
    // Material source images may have only packed derived outputs in the catalog.
    TextureUploadSource LoadSourceThumbnail(const std::filesystem::path &path, std::string &error) noexcept;
}

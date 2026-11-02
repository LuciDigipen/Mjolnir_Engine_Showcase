/**
 * @file TextureUpload.h
 * @author sumin.park
 * @brief Owned texture upload sources and worker-safe mip preparation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Texture.h>

#include <memory>

namespace mir
{
    struct TextureUploadSource
    {
        TextureDesc desc;
        std::shared_ptr<const void> keepalive;
        uint64_t Bytes() const;
    };

    // Copies pixels and prepares missing mips in linear light, run on a worker for large textures.
    // Invalid input returns an empty source, supplied mip chains are preserved byte for byte.
    TextureUploadSource PrepareTextureUpload(const TextureDesc &desc);
}

/**
 * @file Material.h
 * @author Sumin Park
 * @brief Material handle and the description that builds one.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Texture.h>

#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace mir
{
    // kNullMaterial = default material
    struct MaterialHandle
    {
        static constexpr uint32_t kNullIndex = UINT32_MAX;

        uint32_t mIndex = kNullIndex;
        uint32_t mGeneration = 0;

        constexpr bool IsNull() const { return mIndex == kNullIndex; }
        constexpr bool operator==(const MaterialHandle &) const = default;
    };

    inline constexpr MaterialHandle kNullMaterial{};

    struct SamplerDesc
    {
        VkFilter magFilter = VK_FILTER_LINEAR;
        VkFilter minFilter = VK_FILTER_LINEAR;
        VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        VkSamplerAddressMode addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VkSamplerAddressMode addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;

        bool useMips = false;

        // 1 keeps isotropic filtering. Above 1 requests anisotropy up to the device limit.
        float maxAnisotropy = 1.0f;

        constexpr bool operator==(const SamplerDesc &) const = default;
    };

    // The anisotropy the sampler actually requests: 1 without device support.
    float EffectiveAnisotropy(const SamplerDesc &desc, bool supported, float deviceLimit);
    // Identifies the effective sampler state, including the clamped anisotropy.
    uint64_t SamplerCacheKey(const SamplerDesc &desc, bool supported, float deviceLimit);

    enum class MaterialAlphaMode : uint32_t
    {
        Opaque,
        Mask,
        Blend
    };

    struct MaterialDesc
    {
        std::string shaderName = "default";

        VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
        VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        // A null handle falls back to the renderer's default white / flat normal.
        TextureHandle baseColorTexture;
        TextureHandle normalTexture;

        SamplerDesc sampler;

        MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
        float alphaCutoff = 0.5f;

        // Raw bytes of this material's record. The layout belongs to the material
        // type's reflection; the renderer only copies and aligns it.
        std::span<const std::byte> record;
        uint32_t recordAlignment = 16;

        // True for a SurfaceForward/SurfaceRt material: it reads the record
        // through the shared table and gets its own RT hit group.
        bool surfaceMaterial = false;
        // Normalized repo-relative .material.json path; empty selects the first
        // registration with shaderName.
        std::string surfaceAsset;
    };
}

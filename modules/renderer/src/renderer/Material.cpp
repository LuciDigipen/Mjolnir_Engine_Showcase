/**
 * @file Material.cpp
 * @author Sumin Park
 * @brief Effective sampler state and its cache key.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/Material.h>

#include <algorithm>
#include <cmath>

namespace mir
{
    float EffectiveAnisotropy(const SamplerDesc &desc, bool supported, float deviceLimit)
    {
        if (!supported || desc.maxAnisotropy <= 1.0f || deviceLimit <= 1.0f)
            return 1.0f;

        return std::clamp(desc.maxAnisotropy, 2.0f, deviceLimit);
    }

    uint64_t SamplerCacheKey(const SamplerDesc &desc, bool supported, float deviceLimit)
    {
        const float anisotropy = EffectiveAnisotropy(desc, supported, deviceLimit);
        const uint64_t anisotropyLevel =
            anisotropy > 1.0f ? static_cast<uint64_t>(std::lround(anisotropy)) : 0u;

        return (static_cast<uint64_t>(desc.magFilter) << 0) |
            (static_cast<uint64_t>(desc.minFilter) << 3) |
            (static_cast<uint64_t>(desc.mipmapMode) << 6) |
            (static_cast<uint64_t>(desc.addressModeU) << 9) |
            (static_cast<uint64_t>(desc.addressModeV) << 12) |
            (desc.useMips ? 1ull << 15 : 0ull) |
            (anisotropyLevel << 16);
    }
}

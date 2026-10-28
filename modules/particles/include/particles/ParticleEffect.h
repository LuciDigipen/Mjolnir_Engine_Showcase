/**
 * @file ParticleEffect.h
 * @author sumin.park
 * @brief Versioned effect definitions and cooked texture dependencies.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetBlob.h>
#include <assets/AssetId.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    class AssetCache;

    inline constexpr uint32_t kParticleEffectTypeTag = 0x54524150u;
    inline constexpr uint32_t kParticleEffectContentVersion = 1u;
    inline constexpr uint32_t kMaxEffectParticles = 65536;
    inline constexpr float kMaxEffectLifetime = 120.0f;

    struct ParticleEffect
    {
        float lifetime = 2.0f;
        float spawnRate = 60.0f;
        uint32_t capacity = 256;
        AssetId texture;
    };

    struct ParticleEffectResult
    {
        std::optional<ParticleEffect> mEffect;
        // Source-only path relative to the game's asset root, empty for cooked data.
        std::string mTexturePath;
        std::string mError;
    };

    bool ValidateParticleEffect(const ParticleEffect &effect, std::string &error);
    ParticleEffectResult ReadParticleEffectSource(std::string_view text, std::string_view assetIdRoot);
    std::vector<std::byte> BuildParticleEffectAsset(const ParticleEffect &effect);
    ParticleEffectResult ReadParticleEffectAsset(const AssetBlobView &blob);
    // Validates the texture dependency as a cooked texture, never a source image.
    ParticleEffectResult LoadParticleEffect(AssetCache &cache, AssetId id);
}

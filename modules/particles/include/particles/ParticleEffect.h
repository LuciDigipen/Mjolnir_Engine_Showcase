/**
 * @file ParticleEffect.h
 * @author sumin.park
 * @brief Versioned particle system and emitter definitions and dependencies.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetBlob.h>
#include <assets/AssetId.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    class AssetCache;

    inline constexpr uint32_t kParticleEffectTypeTag = 0x54524150u;
    inline constexpr uint32_t kParticleEffectContentVersion = 2u;
    inline constexpr uint32_t kParticleEmitterTypeTag = 0x54494D45u;
    inline constexpr uint32_t kParticleEmitterContentVersion = 1u;
    inline constexpr uint32_t kMaxSystemEmitters = 8;
    inline constexpr uint32_t kMaxEffectParticles = 65536;
    inline constexpr float kMaxEffectLifetime = 120.0f;

    enum class ParticleSimulationTarget { GPU, CPU };

    struct ParticleUserParameters
    {
        float spawnRateScale = 1.0f;
        float sizeScale = 1.0f;
    };

    struct ParticleEmitterAsset
    {
        float lifetime = 2.0f;
        float spawnRate = 60.0f;
        uint32_t capacity = 256;
        AssetId texture;
        ParticleSimulationTarget simulationTarget = ParticleSimulationTarget::GPU;
        float size = 0.15f;
        glm::vec4 color{1.0f, 0.4f, 0.1f, 0.5f};
        glm::vec3 velocity{0.0f, 1.0f, 0.0f};
    };

    struct ParticleEffect
    {
        std::vector<AssetId> emitters;
        ParticleUserParameters parameters;
        // Loaded dependencies, omitted from the system blob itself.
        std::vector<ParticleEmitterAsset> emitterDefinitions;
    };

    struct ParticleEffectResult
    {
        std::optional<ParticleEffect> mEffect;
        std::vector<std::string> mEmitterPaths;
        std::string mError;
    };

    struct ParticleEmitterAssetResult
    {
        std::optional<ParticleEmitterAsset> mEmitter;
        std::string mTexturePath;
        std::string mError;
    };

    bool ValidateParticleUserParameters(const ParticleUserParameters &parameters, std::string &error);
    bool ValidateParticleEmitterAsset(const ParticleEmitterAsset &emitter, std::string &error);
    // Unresolved definitions may be empty, runtime instances require every dependency.
    bool ValidateParticleEffect(const ParticleEffect &effect, std::string &error);
    ParticleEffectResult ReadParticleEffectSource(std::string_view text, std::string_view assetIdRoot);
    std::vector<std::byte> BuildParticleEffectAsset(const ParticleEffect &effect);
    ParticleEffectResult ReadParticleEffectAsset(const AssetBlobView &blob);
    ParticleEffectResult LoadParticleEffect(AssetCache &cache, AssetId id);

    ParticleEmitterAssetResult ReadParticleEmitterSource(std::string_view text, std::string_view assetIdRoot);
    std::vector<std::byte> BuildParticleEmitterAsset(const ParticleEmitterAsset &emitter);
    ParticleEmitterAssetResult ReadParticleEmitterAsset(const AssetBlobView &blob);
    // Validates the texture dependency as a cooked texture, never a source image.
    ParticleEmitterAssetResult LoadParticleEmitterAsset(AssetCache &cache, AssetId id);
}

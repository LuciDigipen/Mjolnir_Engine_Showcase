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
    inline constexpr uint32_t kParticleEmitterContentVersion = 4u;
    inline constexpr uint32_t kParticleScriptTypeTag = 0x52435350u;
    inline constexpr uint32_t kParticleScriptContentVersion = 2u;
    inline constexpr uint32_t kParticleScriptLayoutTypeTag = 0x594C5350u;
    inline constexpr uint32_t kParticleScriptLayoutContentVersion = 2u;
    inline constexpr uint32_t kMaxSystemEmitters = 8;
    inline constexpr uint32_t kMaxEffectParticles = 65536;
    inline constexpr float kMaxEffectLifetime = 120.0f;

    enum class ParticleSimulationTarget { GPU, CPU };

    struct ParticleUserParameters
    {
        float spawnRateScale = 1.0f;
        float sizeScale = 1.0f;
        glm::vec3 wind{0.0f};
    };

    struct ParticleModule
    {
        std::string name;
        std::vector<float> parameters;
    };

    // Transport values are mapped explicitly by the scene bridge, no renderer dependency.
    struct ParticleRendererProperties
    {
        uint32_t alignment = 0; // 0 screen, 1 velocity
        uint32_t facing = 0; // 0 camera plane, 1 camera position
        uint32_t blend = 0; // 0 alpha, 1 additive
        uint32_t sort = 0; // 0 back to front, 1 none
        float softDistance = 0.25f;
        float maxDistance = 65536.0f;
        bool lit = false;
        bool fog = true;
        bool textured = false;
    };

    struct ParticleEmitterAsset
    {
        float lifetime = 2.0f;
        float spawnRate = 60.0f;
        uint32_t capacity = 256;
        glm::vec3 localOffset{0.0f};
        float spawnPerMeter = 0.0f;
        uint32_t burst = 0;
        uint32_t seed = 1;
        float inheritVelocity = 0.0f;
        float teleportDistance = 100.0f;
        AssetId texture;
        ParticleSimulationTarget simulationTarget = ParticleSimulationTarget::GPU;
        ParticleRendererProperties renderer;
        float size = 0.15f;
        glm::vec4 color{1.0f, 0.4f, 0.1f, 0.5f};
        glm::vec3 velocity{0.0f, 1.0f, 0.0f};
        std::vector<ParticleModule> spawnModules{{"Initialize", {}}};
        std::vector<ParticleModule> updateModules{{"Age", {}}, {"Integrate", {}}};
        AssetId script;
        AssetId scriptLayout;
        // Loaded cooked dependency, no source shader reaches the runtime.
        std::vector<std::byte> scriptCode;
        uint32_t textureWidth = 0, textureHeight = 0;
        std::vector<uint8_t> texturePixels;
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
    std::string GenerateParticleScript(const ParticleEmitterAsset &emitter);
    std::string ParticleScriptLayout();
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

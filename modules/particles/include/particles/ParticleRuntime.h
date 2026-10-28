/**
 * @file ParticleRuntime.h
 * @author sumin.park
 * @brief Bounded logical instances and generational playback handles.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "particles/ParticleEffect.h"
#include "particles/ParticleRibbon.h"

#include <core/ecs/Entity.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/math/WorldCoordinates.h>

#include <cstdint>
#include <span>
#include <vector>

namespace mir
{
    inline constexpr uint32_t kMaxParticleEmitterInstances = 64;
    inline constexpr uint32_t kParticleSpawnBudget = 4096;
    inline constexpr float kParticleFloatWorldLimitMeters = 65536.0f;
    inline constexpr float kParticleTileRadius = 64.0f;

    struct ParticleHandle
    {
        uint32_t index = UINT32_MAX;
        uint32_t generation = 0;
        bool IsNull() const { return index == UINT32_MAX || generation == 0; }
        bool operator==(const ParticleHandle &) const = default;
    };

    struct ParticleEmitterHandle
    {
        uint32_t index = UINT32_MAX;
        uint32_t generation = 0;
        bool IsNull() const { return index == UINT32_MAX || generation == 0; }
        bool operator==(const ParticleEmitterHandle &) const = default;
    };

    enum class ParticleExecutionState { Active, Inactive, InactiveClear, Complete };

    struct ParticleSystemInstance
    {
        ParticleHandle handle;
        AssetId effectId;
        ParticleEffect effect;
        WorldPose pose;
        WorldPosition tileOrigin;
        Entity owner = kNullEntity;
        ParticleExecutionState state = ParticleExecutionState::Active;
        bool paused = false;
        double drainRemaining = 0.0;
        ParticleUserParameters parameters;
        std::vector<ParticleEmitterHandle> emitters;
    };

    // Fractions interpolate the accepted attachment sweep, ages are at packet end.
    struct ParticleBirth
    {
        float fraction = 1.0f;
        float age = 0.0f;
        uint32_t sequence = 0;
        uint32_t reserved = 0;
    };
    static_assert(sizeof(ParticleBirth) == 16);

    struct ParticleEmitterInstance
    {
        ParticleRibbonState ribbon;
        ParticleEmitterHandle handle;
        ParticleHandle system;
        AssetId emitterId;
        ParticleEmitterAsset definition;
        WorldPose pose;
        ParticleExecutionState state = ParticleExecutionState::Active;
        bool paused = false;
        WorldPosition attachmentPosition;
        WorldPosition previousPosition;
        WorldPosition spawnStart;
        glm::vec3 attachmentVelocity{0};
        glm::vec3 emissionVelocity{0};
        std::vector<ParticleBirth> births;
        double motionSeconds = 0.0;
        double distanceRemainder = 0.0;
        uint32_t birthSequence = 0;
        bool burstPending = true;
        bool resetMotionOnPose = false;
        bool teleported = false;
        double spawnRemainder = 0.0;
        double drainRemaining = 0.0;
        uint32_t spawns = 0;
        float seconds = 0.0f;
    };

    struct ParticleRuntimeStats
    {
        uint32_t active = 0;
        uint32_t activeEmitters = 0;
        uint64_t rejectedStarts = 0;
        uint64_t droppedBudgetSpawns = 0;
        uint64_t evictedRibbonPoints = 0;
        uint64_t rejectedRibbonPoints = 0;
        uint64_t ticks = 0;
        double droppedSeconds = 0.0;
    };

    class ParticleRuntime
    {
    public:
        explicit ParticleRuntime(uint32_t capacity = 64);
        ParticleRuntime(const ParticleRuntime &) = delete;
        ParticleRuntime &operator=(const ParticleRuntime &) = delete;
        ParticleRuntime(ParticleRuntime &&) = delete;
        ParticleRuntime &operator=(ParticleRuntime &&) = delete;

        ParticleHandle Start(AssetId effectId, const ParticleEffect &effect,
            WorldPose pose = {}, Entity owner = kNullEntity, const ParticleUserParameters *overrides = nullptr);
        const ParticleSystemInstance *Find(ParticleHandle handle) const;
        const ParticleEmitterInstance *FindEmitter(ParticleEmitterHandle handle) const;
        bool Stop(ParticleHandle handle);
        bool ResumeEmission(ParticleHandle handle);
        bool Pause(ParticleHandle handle, bool paused);
        bool SetPose(ParticleHandle handle, const WorldPose &pose);
        bool SetParameters(ParticleHandle handle, const ParticleUserParameters &parameters);
        bool SetExecutionState(ParticleHandle handle, ParticleExecutionState state);
        bool Destroy(ParticleHandle handle);
        void Detach(Entity owner);
        void Clear();
        // Fixed 60 Hz, at most four steps, no catch-up queue after a hitch.
        void Advance(float dt);
        const ParticleRuntimeStats &Stats() const { return mStats; }
        uint32_t Capacity() const { return static_cast<uint32_t>(mSlots.size()); }
        bool HasCapacity() const { return !mFree.empty() && !mEmitterFree.empty(); }
        std::span<const ParticleSystemInstance> Slots() const { return mSlots; }
        std::span<const ParticleEmitterInstance> EmitterSlots() const { return mEmitters; }

    private:
        ParticleSystemInstance *Mutable(ParticleHandle handle);
        ParticleEmitterInstance *MutableEmitter(ParticleEmitterHandle handle);
        void ReleaseEmitter(ParticleEmitterHandle handle);
        std::vector<ParticleSystemInstance> mSlots;
        std::vector<uint32_t> mGenerations;
        std::vector<uint32_t> mFree;
        std::vector<ParticleEmitterInstance> mEmitters;
        std::vector<uint32_t> mEmitterGenerations;
        std::vector<uint32_t> mEmitterFree;
        ParticleRuntimeStats mStats;
        double mRemainder = 0.0;
    };
}

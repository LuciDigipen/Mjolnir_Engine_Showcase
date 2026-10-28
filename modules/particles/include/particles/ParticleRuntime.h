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

#include <core/ecs/Entity.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/math/WorldCoordinates.h>

#include <cstdint>
#include <span>
#include <vector>

namespace mir
{
    struct ParticleHandle
    {
        uint32_t index = UINT32_MAX;
        uint32_t generation = 0;
        bool IsNull() const { return index == UINT32_MAX || generation == 0; }
        bool operator==(const ParticleHandle &) const = default;
    };

    enum class ParticlePhase { Emitting, Draining };

    struct ParticleInstance
    {
        ParticleHandle handle;
        AssetId effectId;
        ParticleEffect effect;
        WorldPose pose;
        Entity owner = kNullEntity;
        ParticlePhase phase = ParticlePhase::Emitting;
        bool paused = false;
        double drainRemaining = 0.0;
    };

    struct ParticleRuntimeStats
    {
        uint32_t active = 0;
        uint64_t rejectedStarts = 0;
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
            WorldPose pose = {}, Entity owner = kNullEntity);
        const ParticleInstance *Find(ParticleHandle handle) const;
        bool Stop(ParticleHandle handle);
        bool ResumeEmission(ParticleHandle handle);
        bool Pause(ParticleHandle handle, bool paused);
        bool SetPose(ParticleHandle handle, const WorldPose &pose);
        bool Destroy(ParticleHandle handle);
        void Detach(Entity owner);
        void Clear();
        // Fixed 60 Hz, at most four steps, no catch-up queue after a hitch.
        void Advance(float dt);
        const ParticleRuntimeStats &Stats() const { return mStats; }
        uint32_t Capacity() const { return static_cast<uint32_t>(mSlots.size()); }
        bool HasCapacity() const { return !mFree.empty(); }
        std::span<const ParticleInstance> Slots() const { return mSlots; }

    private:
        ParticleInstance *Mutable(ParticleHandle handle);
        std::vector<ParticleInstance> mSlots;
        std::vector<uint32_t> mGenerations;
        std::vector<uint32_t> mFree;
        ParticleRuntimeStats mStats;
        double mRemainder = 0.0;
    };
}

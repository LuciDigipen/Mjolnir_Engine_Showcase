/**
 * @file ParticleRuntime.cpp
 * @author sumin.park
 * @brief Bounded playback state, drain timing and stale-handle rejection.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles/ParticleRuntime.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace mir
{
    namespace
    {
        bool ValidPose(const WorldPose &pose)
        {
            for (int c = 0; c < 3; ++c)
                for (int r = 0; r < 3; ++r)
                    if (!std::isfinite(pose.linear[c][r]))
                        return false;
            return true;
        }
    }

    ParticleRuntime::ParticleRuntime(uint32_t capacity)
    {
        if (capacity > 64)
            throw std::invalid_argument("particle instance capacity cannot exceed 64");
        mSlots.resize(capacity);
        mGenerations.resize(capacity, 1);
        mFree.reserve(capacity);
        for (uint32_t i = capacity; i > 0; --i)
            mFree.push_back(i - 1);
    }

    ParticleHandle ParticleRuntime::Start(AssetId effectId, const ParticleEffect &effect,
        WorldPose pose, Entity owner)
    {
        std::string error;
        if (mFree.empty() || effectId.IsNull() || !ValidateParticleEffect(effect, error) || !ValidPose(pose))
        {
            ++mStats.rejectedStarts;
            return {};
        }
        const uint32_t index = mFree.back();
        mFree.pop_back();
        const ParticleHandle handle{index, mGenerations[index]};
        mSlots[index] = ParticleInstance{handle, effectId, effect, pose, owner};
        ++mStats.active;
        return handle;
    }

    const ParticleInstance *ParticleRuntime::Find(ParticleHandle handle) const
    {
        if (handle.IsNull() || handle.index >= mSlots.size() || mSlots[handle.index].handle != handle)
            return nullptr;
        return &mSlots[handle.index];
    }

    ParticleInstance *ParticleRuntime::Mutable(ParticleHandle handle)
    {
        return const_cast<ParticleInstance *>(std::as_const(*this).Find(handle));
    }

    bool ParticleRuntime::Stop(ParticleHandle handle)
    {
        auto *instance = Mutable(handle);
        if (instance == nullptr)
            return false;
        if (instance->phase != ParticlePhase::Draining)
        {
            instance->phase = ParticlePhase::Draining;
            // Reserve the full lifetime of the last emission, independently of GPU alive counts.
            instance->drainRemaining = instance->effect.lifetime;
        }
        return true;
    }

    bool ParticleRuntime::Pause(ParticleHandle handle, bool paused)
    {
        auto *instance = Mutable(handle);
        if (instance == nullptr)
            return false;
        instance->paused = paused;
        return true;
    }

    bool ParticleRuntime::ResumeEmission(ParticleHandle handle)
    {
        auto *instance = Mutable(handle);
        if (instance == nullptr)
            return false;
        instance->phase = ParticlePhase::Emitting;
        instance->drainRemaining = 0.0;
        return true;
    }

    bool ParticleRuntime::SetPose(ParticleHandle handle, const WorldPose &pose)
    {
        auto *instance = Mutable(handle);
        if (instance == nullptr || instance->phase != ParticlePhase::Emitting || !ValidPose(pose))
            return false;
        instance->pose = pose;
        return true;
    }

    bool ParticleRuntime::Destroy(ParticleHandle handle)
    {
        if (Find(handle) == nullptr)
            return false;
        mSlots[handle.index] = {};
        --mStats.active;
        // Retire a slot on generation exhaustion rather than resurrect an old handle.
        if (mGenerations[handle.index] != UINT32_MAX)
        {
            ++mGenerations[handle.index];
            mFree.push_back(handle.index);
        }
        return true;
    }

    void ParticleRuntime::Detach(Entity owner)
    {
        if (owner.IsNull())
            return;
        for (auto &instance : mSlots)
            if (!instance.handle.IsNull() && instance.owner == owner)
            {
                Stop(instance.handle);
                instance.owner = kNullEntity;
                instance.paused = false;
            }
    }

    void ParticleRuntime::Clear()
    {
        for (const auto &instance : mSlots)
            if (!instance.handle.IsNull())
                Destroy(instance.handle);
        mRemainder = 0.0;
        mStats = {};
    }

    void ParticleRuntime::Advance(float dt)
    {
        if (!std::isfinite(dt) || dt <= 0)
            return;
        constexpr double kStep = 1.0 / 60.0;
        const double total = mRemainder + dt;
        const double wholeSteps = std::floor(total / kStep);
        const uint32_t steps = static_cast<uint32_t>(std::min(wholeSteps, 4.0));
        mRemainder = std::fmod(total, kStep);
        mStats.droppedSeconds += std::max(0.0, wholeSteps - steps) * kStep;
        mStats.ticks += steps;
        for (auto &instance : mSlots)
            if (!instance.handle.IsNull() && !instance.paused && instance.phase == ParticlePhase::Draining)
            {
                instance.drainRemaining -= steps * kStep;
                if (instance.drainRemaining <= 1e-6)
                    Destroy(instance.handle);
            }
    }
}

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
            throw std::invalid_argument("particle system capacity cannot exceed 64");
        mSlots.resize(capacity);
        mGenerations.resize(capacity, 1);
        mFree.reserve(capacity);
        for (uint32_t i = capacity; i > 0; --i)
            mFree.push_back(i - 1);
        mEmitters.resize(kMaxParticleEmitterInstances);
        mEmitterGenerations.resize(kMaxParticleEmitterInstances, 1);
        mEmitterFree.reserve(kMaxParticleEmitterInstances);
        for (uint32_t i = kMaxParticleEmitterInstances; i > 0; --i)
            mEmitterFree.push_back(i - 1);
    }

    ParticleHandle ParticleRuntime::Start(AssetId effectId, const ParticleEffect &effect,
        WorldPose pose, Entity owner, const ParticleUserParameters *overrides)
    {
        const auto parameters = overrides ? *overrides : effect.parameters;
        std::string error;
        if (mFree.empty() || effectId.IsNull() || !ValidateParticleEffect(effect, error) ||
            effect.emitterDefinitions.size() != effect.emitters.size() ||
            mEmitterFree.size() < effect.emitters.size() ||
            !ValidateParticleUserParameters(parameters, error) || !ValidPose(pose))
        {
            ++mStats.rejectedStarts;
            return {};
        }
        // Complete all allocating work before consuming any system or emitter slots.
        ParticleSystemInstance instance;
        instance.effectId = effectId;
        instance.effect = effect;
        instance.pose = pose;
        instance.owner = owner;
        instance.parameters = parameters;
        instance.emitters.reserve(effect.emitters.size());
        const uint32_t index = mFree.back();
        const ParticleHandle handle{index, mGenerations[index]};
        instance.handle = handle;
        for (size_t i = 0; i < effect.emitters.size(); ++i)
        {
            const uint32_t emitterIndex = mEmitterFree[mEmitterFree.size() - 1 - i];
            instance.emitters.push_back({emitterIndex, mEmitterGenerations[emitterIndex]});
        }
        mSlots[index] = std::move(instance);
        mFree.pop_back();
        for (size_t i = 0; i < effect.emitters.size(); ++i)
        {
            const auto emitterHandle = mSlots[index].emitters[i];
            ParticleEmitterInstance emitter;
            emitter.handle = emitterHandle;
            emitter.system = handle;
            emitter.emitterId = effect.emitters[i];
            emitter.definition = effect.emitterDefinitions[i];
            emitter.pose = pose;
            mEmitters[emitterHandle.index] = emitter;
            mEmitterFree.pop_back();
        }
        ++mStats.active;
        mStats.activeEmitters += static_cast<uint32_t>(effect.emitters.size());
        return handle;
    }

    const ParticleSystemInstance *ParticleRuntime::Find(ParticleHandle handle) const
    {
        if (handle.IsNull() || handle.index >= mSlots.size() || mSlots[handle.index].handle != handle)
            return nullptr;
        return &mSlots[handle.index];
    }

    const ParticleEmitterInstance *ParticleRuntime::FindEmitter(ParticleEmitterHandle handle) const
    {
        if (handle.IsNull() || handle.index >= mEmitters.size() || mEmitters[handle.index].handle != handle)
            return nullptr;
        return &mEmitters[handle.index];
    }

    ParticleSystemInstance *ParticleRuntime::Mutable(ParticleHandle handle)
    {
        return const_cast<ParticleSystemInstance *>(std::as_const(*this).Find(handle));
    }

    ParticleEmitterInstance *ParticleRuntime::MutableEmitter(ParticleEmitterHandle handle)
    {
        return const_cast<ParticleEmitterInstance *>(std::as_const(*this).FindEmitter(handle));
    }

    bool ParticleRuntime::Stop(ParticleHandle handle)
    {
        return SetExecutionState(handle, ParticleExecutionState::Inactive);
    }

    bool ParticleRuntime::Pause(ParticleHandle handle, bool paused)
    {
        auto *instance = Mutable(handle);
        if (instance == nullptr)
            return false;
        instance->paused = paused;
        for (const auto emitterHandle : instance->emitters)
            if (auto *emitter = MutableEmitter(emitterHandle))
            {
                emitter->paused = paused;
                if (paused) { emitter->spawns = 0; emitter->seconds = 0; }
            }
        return true;
    }

    bool ParticleRuntime::ResumeEmission(ParticleHandle handle)
    {
        return SetExecutionState(handle, ParticleExecutionState::Active);
    }

    bool ParticleRuntime::SetPose(ParticleHandle handle, const WorldPose &pose)
    {
        auto *instance = Mutable(handle);
        if (instance == nullptr || instance->state != ParticleExecutionState::Active || !ValidPose(pose))
            return false;
        instance->pose = pose;
        for (const auto emitterHandle : instance->emitters)
            if (auto *emitter = MutableEmitter(emitterHandle))
                emitter->pose = pose;
        return true;
    }

    bool ParticleRuntime::SetParameters(ParticleHandle handle, const ParticleUserParameters &parameters)
    {
        auto *instance = Mutable(handle);
        std::string error;
        if (instance == nullptr || !ValidateParticleUserParameters(parameters, error))
            return false;
        instance->parameters = parameters;
        return true;
    }

    bool ParticleRuntime::SetExecutionState(ParticleHandle handle, ParticleExecutionState state)
    {
        auto *instance = Mutable(handle);
        if (instance == nullptr)
            return false;
        if (state == ParticleExecutionState::InactiveClear || state == ParticleExecutionState::Complete)
            return Destroy(handle);
        if (state != ParticleExecutionState::Active && state != ParticleExecutionState::Inactive)
            return false;
        if (instance->state == state)
            return true;
        instance->state = state;
        instance->drainRemaining = 0;
        for (const auto emitterHandle : instance->emitters)
            if (auto *emitter = MutableEmitter(emitterHandle))
            {
                emitter->state = state;
                emitter->spawns = 0;
                emitter->drainRemaining = state == ParticleExecutionState::Inactive ? emitter->definition.lifetime : 0;
                instance->drainRemaining = std::max(instance->drainRemaining, emitter->drainRemaining);
            }
        return true;
    }

    void ParticleRuntime::ReleaseEmitter(ParticleEmitterHandle handle)
    {
        if (FindEmitter(handle) == nullptr)
            return;
        mEmitters[handle.index] = {};
        --mStats.activeEmitters;
        // Generation exhaustion permanently retires the slot, stale handles never resurrect.
        if (mEmitterGenerations[handle.index] != UINT32_MAX)
        {
            ++mEmitterGenerations[handle.index];
            mEmitterFree.push_back(handle.index);
        }
    }

    bool ParticleRuntime::Destroy(ParticleHandle handle)
    {
        const auto *instance = Find(handle);
        if (instance == nullptr)
            return false;
        for (const auto emitterHandle : instance->emitters)
            ReleaseEmitter(emitterHandle);
        mSlots[handle.index] = {};
        --mStats.active;
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
                Pause(instance.handle, false);
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
        for (auto &emitter : mEmitters)
        {
            emitter.spawns = 0;
            emitter.seconds = 0;
        }
        if (!std::isfinite(dt) || dt <= 0)
            return;
        constexpr double kStep = 1.0 / 60.0;
        const double total = mRemainder + dt;
        const double wholeSteps = std::floor(total / kStep);
        const uint32_t steps = static_cast<uint32_t>(std::min(wholeSteps, 4.0));
        mRemainder = std::fmod(total, kStep);
        mStats.droppedSeconds += std::max(0.0, wholeSteps - steps) * kStep;
        mStats.ticks += steps;
        uint32_t budget = kParticleSpawnBudget;
        for (auto &instance : mSlots)
        {
            if (instance.handle.IsNull() || instance.paused)
                continue;
            bool any = false;
            instance.drainRemaining = 0;
            for (const auto emitterHandle : instance.emitters)
            {
                auto *emitter = MutableEmitter(emitterHandle);
                if (emitter == nullptr || emitter->state == ParticleExecutionState::Complete)
                    continue;
                emitter->seconds = static_cast<float>(steps * kStep);
                if (emitter->state == ParticleExecutionState::Active)
                {
                    const double births = emitter->spawnRemainder + steps * kStep *
                        emitter->definition.spawnRate * instance.parameters.spawnRateScale;
                    const double wholeBirths = std::floor(births);
                    emitter->spawnRemainder = births - wholeBirths;
                    emitter->spawns = static_cast<uint32_t>(std::min(wholeBirths, double(budget)));
                    budget -= emitter->spawns;
                    mStats.droppedBudgetSpawns += static_cast<uint64_t>(wholeBirths) - emitter->spawns;
                }
                else if (emitter->state == ParticleExecutionState::Inactive)
                {
                    emitter->drainRemaining -= steps * kStep;
                    if (emitter->drainRemaining <= 1e-6)
                    {
                        emitter->state = ParticleExecutionState::Complete;
                        emitter->seconds = 0;
                        continue;
                    }
                    instance.drainRemaining = std::max(instance.drainRemaining, emitter->drainRemaining);
                }
                any = true;
            }
            if (!any)
                Destroy(instance.handle);
        }
    }
}

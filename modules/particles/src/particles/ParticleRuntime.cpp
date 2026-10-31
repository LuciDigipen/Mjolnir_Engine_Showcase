/**
 * @file ParticleRuntime.cpp
 * @author sumin.park
 * @brief Bounded playback state, drain timing and stale-handle rejection.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles/ParticleRuntime.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
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
            if constexpr (sizeof(WorldReal) == sizeof(float))
                for (int axis = 0; axis < 3; ++axis)
                    if (std::abs(pose.position.Meters()[axis]) > kParticleFloatWorldLimitMeters)
                        return false;
            return true;
        }

        // Matches the placement offset limit in ValidateParticleEffect.
        constexpr float kMaxInstanceOffset = 4096.0f;

        bool Attachment(const WorldPose &pose, const ParticleEmitterAsset &definition, const glm::vec3 &instanceOffset,
            WorldPosition &position, glm::vec3 &velocity)
        {
            const auto point = WorldPosition::FromMeters(pose.position.Meters() +
                WorldMat3(pose.linear) * WorldVec3(definition.localOffset + instanceOffset));
            if (!point) return false;
            if constexpr (sizeof(WorldReal) == sizeof(float))
                for (int axis = 0; axis < 3; ++axis)
                    if (std::abs(point->Meters()[axis]) > kParticleFloatWorldLimitMeters)
                        return false;
            velocity = glm::vec3(WorldMat3(pose.linear) * WorldVec3(definition.velocity));
            for (int axis = 0; axis < 3; ++axis)
                if (!std::isfinite(velocity[axis])) return false;
            position = *point;
            return true;
        }
    }

    ParticleRuntime::ParticleRuntime(uint32_t capacity, ParticleBudgets budgets)
    {
        if (capacity > 64)
            throw std::invalid_argument("particle system capacity cannot exceed 64");
        if (!ConfigureBudgets(budgets)) throw std::invalid_argument("invalid particle budgets");
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
        WorldPose pose, Entity owner, const ParticleUserParameters *overrides, bool preview)
    {
        const auto parameters = overrides ? *overrides : effect.parameters;
        std::string error;
        if (mFree.empty() || mEmitterFree.size() < effect.emitters.size())
        {
            ++mStats.rejectedStarts; ++mStats.rejectedCapacityStarts; return {};
        }
        if (effectId.IsNull() || !ValidateParticleEffect(effect, error) ||
            effect.emitterDefinitions.size() != effect.emitters.size() ||
            !ValidateParticleUserParameters(parameters, error) || !ValidPose(pose))
        {
            ++mStats.rejectedStarts;
            return {};
        }
        RefreshStats();
        uint64_t reservation = sizeof(ParticleSystemInstance) + effect.emitters.size() * (sizeof(AssetId) + sizeof(ParticleEmitterHandle));
        uint32_t gpu = 0, ribbon = 0;
        for (const auto &definition : effect.emitterDefinitions)
        {
            uint64_t bytes = sizeof(ParticleEmitterAsset) + definition.scriptCode.size() + definition.texturePixels.size();
            for (const auto *stack :{ &definition.spawnBehaviors, &definition.updateBehaviors })
                for (const auto &behavior : *stack) bytes += sizeof(ParticleBehavior) + behavior.name.size() + 32 + behavior.parameters.size() * sizeof(float);
            reservation += bytes * 2 + sizeof(ParticleEmitterInstance);
            if (definition.simulationTarget == ParticleSimulationTarget::GPU)
            {
                gpu += definition.capacity; reservation += mBudgets.births * sizeof(ParticleBirth);
            }
            else { ribbon += definition.capacity; reservation += definition.capacity * sizeof(ParticleRibbonPoint); }
        }
        if (mStats.activeEmitters + effect.emitters.size() > mBudgets.emitters ||
            mStats.gpuCapacity + gpu > mBudgets.gpuParticles || mStats.ribbonCapacity + ribbon > mBudgets.ribbonPoints)
        {
            ++mStats.rejectedStarts; ++mStats.rejectedCapacityStarts; return {};
        }
        if (reservation > mBudgets.cpuBytes || mStats.cpuBytes > mBudgets.cpuBytes - reservation)
        {
            ++mStats.rejectedStarts; ++mStats.rejectedMemoryStarts; return {};
        }
        for (size_t i = 0; i < effect.emitterDefinitions.size(); ++i)
        {
            auto definition = effect.emitterDefinitions[i];
            if (!effect.placementOffsets.empty()) definition.localOffset += effect.placementOffsets[i];
            WorldPosition attachment;
            glm::vec3 velocity;
            if (!Attachment(pose, definition, glm::vec3(0), attachment, velocity))
            {
                ++mStats.rejectedStarts;
                return {};
            }
        }
        // Complete all allocating work before consuming any system or emitter slots.
        ParticleSystemInstance instance;
        instance.preview = preview;
        instance.reservedBytes = reservation;
        instance.effectId = effectId;
        instance.effect = effect;
        instance.pose = pose;
        instance.tileOrigin = pose.position;
        instance.owner = owner;
        instance.parameters = parameters;
        instance.emitters.reserve(effect.emitters.size());
        const uint32_t index = mFree.back();
        const ParticleHandle handle{ index, mGenerations[index] };
        instance.handle = handle;
        for (size_t i = 0; i < effect.emitters.size(); ++i)
        {
            const uint32_t emitterIndex = mEmitterFree[mEmitterFree.size() - 1 - i];
            instance.emitters.push_back({ emitterIndex, mEmitterGenerations[emitterIndex] });
        }
        std::vector<ParticleEmitterInstance> prepared;
        prepared.reserve(effect.emitters.size());
        for (size_t i = 0; i < effect.emitters.size(); ++i)
        {
            const auto emitterHandle = instance.emitters[i];
            ParticleEmitterInstance emitter;
            emitter.handle = emitterHandle;
            emitter.system = handle;
            emitter.emitterId = effect.emitters[i];
            emitter.definition = effect.emitterDefinitions[i];
            emitter.delayRemaining = emitter.definition.startDelay;
            if (!effect.placementOffsets.empty()) emitter.definition.localOffset += effect.placementOffsets[i];
            if (emitter.definition.simulationTarget == ParticleSimulationTarget::CPU)
                emitter.ribbon.points.reserve(emitter.definition.capacity);
            else emitter.births.reserve(mBudgets.births);
            emitter.pose = pose;
            Attachment(pose, emitter.definition, emitter.instanceOffset, emitter.attachmentPosition, emitter.emissionVelocity);
            emitter.previousPosition = emitter.spawnStart = emitter.attachmentPosition;
            prepared.push_back(std::move(emitter));
        }
        mSlots[index] = std::move(instance);
        mFree.pop_back();
        for (auto &emitter : prepared)
        {
            mEmitters[emitter.handle.index] = std::move(emitter);
            mEmitterFree.pop_back();
        }
        ++mStats.active;
        mStats.activeEmitters += static_cast<uint32_t>(effect.emitters.size());
        RefreshStats();
        return handle;
    }

    bool ParticleRuntime::ConfigureBudgets(const ParticleBudgets &budgets)
    {
        if (mStats.active || budgets.emitters == 0 || budgets.emitters > 64 || !budgets.gpuParticles || budgets.gpuParticles > 65536 ||
            !budgets.ribbonPoints || budgets.ribbonPoints > 65536 || !budgets.births || budgets.births > 4096 ||
            budgets.cpuBytes < 65536 || budgets.cpuBytes > 64ull * 1024 * 1024) return false;
        mBudgets = budgets;
        return true;
    }

    void ParticleRuntime::RefreshStats()
    {
        mStats.gpuCapacity = mStats.ribbonCapacity = mStats.ribbonPoints = 0;
        mStats.cpuBytes = mSlots.capacity() * sizeof(ParticleSystemInstance) + mEmitters.capacity() * sizeof(ParticleEmitterInstance) +
            (mGenerations.capacity() + mFree.capacity() + mEmitterGenerations.capacity() + mEmitterFree.capacity()) * sizeof(uint32_t);
        for (const auto &slot : mSlots) if (!slot.handle.IsNull()) mStats.cpuBytes += slot.reservedBytes;
        for (const auto &emitter : mEmitters)
        {
            if (emitter.handle.IsNull()) continue;
            if (emitter.definition.simulationTarget == ParticleSimulationTarget::GPU) mStats.gpuCapacity += emitter.definition.capacity;
            else mStats.ribbonCapacity += emitter.definition.capacity;
            mStats.ribbonPoints += static_cast<uint32_t>(emitter.ribbon.points.size());
        }
        mStats.cpuHighWaterBytes = std::max(mStats.cpuHighWaterBytes, mStats.cpuBytes);
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
        const bool resumed = instance->paused && !paused;
        instance->paused = paused;
        for (const auto emitterHandle : instance->emitters)
            if (auto *emitter = MutableEmitter(emitterHandle))
            {
                emitter->paused = paused;
                if (resumed) { emitter->resetMotionOnPose = true; emitter->ribbon.connected = false; }
                if (paused)
                {
                    emitter->spawns = 0;
                    emitter->seconds = 0;
                    emitter->births.clear();
                    emitter->previousPosition = emitter->spawnStart = emitter->attachmentPosition;
                    emitter->attachmentVelocity = glm::vec3(0);
                    emitter->motionSeconds = 0;
                }
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
        for (const auto emitterHandle : instance->emitters)
            if (const auto *emitter = FindEmitter(emitterHandle))
            {
                WorldPosition attachment;
                glm::vec3 velocity;
                if (!Attachment(pose, emitter->definition, emitter->instanceOffset, attachment, velocity)) return false;
            }
        instance->pose = pose;
        for (const auto emitterHandle : instance->emitters)
            if (auto *emitter = MutableEmitter(emitterHandle))
            {
                emitter->pose = pose;
                Attachment(pose, emitter->definition, emitter->instanceOffset, emitter->attachmentPosition, emitter->emissionVelocity);
                if (emitter->resetMotionOnPose)
                {
                    emitter->previousPosition = emitter->spawnStart = emitter->attachmentPosition;
                    emitter->attachmentVelocity = glm::vec3(0);
                    emitter->motionSeconds = 0;
                    emitter->resetMotionOnPose = false;
                }
            }
        return true;
    }

    bool ParticleRuntime::SetEmitterOffsets(ParticleHandle handle, std::span<const glm::vec3> offsets)
    {
        auto *instance = Mutable(handle);
        if (instance == nullptr || instance->state != ParticleExecutionState::Active)
            return false;
        const size_t count = std::min(offsets.size(), instance->emitters.size());
        // Reject the whole update before touching any emitter, like SetPose.
        for (size_t i = 0; i < count; ++i)
        {
            for (int axis = 0; axis < 3; ++axis)
                if (!std::isfinite(offsets[i][axis]) || std::abs(offsets[i][axis]) > kMaxInstanceOffset) return false;
            const auto *emitter = FindEmitter(instance->emitters[i]);
            WorldPosition attachment;
            glm::vec3 velocity;
            if (emitter == nullptr || !Attachment(instance->pose, emitter->definition, offsets[i], attachment, velocity))
                return false;
        }
        for (size_t i = 0; i < count; ++i)
        {
            auto *emitter = MutableEmitter(instance->emitters[i]);
            if (emitter->instanceOffset == offsets[i]) continue;
            emitter->instanceOffset = offsets[i];
            Attachment(instance->pose, emitter->definition, emitter->instanceOffset,
                emitter->attachmentPosition, emitter->emissionVelocity);
            emitter->previousPosition = emitter->spawnStart = emitter->attachmentPosition;
            emitter->attachmentVelocity = glm::vec3(0);
            emitter->motionSeconds = 0;
            emitter->ribbon.connected = false;
        }
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
                emitter->ribbon.connected = false;
                emitter->spawns = 0;
                emitter->births.clear();
                emitter->distanceRemainder = 0;
                emitter->resetMotionOnPose = state == ParticleExecutionState::Active;
                emitter->previousPosition = emitter->spawnStart = emitter->attachmentPosition;
                emitter->motionSeconds = 0;
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
        RefreshStats();
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

    void ParticleRuntime::Advance(float dt, bool previewsOnly)
    {
        const auto started = std::chrono::steady_clock::now();
        mStats.qualityReducedEmitters = 0;
        for (auto &emitter : mEmitters)
        {
            emitter.spawns = 0;
            emitter.seconds = 0;
            emitter.births.clear();
            emitter.teleported = false;
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
        uint32_t budget = mBudgets.births;
        for (auto &instance : mSlots)
        {
            if (instance.handle.IsNull() || (previewsOnly && !instance.preview)) continue;
            if (instance.paused)
            {
                for (const auto handle : instance.emitters)
                    if (auto *emitter = MutableEmitter(handle))
                    {
                        emitter->previousPosition = emitter->spawnStart = emitter->attachmentPosition;
                        emitter->motionSeconds = 0;
                        emitter->attachmentVelocity = glm::vec3(0);
                    }
                continue;
            }
            if (steps && glm::length(instance.pose.position.Meters() - instance.tileOrigin.Meters()) > WorldReal(kParticleTileRadius))
                instance.tileOrigin = instance.pose.position;
            bool any = false;
            instance.drainRemaining = 0;
            for (const auto emitterHandle : instance.emitters)
            {
                auto *emitter = MutableEmitter(emitterHandle);
                if (emitter == nullptr || emitter->state == ParticleExecutionState::Complete)
                    continue;
                if (emitter->resetMotionOnPose)
                {
                    emitter->previousPosition = emitter->spawnStart = emitter->attachmentPosition;
                    emitter->attachmentVelocity = glm::vec3(0);
                    emitter->motionSeconds = 0;
                    emitter->resetMotionOnPose = false;
                }
                auto parameters = instance.parameters;
                const auto &q = emitter->definition.quality;
                float distanceScale = 1;
                if (mQualityOrigin)
                {
                    const double distance = glm::length(emitter->attachmentPosition.Meters() - mQualityOrigin->Meters());
                    const float t = static_cast<float>(std::clamp((distance - q.nearDistance) / (q.farDistance - q.nearDistance), 0.0, 1.0));
                    distanceScale = std::lerp(1.0f, q.minSpawnScale, t);
                }
                emitter->qualityScale = q.importance * distanceScale;
                if (emitter->qualityScale < 0.999f) ++mStats.qualityReducedEmitters;
                parameters.spawnRateScale *= emitter->qualityScale;
                emitter->seconds = static_cast<float>(steps * kStep);
                emitter->motionSeconds += dt;
                if (emitter->state == ParticleExecutionState::Active && steps && emitter->delayRemaining > 0)
                {
                    emitter->delayRemaining -= steps * kStep;
                    emitter->previousPosition = emitter->spawnStart = emitter->attachmentPosition;
                    emitter->attachmentVelocity = glm::vec3(0);
                    emitter->motionSeconds = 0;
                }
                else if (emitter->state == ParticleExecutionState::Active && steps)
                {
                    const auto displacement = emitter->attachmentPosition.Meters() - emitter->previousPosition.Meters();
                    const double distance = static_cast<double>(glm::length(displacement));
                    emitter->teleported = distance > emitter->definition.teleportDistance;
                    // Ordinary clock remainders retain the complete motion span, only dropped hitch time clips it.
                    const double sweepScale = wholeSteps > 4 ?
                        std::min(1.0, double(emitter->seconds) / emitter->motionSeconds) : 1.0;
                    const auto swept = displacement * WorldReal(sweepScale);
                    const auto start = WorldPosition::FromMeters(emitter->attachmentPosition.Meters() - swept);
                    emitter->spawnStart = emitter->teleported || !start ? emitter->attachmentPosition : *start;
                    emitter->attachmentVelocity = emitter->teleported ? glm::vec3(0) :
                        glm::vec3(displacement / WorldReal(emitter->motionSeconds));
                    if (emitter->teleported) emitter->distanceRemainder = 0;
                    if (emitter->definition.simulationTarget == ParticleSimulationTarget::CPU)
                    {
                        AdvanceParticleRibbon(*emitter, parameters, budget, mStats);
                        emitter->previousPosition = emitter->attachmentPosition;
                        emitter->motionSeconds = 0;
                        any = true;
                        continue;
                    }
                    uint64_t requested = 0;
                    auto append = [&](double amount, double &remainder)
                        {
                            const double oldRemainder = remainder;
                            const double count = std::floor(oldRemainder + amount + 1e-9);
                            remainder = std::max(0.0, oldRemainder + amount - count);
                            const auto accepted = static_cast<uint32_t>(std::min(count, double(budget)));
                            for (uint32_t i = 0; i < accepted; ++i)
                            {
                                const auto fraction = static_cast<float>(std::clamp((1.0 - oldRemainder + i) / amount, 0.0, 1.0));
                                emitter->births.push_back({ fraction, (1.0f - fraction) * emitter->seconds, 0, 0 });
                            }
                            budget -= accepted;
                            requested += static_cast<uint64_t>(count);
                        };
                        // The initial burst is a single activation event, resuming a drain does not replay it.
                    if (emitter->burstPending)
                    {
                        const auto count = static_cast<uint32_t>(emitter->definition.burst * emitter->qualityScale);
                        const auto accepted = std::min(count, budget);
                        for (uint32_t i = 0; i < accepted; ++i)
                            emitter->births.push_back({ 0, emitter->seconds, 0, 0 });
                        budget -= accepted;
                        requested += count;
                        emitter->burstPending = false;
                    }
                    append(steps * kStep * emitter->definition.spawnRate * parameters.spawnRateScale,
                        emitter->spawnRemainder);
                    const double sweptDistance = emitter->teleported ? 0 : distance * sweepScale;
                    append(sweptDistance * emitter->definition.spawnPerMeter * parameters.spawnRateScale,
                        emitter->distanceRemainder);
                    std::stable_sort(emitter->births.begin(), emitter->births.end(),
                        [](const auto &a, const auto &b) { return a.fraction < b.fraction; });
                    for (uint32_t i = 0; i < emitter->births.size(); ++i)
                        emitter->births[i].sequence = emitter->birthSequence + i;
                    emitter->birthSequence += static_cast<uint32_t>(requested);
                    emitter->spawns = static_cast<uint32_t>(emitter->births.size());
                    mStats.droppedBudgetSpawns += requested - emitter->spawns;
                    emitter->previousPosition = emitter->attachmentPosition;
                    emitter->motionSeconds = 0;
                }
                else if (emitter->state == ParticleExecutionState::Inactive)
                {
                    if (emitter->definition.simulationTarget == ParticleSimulationTarget::CPU)
                        AdvanceParticleRibbon(*emitter, parameters, budget, mStats);
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
        RefreshStats();
        mStats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
}

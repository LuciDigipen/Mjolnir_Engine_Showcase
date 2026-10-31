/**
 * @file ParticleWorld.cpp
 * @author sumin.park
 * @brief Coordinates emitter requests, cached cooked assets and staged reloads.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles/ParticleWorld.h"

#include <assets/AssetCache.h>
#include <assets/AssetPath.h>
#include <core/log/Log.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace mir
{
    ParticleWorld::ParticleWorld(World &world, AssetCache &assets, std::string assetIdRoot, uint32_t capacity)
        : mWorld(world), mAssets(assets), mAssetIdRoot(std::move(assetIdRoot)), mRuntime(capacity)
    {}

    const ParticleSystemInstance *ParticleWorld::Find(Entity owner) const
    {
        const auto *emitter = mWorld.GetComponent<ParticleSystem>(owner);
        const auto *instance = emitter ? mRuntime.Find(emitter->instance) : nullptr;
        return instance != nullptr && instance->owner == owner ? instance : nullptr;
    }

    bool ParticleWorld::Start(Entity owner)
    {
        auto *emitter = mWorld.GetComponent<ParticleSystem>(owner);
        if (emitter == nullptr)
            return false;
        if (const auto *instance = Find(owner); instance && instance->state == ParticleExecutionState::Inactive)
        {
            mRuntime.ResumeEmission(instance->handle);
        }
        emitter->initialized = true;
        emitter->wantsPlay = true;
        emitter->failedEffect = {};
        return true;
    }

    bool ParticleWorld::Stop(Entity owner)
    {
        auto *emitter = mWorld.GetComponent<ParticleSystem>(owner);
        if (emitter == nullptr)
            return false;
        emitter->initialized = true;
        emitter->wantsPlay = false;
        if (const auto *instance = Find(owner))
            mRuntime.Stop(instance->handle);
        return true;
    }

    bool ParticleWorld::Pause(Entity owner, bool paused)
    {
        auto *emitter = mWorld.GetComponent<ParticleSystem>(owner);
        if (emitter == nullptr)
            return false;
        emitter->paused = paused;
        if (const auto *instance = Find(owner))
            mRuntime.Pause(instance->handle, paused);
        return true;
    }

    bool ParticleWorld::SetParameters(Entity owner, const ParticleUserParameters &parameters)
    {
        auto *system = mWorld.GetComponent<ParticleSystem>(owner);
        std::string error;
        if (!system || !ValidateParticleUserParameters(parameters, error)) return false;
        if (const auto *instance = Find(owner))
            if (!mRuntime.SetParameters(instance->handle, parameters)) return false;
        system->overrideParameters = true;
        system->userParameters = parameters;
        return true;
    }

    std::optional<ParticleUserParameters> ParticleWorld::BaseParameters(Entity owner) const
    {
        const auto *system = mWorld.GetComponent<ParticleSystem>(owner);
        if (!system) return std::nullopt;
        if (system->overrideParameters) return system->userParameters;
        if (const auto *instance = Find(owner)) return instance->effect.parameters;
        const auto end = std::find(std::begin(system->systemName), std::end(system->systemName), '\0');
        if (end == std::end(system->systemName) || end == std::begin(system->systemName)) return system->userParameters;
        const std::string_view name(system->systemName, static_cast<size_t>(end - std::begin(system->systemName)));
        const auto logical = MakeLogicalAssetPath(mAssetIdRoot, name);
        if (!logical.Ok()) return system->userParameters;
        const auto parsed = LoadParticleEffect(mAssets, MakeAssetId(logical.path));
        return parsed.mEffect ? parsed.mEffect->parameters : system->userParameters;
    }

    bool ParticleWorld::Destroy(Entity owner)
    {
        auto *emitter = mWorld.GetComponent<ParticleSystem>(owner);
        if (emitter == nullptr)
            return false;
        if (const auto *instance = Find(owner))
            mRuntime.Destroy(instance->handle);
        emitter->instance = {};
        emitter->initialized = true;
        emitter->wantsPlay = false;
        emitter->failedEffect = {};
        return true;
    }

    bool ParticleWorld::Reset(Entity owner)
    {
        if (!Destroy(owner))
            return false;
        auto &emitter = *mWorld.GetComponent<ParticleSystem>(owner);
        emitter.initialized = false;
        emitter.paused = false;
        return true;
    }

    ParticleHandle ParticleWorld::StartPreview(AssetId id, const WorldPose &pose)
    {
        auto effect = LoadParticleEffect(mAssets, id);
        if (!effect.mEffect) return {};
        StopPreview();
        mPreview = mRuntime.Start(id, *effect.mEffect, pose, kNullEntity, nullptr, true);
        return mPreview;
    }

    void ParticleWorld::StopPreview()
    {
        mRuntime.Destroy(mPreview);
        mPreview = {};
    }

    void ParticleWorld::Synchronize(Entity owner, ParticleSystem &emitter, const WorldPose &pose)
    {
        if (!emitter.initialized)
        {
            emitter.initialized = true;
            emitter.wantsPlay = emitter.playOnStart;
        }
        if (!emitter.wantsPlay)
        {
            if (const auto *current = Find(owner))
            {
                mRuntime.SetParameters(current->handle,
                    emitter.overrideParameters ? emitter.userParameters : current->effect.parameters);
                mRuntime.Pause(current->handle, emitter.paused);
            }
            return;
        }
        const auto end = std::find(std::begin(emitter.systemName), std::end(emitter.systemName), '\0');
        const std::string_view name(emitter.systemName, static_cast<size_t>(end - std::begin(emitter.systemName)));
        if (name.empty())
        {
            if (const auto *instance = Find(owner))
                mRuntime.Destroy(instance->handle);
            emitter.instance = {};
            emitter.failedEffect = {};
            return;
        }
        const auto logical = MakeLogicalAssetPath(mAssetIdRoot, name);
        const AssetId id = logical.Ok() ? MakeAssetId(logical.path) : MakeAssetId(name);
        const ParticleSystemInstance *instance = Find(owner);
        if (instance != nullptr && instance->effectId != id)
        {
            mRuntime.Destroy(instance->handle);
            emitter.instance = {};
            instance = nullptr;
        }
        if (instance == nullptr)
        {
            emitter.instance = {};
            if (emitter.failedEffect == id)
                return;
            if (!mRuntime.HasCapacity())
                return;
            auto parsed = logical.Ok() && end != std::end(emitter.systemName)
                ? LoadParticleEffect(mAssets, id) : ParticleEffectResult{};
            if (!parsed.mEffect)
            {
                emitter.failedEffect = id;
                MIR_LOG_ERROR("Particle effect '{}': {}", name,
                    parsed.mError.empty() ? "invalid effect asset path" : parsed.mError);
                return;
            }
            emitter.instance = mRuntime.Start(id, *parsed.mEffect, pose, owner,
                emitter.overrideParameters ? &emitter.userParameters : nullptr);
            if (emitter.instance.IsNull())
                return; // Capacity failures may recover after other instances drain.
            emitter.failedEffect = {};
        }
        if (emitter.overrideParameters) mRuntime.SetParameters(emitter.instance, emitter.userParameters);
        else if (const auto *current = mRuntime.Find(emitter.instance)) mRuntime.SetParameters(emitter.instance, current->effect.parameters);
        mRuntime.SetPose(emitter.instance, pose);
        if (const auto *running = mRuntime.Find(emitter.instance))
        {
            // Offsets belong to placement ids, the emitter at index i is the placement with the i-th id.
            std::array<glm::vec3, kMaxSystemEmitters> offsets{};
            for (size_t i = 0; i < running->effect.emitters.size() && i < offsets.size(); ++i)
            {
                const uint32_t id = running->effect.placementIds.empty() ? static_cast<uint32_t>(i + 1) : running->effect.placementIds[i];
                if (const size_t slot = FindEmitterOffsetSlot(emitter, id); slot != kNoEmitterOffsetSlot)
                    offsets[i] = emitter.emitterOffsets[slot];
            }
            mRuntime.SetEmitterOffsets(emitter.instance, offsets);
        }
        mRuntime.Pause(emitter.instance, emitter.paused);
    }

    void ParticleWorld::Detach(Entity owner)
    {
        mRuntime.Detach(owner);
    }

    void ParticleWorld::Clear()
    {
        mPreview = {};
        mRuntime.Clear();
        auto &query = mWorld.GetOrCreateQuery<ParticleSystem>();
        query.ForEach([](Entity, ParticleSystem &emitter)
            {
                emitter.instance = {};
                emitter.initialized = false;
                emitter.wantsPlay = false;
                emitter.paused = false;
                emitter.failedEffect = {};
            });
        AbortReload();
    }

    void ParticleWorld::PrepareReload(AssetCache &candidate)
    {
        AbortReload();
        for (const auto &instance : mRuntime.Slots())
        {
            if (instance.handle.IsNull() || mPendingEffects.contains(instance.effectId.value))
                continue;
            auto parsed = LoadParticleEffect(candidate, instance.effectId);
            if (!parsed.mEffect)
                throw std::runtime_error("particle reload rejected: " + parsed.mError);
            mPendingEffects.emplace(instance.effectId.value, *parsed.mEffect);
        }
        mReloadPrepared = true;
    }

    void ParticleWorld::CommitReload()
    {
        if (!mReloadPrepared)
            return;
        mPreview = {};
        mRuntime.Clear();
        auto &query = mWorld.GetOrCreateQuery<ParticleSystem>();
        query.ForEach([](Entity, ParticleSystem &emitter)
            {
                // Preserve stopped/paused intent, reset only transient bindings.
                emitter.instance = {};
                emitter.failedEffect = {};
            });
        AbortReload();
    }

    void ParticleWorld::AbortReload()
    {
        mPendingEffects.clear();
        mReloadPrepared = false;
    }
}

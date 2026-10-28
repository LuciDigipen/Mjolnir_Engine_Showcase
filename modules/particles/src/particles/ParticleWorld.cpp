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
#include <iterator>
#include <stdexcept>
#include <utility>

namespace mir
{
    ParticleWorld::ParticleWorld(World &world, AssetCache &assets, std::string assetIdRoot, uint32_t capacity)
        : mWorld(world), mAssets(assets), mAssetIdRoot(std::move(assetIdRoot)), mRuntime(capacity)
    {}

    const ParticleInstance *ParticleWorld::Find(Entity owner) const
    {
        const auto *emitter = mWorld.GetComponent<ParticleEmitter>(owner);
        const auto *instance = emitter ? mRuntime.Find(emitter->instance) : nullptr;
        return instance != nullptr && instance->owner == owner ? instance : nullptr;
    }

    bool ParticleWorld::Start(Entity owner)
    {
        auto *emitter = mWorld.GetComponent<ParticleEmitter>(owner);
        if (emitter == nullptr)
            return false;
        if (const auto *instance = Find(owner); instance && instance->phase == ParticlePhase::Draining)
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
        auto *emitter = mWorld.GetComponent<ParticleEmitter>(owner);
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
        auto *emitter = mWorld.GetComponent<ParticleEmitter>(owner);
        if (emitter == nullptr)
            return false;
        emitter->paused = paused;
        if (const auto *instance = Find(owner))
            mRuntime.Pause(instance->handle, paused);
        return true;
    }

    bool ParticleWorld::Destroy(Entity owner)
    {
        auto *emitter = mWorld.GetComponent<ParticleEmitter>(owner);
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
        auto &emitter = *mWorld.GetComponent<ParticleEmitter>(owner);
        emitter.initialized = false;
        emitter.paused = false;
        return true;
    }

    void ParticleWorld::Synchronize(Entity owner, ParticleEmitter &emitter, const WorldPose &pose)
    {
        if (!emitter.initialized)
        {
            emitter.initialized = true;
            emitter.wantsPlay = emitter.playOnStart;
        }
        if (!emitter.wantsPlay)
            return;
        const auto end = std::find(std::begin(emitter.effectName), std::end(emitter.effectName), '\0');
        const std::string_view name(emitter.effectName, static_cast<size_t>(end - std::begin(emitter.effectName)));
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
        const ParticleInstance *instance = Find(owner);
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
            auto parsed = logical.Ok() && end != std::end(emitter.effectName)
                ? LoadParticleEffect(mAssets, id) : ParticleEffectResult{};
            if (!parsed.mEffect)
            {
                emitter.failedEffect = id;
                MIR_LOG_ERROR("Particle effect '{}': {}", name,
                    parsed.mError.empty() ? "invalid effect asset path" : parsed.mError);
                return;
            }
            emitter.instance = mRuntime.Start(id, *parsed.mEffect, pose, owner);
            if (emitter.instance.IsNull())
                return; // Capacity failures may recover after other instances drain.
            emitter.failedEffect = {};
        }
        mRuntime.SetPose(emitter.instance, pose);
        mRuntime.Pause(emitter.instance, emitter.paused);
    }

    void ParticleWorld::Detach(Entity owner)
    {
        mRuntime.Detach(owner);
    }

    void ParticleWorld::Clear()
    {
        mRuntime.Clear();
        auto &query = mWorld.GetOrCreateQuery<ParticleEmitter>();
        query.ForEach([](Entity, ParticleEmitter &emitter)
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
        mRuntime.Clear();
        auto &query = mWorld.GetOrCreateQuery<ParticleEmitter>();
        query.ForEach([](Entity, ParticleEmitter &emitter)
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

/**
 * @file ParticleWorld.h
 * @author sumin.park
 * @brief World-facing playback, cooked asset resolution and reload staging.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "particles/ParticleRuntime.h"
#include "particles/components/ParticleEmitter.h"

#include <core/ecs/World.h>

#include <string>
#include <unordered_map>

namespace mir
{
    class AssetCache;

    class ParticleWorld
    {
    public:
        ParticleWorld(World &world, AssetCache &assets, std::string assetIdRoot, uint32_t capacity = 64);
        ParticleWorld(const ParticleWorld &) = delete;
        ParticleWorld &operator=(const ParticleWorld &) = delete;

        // Start accepts a request, asset resolution/allocation happens in LateUpdate.
        bool Start(Entity owner);
        bool Stop(Entity owner);
        bool Pause(Entity owner, bool paused);
        bool Reset(Entity owner);
        bool Destroy(Entity owner);
        const ParticleInstance *Find(Entity owner) const;
        ParticleRuntime &Runtime() { return mRuntime; }
        const ParticleRuntime &Runtime() const { return mRuntime; }

        void Synchronize(Entity owner, ParticleEmitter &emitter, const WorldPose &pose);
        void Clear();
        void Detach(Entity owner);
        void PrepareReload(AssetCache &candidate);
        void CommitReload();
        void AbortReload();

    private:
        World &mWorld;
        AssetCache &mAssets;
        std::string mAssetIdRoot;
        ParticleRuntime mRuntime;
        std::unordered_map<uint64_t, ParticleEffect> mPendingEffects;
        bool mReloadPrepared = false;
    };
}

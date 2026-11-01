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
#include "particles/components/ParticleSystem.h"

#include <core/ecs/World.h>

#include <optional>
#include <string>
#include <unordered_map>

namespace mir
{
    class AssetCache;
    class AssetResolver;

    class ParticleWorld
    {
    public:
        ParticleWorld(World &world, AssetCache &assets, const AssetResolver &resolver, uint32_t capacity = 64);
        ParticleWorld(const ParticleWorld &) = delete;
        ParticleWorld &operator=(const ParticleWorld &) = delete;

        // Start accepts a request, asset resolution/allocation happens in LateUpdate.
        bool Start(Entity owner);
        bool Stop(Entity owner);
        bool Pause(Entity owner, bool paused);
        bool SetParameters(Entity owner, const ParticleUserParameters &parameters);
        // Parameters a setter should start from
        std::optional<ParticleUserParameters> BaseParameters(Entity owner) const;
        bool Reset(Entity owner);
        bool Destroy(Entity owner);
        const ParticleSystemInstance *Find(Entity owner) const;
        ParticleRuntime &Runtime() { return mRuntime; }
        const ParticleRuntime &Runtime() const { return mRuntime; }

        ParticleHandle StartPreview(AssetId id, const WorldPose &pose);
        void StopPreview();
        ParticleHandle PreviewHandle() const { return mPreview; }
        void Synchronize(Entity owner, ParticleSystem &emitter, const WorldPose &pose);
        void Clear();
        void Detach(Entity owner);
        void PrepareReload(AssetCache &candidate);
        void CommitReload();
        void AbortReload();

    private:
        World &mWorld;
        AssetCache &mAssets;
        const AssetResolver &mResolver;
        ParticleRuntime mRuntime;
        std::unordered_map<uint64_t, ParticleEffect> mPendingEffects;
        bool mReloadPrepared = false;
        ParticleHandle mPreview;
    };
}

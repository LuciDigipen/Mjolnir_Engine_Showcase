/**
 * @file ParticlesModule.cpp
 * @author sumin.park
 * @brief Registers CPU-only particle playback and scene/reload lifecycle hooks.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles/ParticlesModule.h"

#include "particles/ComponentRegistration.h"
#include "particles/ParticleWorld.h"

#include <assets/AssetsModule.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/Query.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/components/WorldTransform.h>

#include <utility>

namespace mir
{
    namespace
    {
        class ParticleUpdateSystem final : public ISystem
        {
        public:
            explicit ParticleUpdateSystem(ParticleWorld &particles) : mParticles(particles) {}
            void OnStart(SystemContext &context) override
            {
                mQuery = &context.world.GetOrCreateQuery<ParticleSystem, const WorldTransform>();
            }
            void OnUpdate(SystemContext &context) override
            {
                if (IsEditing(context.world))
                {
                    mParticles.Runtime().Advance(context.dt, true);
                    return;
                }
                if (!IsSimulating(context.world)) return;
                mQuery->ForEach([this](Entity owner, ParticleSystem &emitter, const WorldTransform &transform)
                    {
                        mParticles.Synchronize(owner, emitter, transform.Pose());
                    });
                mParticles.Runtime().Advance(context.dt);
            }
        private:
            ParticleWorld &mParticles;
            Query<ParticleSystem, const WorldTransform> *mQuery = nullptr;
        };
    }

    ParticlesModule::ParticlesModule(ModuleContext context, AssetsModule &assets,
        std::string assetIdRoot, uint32_t capacity)
        : mParticles(context.world.EmplaceResource<ParticleWorld>(
            context.world, assets.Cache(), std::move(assetIdRoot), capacity))
    {
        context.world.EmplaceResource<ParticleDiagnostics>();
        RegisterParticleComponents();
        mHooks = context.world.SetComponentHooks<ParticleSystem>({
            .onRemove = [this](World &, Entity owner) { mParticles.Detach(owner); } });
        context.scheduler.AddSystem<ParticleUpdateSystem>(SystemPhase::LateUpdate,
            SystemOrder{ 70 }, mParticles);
        const Stage resetStages[]{ Stage::SceneUnload, Stage::SceneLoad, Stage::PlayBegin, Stage::PlayEnd };
        for (size_t i = 0; i < 4; ++i)
            mStages[i] = context.scheduler.AddStage(resetStages[i],
                [this](SystemContext &) { mParticles.Clear(); });
        mStages[4] = context.scheduler.AddStage(Stage::ReloadPrepare,
            [this, &assets](SystemContext &) { mParticles.PrepareReload(assets.Candidate()); });
        mStages[5] = context.scheduler.AddStage(Stage::ReloadCommit,
            [this](SystemContext &) { mParticles.CommitReload(); });
        mStages[6] = context.scheduler.AddStage(Stage::ReloadAbort,
            [this](SystemContext &) { mParticles.AbortReload(); });
    }
}

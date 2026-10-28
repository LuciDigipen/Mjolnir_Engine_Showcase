/**
 * @file ParticlesRuntimeModule.cpp
 * @author sumin.park
 * @brief Collects bounded spawn work after propagated poses and CPU lifecycle updates.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "particles_runtime/ParticlesRuntimeModule.h"

#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <particles/ParticleWorld.h>
#include <particles/ParticlesModule.h>
#include <renderer/RendererModule.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace mir
{
    namespace
    {
        class ParticleSubmitSystem final : public ISystem
        {
        public:
            ParticleSubmitSystem(ParticleWorld &particles, VulkanRenderer &renderer)
                : mParticles(particles), mRenderer(renderer) {}
            void Reset()
            {
                mTicks = 0;
                mGenerations.fill(0);
                mRemainders.fill(0);
                mRenderer.SubmitParticles({});
            }
            void OnUpdate(SystemContext &context) override
            {
                auto &runtime = mParticles.Runtime();
                const auto ticks = runtime.Stats().ticks;
                const uint64_t steps = std::min<uint64_t>(ticks >= mTicks ? ticks - mTicks : ticks, 4);
                mTicks = ticks;
                std::array<ParticleSubmission, kGpuParticleInstances> submissions;
                uint32_t count = 0;
                for (const auto &instance : runtime.Slots())
                {
                    if (instance.handle.IsNull()) continue;
                    const auto index = instance.handle.index;
                    if (mGenerations[index] != instance.handle.generation)
                    {
                        mGenerations[index] = instance.handle.generation;
                        mRemainders[index] = 0;
                    }
                    const float seconds = IsSimulating(context.world) && !instance.paused ? float(steps) / 60.0f : 0;
                    uint32_t spawns = 0;
                    if (instance.phase == ParticlePhase::Emitting)
                    {
                        const double total = mRemainders[index] + seconds * instance.effect.spawnRate;
                        const double whole = std::floor(total);
                        mRemainders[index] = total - whole;
                        spawns = static_cast<uint32_t>(whole);
                    }
                    ParticleSubmission p;
                    p.index = index;
                    p.generation = instance.handle.generation;
                    p.capacity = instance.effect.capacity;
                    p.spawns = spawns;
                    p.anchor = instance.pose.position;
                    p.seconds = seconds;
                    p.lifetime = instance.effect.lifetime;
                    submissions[count++] = p;
                }
                mRenderer.SubmitParticles(std::span(submissions.data(), count));
            }
        private:
            ParticleWorld &mParticles;
            VulkanRenderer &mRenderer;
            uint64_t mTicks = 0;
            std::array<uint32_t, kGpuParticleInstances> mGenerations{};
            std::array<double, kGpuParticleInstances> mRemainders{};
        };
    }

    ParticlesRuntimeModule::ParticlesRuntimeModule(ModuleContext context, ParticlesModule &particles, RendererModule &renderer)
    {
        auto &system = context.scheduler.AddSystem<ParticleSubmitSystem>(SystemPhase::LateUpdate,
            SystemOrder{80}, particles.Particles(), renderer.Renderer());
        const Stage stages[]{Stage::SceneUnload, Stage::SceneLoad, Stage::PlayBegin, Stage::PlayEnd, Stage::ReloadCommit};
        for (size_t i = 0; i < mStages.size(); ++i)
            mStages[i] = context.scheduler.AddStage(stages[i],
                [system = &system](SystemContext &) { system->Reset(); });
    }
}

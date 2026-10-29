/**
 * @file ParticlesRuntimeModule.cpp
 * @author sumin.park
 * @brief Collects bounded spawn work after propagated poses and CPU lifecycle updates.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles_runtime/ParticlesRuntimeModule.h"

#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <particles/ParticleWorld.h>
#include <particles/ParticlesModule.h>
#include <particles_gpu/ParticlesGpuModule.h>

namespace mir
{
    namespace
    {
        ParticleRenderProperties RenderProperties(const ParticleRendererProperties &r)
        {
            return {r.alignment, r.facing, r.blend, r.sort, r.softDistance, r.maxDistance, r.lit, r.fog, r.textured};
        }
    }
    std::vector<ParticleSubmission> BuildParticleRenderData(const ParticleRuntime &runtime, bool simulating)
    {
        std::vector<ParticleSubmission> submissions;
        submissions.reserve(runtime.Stats().activeEmitters);
        for (const auto &emitter : runtime.EmitterSlots())
        {
            if (emitter.handle.IsNull() || emitter.state == ParticleExecutionState::Complete ||
                emitter.definition.simulationTarget != ParticleSimulationTarget::GPU) continue;
            const auto *system = runtime.Find(emitter.system);
            if (!system) continue;
            ParticleSubmission input;
            input.renderer = RenderProperties(emitter.definition.renderer);
            input.scriptKey = emitter.definition.script.value;
            input.scriptCode = emitter.definition.scriptCode;
            input.index = emitter.handle.index;
            input.generation = emitter.handle.generation;
            input.capacity = emitter.definition.capacity;
            input.spawns = simulating ? emitter.spawns : 0;
            input.seconds = simulating ? emitter.seconds : 0;
            input.anchor = emitter.attachmentPosition;
            input.emissionStart = emitter.spawnStart;
            input.tileOrigin = system->tileOrigin;
            input.explicitTile = input.swept = true;
            input.attachmentVelocity = emitter.attachmentVelocity;
            input.inheritVelocity = emitter.definition.inheritVelocity;
            input.wind = system->parameters.wind;
            input.seed = emitter.definition.seed;
            input.textureKey = emitter.definition.texture.value;
            input.textureWidth = emitter.definition.textureWidth;
            input.textureHeight = emitter.definition.textureHeight;
            input.texturePixels = emitter.definition.texturePixels;
            if (simulating)
                for (const auto &birth : emitter.births)
                    input.births.push_back({birth.fraction, birth.age, birth.sequence, 0});
            input.lifetime = emitter.definition.lifetime;
            input.size = emitter.definition.size * system->parameters.sizeScale;
            input.color = emitter.definition.color;
            input.velocity = emitter.emissionVelocity;
            submissions.push_back(input);
        }
        return submissions;
    }
    std::vector<RibbonSubmission> BuildRibbonRenderData(const ParticleRuntime &runtime)
    {
        std::vector<RibbonSubmission> result;
        for (const auto &emitter : runtime.EmitterSlots())
        {
            if (emitter.handle.IsNull() || emitter.definition.simulationTarget != ParticleSimulationTarget::CPU ||
                emitter.state == ParticleExecutionState::Complete || emitter.ribbon.points.empty()) continue;
            RibbonSubmission input;
            input.renderer = RenderProperties(emitter.definition.renderer);
            input.textureKey = emitter.definition.texture.value;
            input.textureWidth = emitter.definition.textureWidth;
            input.textureHeight = emitter.definition.textureHeight;
            input.texturePixels = emitter.definition.texturePixels;
            input.points.reserve(emitter.ribbon.points.size());
            for (const auto &point : emitter.ribbon.points)
                input.points.push_back({point.position, point.textureDistance, point.ribbon, point.link, point.width, point.color});
            result.push_back(std::move(input));
        }
        return result;
    }
    namespace
    {
        class ParticleSubmitSystem final : public ISystem
        {
        public:
            ParticleSubmitSystem(ParticleWorld &particles, ParticlesGpuModule &gpu) : mParticles(particles), mGpu(gpu) {}
            void OnUpdate(SystemContext &context) override
            {
                const auto snapshot = BuildParticleRenderData(mParticles.Runtime(), IsSimulating(context.world));
                mGpu.Submit(snapshot, BuildRibbonRenderData(mParticles.Runtime()));
            }
        private:
            ParticleWorld &mParticles;
            ParticlesGpuModule &mGpu;
        };
    }
    ParticlesRuntimeModule::ParticlesRuntimeModule(ModuleContext context, ParticlesModule &particles, ParticlesGpuModule &gpu)
    {
        context.scheduler.AddSystem<ParticleSubmitSystem>(SystemPhase::LateUpdate, SystemOrder{80}, particles.Particles(), gpu);
        const Stage stages[]{Stage::SceneUnload, Stage::SceneLoad, Stage::PlayBegin, Stage::PlayEnd, Stage::ReloadCommit};
        for (size_t i = 0; i < mStages.size(); ++i)
            mStages[i] = context.scheduler.AddStage(stages[i], [&gpu](SystemContext &) { gpu.Clear(); });
    }
}

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
            input.index = emitter.handle.index;
            input.generation = emitter.handle.generation;
            input.capacity = emitter.definition.capacity;
            input.spawns = simulating ? emitter.spawns : 0;
            input.seconds = simulating ? emitter.seconds : 0;
            input.anchor = emitter.pose.position;
            input.lifetime = emitter.definition.lifetime;
            input.size = emitter.definition.size * system->parameters.sizeScale;
            input.color = emitter.definition.color;
            input.velocity = emitter.definition.velocity;
            submissions.push_back(input);
        }
        return submissions;
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
                mGpu.Submit(snapshot);
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

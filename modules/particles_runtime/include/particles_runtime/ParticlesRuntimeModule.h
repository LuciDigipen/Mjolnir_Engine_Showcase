/**
 * @file ParticlesRuntimeModule.h
 * @author sumin.park
 * @brief Adapts CPU playback into immutable renderer-owned particle submissions.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>

#include <particles_gpu/ParticleSubmission.h>

#include <array>
#include <vector>

namespace mir
{
    class ParticlesModule;
    class ParticleRuntime;
    class ParticlesGpuModule;
    class RenderSystem;
    std::vector<ParticleSubmission> BuildParticleRenderData(const ParticleRuntime &runtime, bool simulating);
    std::vector<RibbonSubmission> BuildRibbonRenderData(const ParticleRuntime &runtime);
    class ParticlesRuntimeModule
    {
    public:
        ParticlesRuntimeModule(ModuleContext context, ParticlesModule &particles, ParticlesGpuModule &gpu, RenderSystem &rendering);
        ParticlesRuntimeModule(const ParticlesRuntimeModule &) = delete;
        ParticlesRuntimeModule &operator=(const ParticlesRuntimeModule &) = delete;
    private:
        std::array<Registration, 5> mStages;
    };
}

/**
 * @file ParticlesGpuModule.h
 * @author sumin.park
 * @brief Registers the particle GPU backend with renderer FX hooks.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "particles_gpu/ParticleSubmission.h"

#include <core/Registration.h>

#include <memory>
#include <span>

namespace mir
{
    class VulkanRenderer;
    namespace gpu { class ParticleFxSystem; }
    class ParticlesGpuModule
    {
    public:
        explicit ParticlesGpuModule(VulkanRenderer &renderer, ParticleGpuBudgets budgets = {});
        ~ParticlesGpuModule();
        ParticlesGpuModule(const ParticlesGpuModule &) = delete;
        ParticlesGpuModule &operator=(const ParticlesGpuModule &) = delete;
        void Submit(std::span<const ParticleSubmission> inputs, std::span<const RibbonSubmission> ribbons = {});
        ParticleGpuStats Stats() const;
        void Clear();
    private:
        std::unique_ptr<gpu::ParticleFxSystem> mSystem;
        // Unregisters and finishes recorded uses before destroying GPU state above.
        Registration mRegistration;
    };
}

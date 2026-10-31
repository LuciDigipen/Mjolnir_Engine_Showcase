/**
 * @file ParticleRenderCheck.h
 * @author sumin.park
 * @brief Production rendering acceptance checks for particle composition.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>

namespace mir
{
    class VulkanRenderer;
    class ParticlesGpuModule;
    class Window;
    struct ParticleEmitterAsset;
}
void RunParticleRenderCheck(mir::VulkanRenderer &renderer,mir::ParticlesGpuModule &particles,
    mir::Window &window,const mir::ParticleEmitterAsset &emitter,const std::filesystem::path &output,bool rt,bool rr);

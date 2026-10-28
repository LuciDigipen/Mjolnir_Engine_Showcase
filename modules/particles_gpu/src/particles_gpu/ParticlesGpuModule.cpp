/**
 * @file ParticlesGpuModule.cpp
 * @author sumin.park
 * @brief FX registration and ownership of particle GPU datasets, dispatch and sprites.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles_gpu/ParticlesGpuModule.h"

#include "ParticleDataset.h"
#include "ParticleDispatch.h"
#include "ParticleSpriteRenderer.h"

#include <renderer/VulkanRenderer.h>

namespace mir::gpu
{
    class ParticleFxSystem final : public IFxSystem
    {
    public:
        void Submit(std::span<const ParticleSubmission> inputs) { mDataset.Submit(inputs); }
        ParticleGpuStats Stats() const { return mDataset.Stats(); }
        void RecordSimulation(const FxFrameContext &context) override { mDispatch.Record(context, mDataset); }
        bool HasRenderData() const override { return mDataset.Initialized(); }
        void RecordDraw(const FxRenderContext &context) override { mSprites.Record(context, mDataset); }
        void ClearScene() override { Submit({}); }
        void Shutdown() override
        {
            mSprites.Shutdown();
            mDispatch.Shutdown();
            mDataset.Shutdown();
        }
    private:
        ParticleDataset mDataset;
        ParticleDispatch mDispatch;
        ParticleSpriteRenderer mSprites;
    };
}
namespace mir
{
    ParticlesGpuModule::ParticlesGpuModule(VulkanRenderer &renderer)
        : mSystem(std::make_unique<gpu::ParticleFxSystem>()), mRegistration(renderer.RegisterFxSystem(*mSystem)) {}
    ParticlesGpuModule::~ParticlesGpuModule() = default;
    void ParticlesGpuModule::Submit(std::span<const ParticleSubmission> inputs) { mSystem->Submit(inputs); }
    ParticleGpuStats ParticlesGpuModule::Stats() const { return mSystem->Stats(); }
    void ParticlesGpuModule::Clear() { mSystem->ClearScene(); }
}

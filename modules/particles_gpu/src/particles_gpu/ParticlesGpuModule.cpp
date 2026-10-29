/**
 * @file ParticlesGpuModule.cpp
 * @author sumin.park
 * @brief FX registration and ownership of particle GPU datasets, dispatch and sprites.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles_gpu/ParticlesGpuModule.h"

#include "ParticleRenderer.h"

#include "ParticleDataset.h"
#include "ParticleDispatch.h"

#include <renderer/VulkanRenderer.h>

namespace mir::gpu
{
    class ParticleFxSystem final : public IFxSystem
    {
    public:
        void Submit(std::span<const ParticleSubmission> inputs, std::span<const RibbonSubmission> ribbons = {}) { mDataset.Submit(inputs); mRenderer.Submit(ribbons); }
        ParticleGpuStats Stats() const { auto stats=mDataset.Stats(); mRenderer.Stats(stats); return stats; }
        void RecordSimulation(const FxFrameContext &context) override { mDispatch.Record(context, mDataset); mRenderer.PrepareTextures(context, mDataset); }
        bool HasRenderData() const override { return mDataset.Initialized() || mRenderer.HasRibbons(); }
        void PrepareDraw(const FxRenderContext &context) override { mRenderer.PrepareDraw(context,mDataset); }
        void RecordDraw(const FxRenderContext &context) override { mRenderer.Record(context, mDataset); }
        void ClearScene() override { Submit({}); }
        void Shutdown() override
        {
            mRenderer.Shutdown();
            mDispatch.Shutdown();
            mDataset.Shutdown();
        }
    private:
        ParticleRenderer mRenderer;
        ParticleDataset mDataset;
        ParticleDispatch mDispatch;
    };
}
namespace mir
{
    ParticlesGpuModule::ParticlesGpuModule(VulkanRenderer &renderer)
        : mSystem(std::make_unique<gpu::ParticleFxSystem>()), mRegistration(renderer.RegisterFxSystem(*mSystem)) {}
    ParticlesGpuModule::~ParticlesGpuModule() = default;
    void ParticlesGpuModule::Submit(std::span<const ParticleSubmission> inputs, std::span<const RibbonSubmission> ribbons) { mSystem->Submit(inputs, ribbons); }
    ParticleGpuStats ParticlesGpuModule::Stats() const { return mSystem->Stats(); }
    void ParticlesGpuModule::Clear() { mSystem->ClearScene(); }
}

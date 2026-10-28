/**
 * @file ParticlesGpuModule.cpp
 * @author sumin.park
 * @brief FX registration and ownership of particle GPU datasets, dispatch and sprites.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles_gpu/ParticlesGpuModule.h"

#include "ParticleRibbonRenderer.h"

#include "ParticleDataset.h"
#include "ParticleDispatch.h"
#include "ParticleSpriteRenderer.h"

#include <renderer/VulkanRenderer.h>

namespace mir::gpu
{
    class ParticleFxSystem final : public IFxSystem
    {
    public:
        void Submit(std::span<const ParticleSubmission> inputs, std::span<const RibbonSubmission> ribbons = {}) { mDataset.Submit(inputs); mRibbons.Submit(ribbons); }
        ParticleGpuStats Stats() const { return mDataset.Stats(); }
        void RecordSimulation(const FxFrameContext &context) override { mDispatch.Record(context, mDataset); mSprites.PrepareTextures(context, mDataset); mRibbons.Prepare(context); }
        bool HasRenderData() const override { return mDataset.Initialized() || mRibbons.HasData(); }
        void RecordDraw(const FxRenderContext &context) override { mSprites.Record(context, mDataset); mRibbons.Record(context); }
        void ClearScene() override { Submit({}); }
        void Shutdown() override
        {
            mRibbons.Shutdown();
            mSprites.Shutdown();
            mDispatch.Shutdown();
            mDataset.Shutdown();
        }
    private:
        ParticleRibbonRenderer mRibbons;
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
    void ParticlesGpuModule::Submit(std::span<const ParticleSubmission> inputs, std::span<const RibbonSubmission> ribbons) { mSystem->Submit(inputs, ribbons); }
    ParticleGpuStats ParticlesGpuModule::Stats() const { return mSystem->Stats(); }
    void ParticlesGpuModule::Clear() { mSystem->ClearScene(); }
}

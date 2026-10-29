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
#include <core/log/SetupError.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace mir::gpu
{
    class ParticleFxSystem final : public IFxSystem
    {
    public:
        explicit ParticleFxSystem(ParticleGpuBudgets budgets) : mBudgets(budgets), mRenderer(budgets), mDataset(budgets)
        {
            if (!budgets.snapshotBytes || budgets.snapshotBytes > 64ull*1024*1024 || budgets.textureBytes < 8 ||
                budgets.textureBytes > 64ull*1024*1024 || budgets.textureUploadBytes < 4 || budgets.textureUploadBytes > 16ull*1024*1024 ||
                !budgets.birthUploads || budgets.birthUploads > 4096 || budgets.ribbonVertices < 6 || budgets.ribbonVertices > 65532 || budgets.ribbonVertices % 6)
                throw std::invalid_argument("invalid particle GPU budgets");
        }
        void Submit(std::span<const ParticleSubmission> inputs, std::span<const RibbonSubmission> ribbons = {})
        {
            mDataset.Submit(inputs);
            mRenderer.Submit(ribbons, mBudgets.snapshotBytes - mDataset.Stats().snapshotBytes);
        }
        ParticleGpuStats Stats() const
        {
            auto stats = mDataset.Stats();
            mRenderer.Stats(stats);
            mGpuHighWater = std::max(mGpuHighWater, stats.gpuBytes);
            stats.gpuHighWaterBytes = mGpuHighWater;
            stats.cpuRecordMs = mCpuMs;
            stats.gpuTimingAvailable = mQueries != VK_NULL_HANDLE;
            stats.simulationGpuMs = mGpuMs[0]; stats.prepareGpuMs = mGpuMs[1]; stats.drawGpuMs = mGpuMs[2];
            return stats;
        }
        void Timestamp(const FxFrameContext &context, uint32_t query)
        {
            if (mQueries) vkCmdWriteTimestamp2(context.commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mQueries, context.frame * 6 + query);
        }
        void RecordSimulation(const FxFrameContext &context) override
        {
            const auto started = std::chrono::steady_clock::now();
            mCpuMs = 0;
            if (!mDevice && context.device.timestampBits && context.device.timestampPeriod > 0)
            {
                mDevice = context.device.device; mBits = context.device.timestampBits; mPeriod = context.device.timestampPeriod;
                const VkQueryPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 6 * kFxFramesInFlight};
                if (vkCreateQueryPool(mDevice, &pool, nullptr, &mQueries) != VK_SUCCESS)
                    throw SetupError(ErrorCategory::System, "Create particle GPU timer", "timestamp queries",
                        "Vulkan query pool creation failed", "Check GPU memory and driver logs.");
            }
            if (mQueries)
            {
                for (uint32_t area = 0; area < 3; ++area)
                    if (mWritten[context.frame] & (1u << area))
                    {
                        uint64_t result[4]{};
                        const auto status = vkGetQueryPoolResults(mDevice, mQueries, context.frame * 6 + area * 2, 2,
                            sizeof(result), result, 2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
                        if (status == VK_SUCCESS && result[1] && result[3])
                        {
                            const uint64_t mask = mBits == 64 ? UINT64_MAX : (uint64_t(1) << mBits) - 1;
                            mGpuMs[area] = double((result[2] - result[0]) & mask) * mPeriod / 1000000.0;
                        }
                    }
                vkCmdResetQueryPool(context.commands, mQueries, context.frame * 6, 6);
            }
            mWritten[context.frame] = 1;
            Timestamp(context, 0);
            mDispatch.Record(context, mDataset);
            mRenderer.PrepareTextures(context, mDataset);
            Timestamp(context, 1);
            mCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        }
        bool HasRenderData() const override { return mDataset.Initialized() || mRenderer.HasRibbons(); }
        void PrepareDraw(const FxRenderContext &context) override
        {
            const auto started = std::chrono::steady_clock::now();
            Timestamp(context.frame, 2); mRenderer.PrepareDraw(context, mDataset); Timestamp(context.frame, 3);
            mWritten[context.frame.frame] |= 2;
            mCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        }
        void RecordDraw(const FxRenderContext &context) override
        {
            const auto started = std::chrono::steady_clock::now();
            Timestamp(context.frame, 4); mRenderer.Record(context, mDataset); Timestamp(context.frame, 5);
            mWritten[context.frame.frame] |= 4;
            mCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        }
        void ClearScene() override { Submit({}); }
        void Shutdown() override
        {
            mRenderer.Shutdown(); mDispatch.Shutdown(); mDataset.Shutdown();
            if (mQueries) vkDestroyQueryPool(mDevice, mQueries, nullptr);
            mQueries = VK_NULL_HANDLE; mDevice = VK_NULL_HANDLE;
            for (auto &written : mWritten) written = 0;
        }
    private:
        ParticleGpuBudgets mBudgets;
        ParticleRenderer mRenderer;
        ParticleDataset mDataset;
        ParticleDispatch mDispatch;
        VkDevice mDevice = VK_NULL_HANDLE;
        VkQueryPool mQueries = VK_NULL_HANDLE;
        uint32_t mBits = 0, mWritten[kFxFramesInFlight]{};
        float mPeriod = 0;
        double mGpuMs[3]{}, mCpuMs = 0;
        mutable uint64_t mGpuHighWater = 0;
    };
}
namespace mir
{
    ParticlesGpuModule::ParticlesGpuModule(VulkanRenderer &renderer, ParticleGpuBudgets budgets)
        : mSystem(std::make_unique<gpu::ParticleFxSystem>(budgets)), mRegistration(renderer.RegisterFxSystem(*mSystem)) {}
    ParticlesGpuModule::~ParticlesGpuModule() = default;
    void ParticlesGpuModule::Submit(std::span<const ParticleSubmission> inputs, std::span<const RibbonSubmission> ribbons) { mSystem->Submit(inputs, ribbons); }
    ParticleGpuStats ParticlesGpuModule::Stats() const { return mSystem->Stats(); }
    void ParticlesGpuModule::Clear() { mSystem->ClearScene(); }
}

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
#include "ParticleVulkan.h"

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
            if(mQueries) stats.gpuBytes+=kFxFramesInFlight*kTimingBytes;
            mGpuHighWater = std::max(mGpuHighWater, stats.gpuBytes);
            stats.gpuHighWaterBytes = mGpuHighWater;
            stats.cpuRecordMs = mCpuMs;
            stats.cpuSimulationRecordMs=mCpuStages[0];stats.cpuTextureRecordMs=mCpuStages[1];
            stats.cpuPrepareRecordMs=mCpuStages[2];stats.cpuDrawRecordMs=mCpuStages[3];
            stats.gpuTimingAvailable = mTimingComplete;
            stats.simulationGpuMs = mGpuMs[0]; stats.uploadGpuMs = mGpuMs[1]; stats.prepareGpuMs = mGpuMs[2]; stats.drawGpuMs = mGpuMs[3];
            return stats;
        }
        void Timestamp(const FxFrameContext &context, uint32_t query)
        {
            if (mQueries) vkCmdWriteTimestamp2(context.commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mQueries, context.frame * 8 + query);
        }
        void CopyTimings(const FxFrameContext &context,uint32_t count)
        {
            if(!mQueries) return;
            MemoryBarrier(context.commands,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdCopyQueryPoolResults(context.commands,mQueries,context.frame*8,count,mTiming[context.frame].buffer,
                0,2*sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            MemoryBarrier(context.commands,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_READ_BIT);
        }
        void RecordSimulation(const FxFrameContext &context) override
        {
            const auto started = std::chrono::steady_clock::now();
            mCpuMs = 0;
            if (!mDevice && context.device.timestampBits && context.device.timestampPeriod > 0)
            {
                mDevice = context.device.device; mBits = context.device.timestampBits; mPeriod = context.device.timestampPeriod;
                mAllocator=context.device.allocator;
                const VkQueryPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 8 * kFxFramesInFlight};
                if (vkCreateQueryPool(mDevice, &pool, nullptr, &mQueries) != VK_SUCCESS)
                    throw SetupError(ErrorCategory::System, "Create particle GPU timer", "timestamp queries",
                        "Vulkan query pool creation failed", "Check GPU memory and driver logs.");
                for(auto &timing:mTiming)
                {
                    VmaAllocationInfo info{};
                    if(!CreateParticleBuffer(mAllocator,kTimingBytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                        timing.buffer,timing.memory,info))
                        throw std::runtime_error("Cannot allocate asynchronous GPU timing results");
                    timing.mapped=static_cast<const uint64_t *>(info.pMappedData);
                }
            }
            mTimingComplete=false;
            if (mQueries)
            {
                if(mCopiedWritten[context.frame])
                {
                    vmaInvalidateAllocation(mAllocator,mTiming[context.frame].memory,0,VK_WHOLE_SIZE);
                    const auto *results=mTiming[context.frame].mapped;
                    mTimingComplete=true;
                    for(uint32_t area=0;area<4;++area)
                    {
                        if(!(mCopiedWritten[context.frame]&(1u<<area))) continue;
                        const auto *result=results+area*4;
                        if(result[1] && result[3])
                        {
                            const uint64_t mask = mBits == 64 ? UINT64_MAX : (uint64_t(1) << mBits) - 1;
                            mGpuMs[area] = double((result[2] - result[0]) & mask) * mPeriod / 1000000.0;
                        }
                        else mTimingComplete=false;
                    }
                }
                // This slot's previous queries completed before the renderer reused it.
                // Copy them before reset, the mapped results are read on its next reuse.
                if(mWritten[context.frame]) CopyTimings(context,(mWritten[context.frame]&8)?8:4);
                mCopiedWritten[context.frame]=mWritten[context.frame];
                MemoryBarrier(context.commands,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT);
                vkCmdResetQueryPool(context.commands, mQueries, context.frame * 8, 8);
            }
            mWritten[context.frame] = 1;
            Timestamp(context, 0);
            mDispatch.Record(context, mDataset);
            Timestamp(context, 1);
            mCpuStages[0]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            const auto uploading=std::chrono::steady_clock::now();
            Timestamp(context, 2);
            mRenderer.PrepareTextures(context, mDataset);
            Timestamp(context, 3);
            mWritten[context.frame] |= 2;
            mCpuStages[1]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-uploading).count();
            mCpuMs=mCpuStages[0]+mCpuStages[1];mCpuStages[2]=mCpuStages[3]=0;
        }
        bool HasRenderData() const override { return mDataset.Initialized() || mRenderer.HasRibbons(); }
        void PrepareDraw(const FxRenderContext &context) override
        {
            const auto started = std::chrono::steady_clock::now();
            Timestamp(context.frame, 4); mRenderer.PrepareDraw(context, mDataset); Timestamp(context.frame, 5);
            mWritten[context.frame.frame] |= 4;
            Timestamp(context.frame, 6);
            mCpuStages[2]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            mCpuMs+=mCpuStages[2];
        }
        void RecordDraw(const FxRenderContext &context) override
        {
            const auto started = std::chrono::steady_clock::now();
            mRenderer.Record(context, mDataset);
            mCpuStages[3]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            mCpuMs+=mCpuStages[3];
        }
        void FinishDraw(const FxRenderContext &context) override
        {
            Timestamp(context.frame, 7);
            mWritten[context.frame.frame] |= 8;
        }
        void ClearScene() override { Submit({}); }
        void Shutdown() override
        {
            mRenderer.Shutdown(); mDispatch.Shutdown(); mDataset.Shutdown();
            if (mQueries) vkDestroyQueryPool(mDevice, mQueries, nullptr);
            for(auto &timing:mTiming)
            {
                if(timing.buffer) vmaDestroyBuffer(mAllocator,timing.buffer,timing.memory);
                timing={};
            }
            mTimingComplete=false;mAllocator=nullptr;
            mQueries = VK_NULL_HANDLE; mDevice = VK_NULL_HANDLE;
            for (auto &written : mWritten) written = 0;
            for (auto &written : mCopiedWritten) written = 0;
        }
    private:
        ParticleGpuBudgets mBudgets;
        ParticleRenderer mRenderer;
        ParticleDataset mDataset;
        ParticleDispatch mDispatch;
        VkDevice mDevice = VK_NULL_HANDLE;
        VkQueryPool mQueries = VK_NULL_HANDLE;
        static constexpr uint64_t kTimingBytes=16*sizeof(uint64_t);
        struct Timing {VkBuffer buffer=VK_NULL_HANDLE;VmaAllocation memory=nullptr;const uint64_t *mapped=nullptr;};
        Timing mTiming[kFxFramesInFlight];
        VmaAllocator mAllocator=nullptr;
        bool mTimingComplete=false;
        uint32_t mBits = 0, mWritten[kFxFramesInFlight]{}, mCopiedWritten[kFxFramesInFlight]{};
        float mPeriod = 0;
        double mGpuMs[4]{}, mCpuStages[4]{}, mCpuMs = 0;
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

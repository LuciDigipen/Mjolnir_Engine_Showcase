/**
 * @file GpuTiming.h
 * @author Jackey Cen
 * @brief Optional GPU timestamp queries for the raster scene and RT trace passes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <volk.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace mir::vk
{
    class Device;

    enum class GpuTimingRegion : uint32_t
    {
        // The whole renderer submission: uploads, shadows/TLAS, scene/trace, post and UI.
        Frame,
        RasterScene,
        RtTraceRays,
        Count
    };

    const char *GpuTimingRegionName(GpuTimingRegion region);

    struct GpuTimingSample
    {
        uint64_t frame = 0;
        GpuTimingRegion region = GpuTimingRegion::RasterScene;
        uint32_t width = 0;
        uint32_t height = 0;
        double milliseconds = 0.0;
    };

    // Converts a hardware timestamp pair without waiting. Nullopt means the
    // value does not fit the queue's valid bits or the pair is not a forward
    // interval. The subtraction wraps with the hardware counter.
    std::optional<double> GpuTimestampMilliseconds(uint64_t begin, uint64_t end, uint32_t validBits,
        float nanosecondsPerTick);

    class GpuTiming
    {
    public:
        // A requested output path with unsupported timestamps still succeeds:
        // the timer disables itself and logs why.
        bool Initialize(Device &device, std::filesystem::path outputPath);
        void Shutdown();

        bool Enabled() const { return mEnabled; }

        // Polls every ready slot and resets this slot's queries in the frame's
        // command buffer. The caller has already waited for this frame slot
        // through FrameSync.
        void BeginFrame(VkCommandBuffer cmd, uint32_t frameIndex);

        // A frame that failed before submission has no valid results.
        void DiscardFrame(uint32_t frameIndex);

        void MarkBegin(VkCommandBuffer cmd, uint32_t frameIndex, GpuTimingRegion region);
        void MarkEnd(VkCommandBuffer cmd, uint32_t frameIndex, GpuTimingRegion region, VkExtent2D extent);

        // Polls available results without VK_QUERY_RESULT_WAIT_BIT.
        void Collect();

        // Waits for nothing; VulkanRenderer::Shutdown waits idle first.
        void Flush();

      private:
        static constexpr uint32_t kRegions = static_cast<uint32_t>(GpuTimingRegion::Count);
        static constexpr uint32_t kQueriesPerRegion = 2;
        static constexpr uint32_t kQueriesPerSlot = kRegions * kQueriesPerRegion;

        struct Slot
        {
            bool written[kRegions]{};
            uint64_t frame[kRegions]{};
            uint32_t width[kRegions]{};
            uint32_t height[kRegions]{};
            uint64_t serial = 0;
        };

        bool PollSlot(uint32_t frameIndex);
        uint32_t QueryIndex(uint32_t frameIndex, GpuTimingRegion region, uint32_t phase) const
        {
            return frameIndex * kQueriesPerSlot + static_cast<uint32_t>(region) * kQueriesPerRegion + phase;
        }

        Device *mDevice = nullptr;
        VkQueryPool mPool = VK_NULL_HANDLE;
        std::vector<Slot> mSlots;
        std::vector<GpuTimingSample> mSamples;
        std::filesystem::path mOutputPath;
        uint32_t mValidBits = 0;
        float mNanosecondsPerTick = 0.0f;
        uint64_t mFrameSerial = 0;
        uint64_t mAvailableCount = 0;
        uint64_t mUnavailableCount = 0;
        uint64_t mInvalidCount = 0;
        bool mEnabled = false;
    };
}

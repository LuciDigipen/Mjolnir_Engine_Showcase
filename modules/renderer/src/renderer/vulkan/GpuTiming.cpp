/**
 * @file GpuTiming.cpp
 * @author Jackey Cen
 * @brief Optional GPU timestamp queries for the raster scene and RT trace passes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "GpuTiming.h"
#include "Device.h"
#include "VulkanConstants.h"

#include <core/log/Log.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

namespace mir::vk
{
    const char *GpuTimingRegionName(GpuTimingRegion region)
    {
        switch (region)
        {
        case GpuTimingRegion::Frame:
            return "frame";
        case GpuTimingRegion::RasterScene:
            return "raster_scene";
        case GpuTimingRegion::RtTraceRays:
            return "rt_trace_rays";
        case GpuTimingRegion::Count:
            break;
        }
        return "unknown";
    }

    std::optional<double> GpuTimestampMilliseconds(uint64_t begin, uint64_t end, uint32_t validBits,
        float nanosecondsPerTick)
    {
        if (validBits == 0 || validBits > 64 || !(nanosecondsPerTick > 0.0f) || !std::isfinite(nanosecondsPerTick))
            return std::nullopt;

        const uint64_t validMask = validBits == 64 ? ~uint64_t{0} : ((uint64_t{1} << validBits) - 1);
        if ((begin & ~validMask) != 0 || (end & ~validMask) != 0)
            return std::nullopt;

        const uint64_t delta = (end - begin) & validMask;
        if (delta > (validMask >> 1))
            return std::nullopt;
        const double milliseconds = static_cast<double>(delta) * static_cast<double>(nanosecondsPerTick) / 1.0e6;
        if (!std::isfinite(milliseconds) || milliseconds < 0.0)
            return std::nullopt;
        return milliseconds;
    }

    bool GpuTiming::Initialize(Device &device, std::filesystem::path outputPath)
    {
        mDevice = &device;
        mOutputPath = std::move(outputPath);
        mSamples.clear();
        mFrameSerial = 0;
        mAvailableCount = 0;
        mUnavailableCount = 0;
        mInvalidCount = 0;
        mEnabled = false;

        const VkPhysicalDeviceLimits &limits = device.Properties().limits;
        if (!(limits.timestampPeriod > 0.0f) || !std::isfinite(limits.timestampPeriod))
        {
            MIR_LOG_WARN("GPU timing requested, but the device reports no timestamp period; timing is off");
            return true;
        }

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device.Physical(), &familyCount, nullptr);
        if (device.QueueFamily() >= familyCount)
        {
            MIR_LOG_WARN("GPU timing requested, but the render queue family has no timestamp support; timing is off");
            return true;
        }
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device.Physical(), &familyCount, families.data());
        mValidBits = families[device.QueueFamily()].timestampValidBits;
        if (mValidBits == 0)
        {
            MIR_LOG_WARN("GPU timing requested, but the render queue has zero timestamp valid bits; timing is off");
            return true;
        }
        mValidBits = std::min(mValidBits, 64u);
        mNanosecondsPerTick = limits.timestampPeriod;

        const VkQueryPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = kFramesInFlight * kQueriesPerSlot };
        if (vkCreateQueryPool(device.Handle(), &poolInfo, nullptr, &mPool) != VK_SUCCESS)
        {
            MIR_LOG_WARN("GPU timing requested, but the timestamp query pool could not be created; timing is off");
            mPool = VK_NULL_HANDLE;
            return true;
        }

        mSlots.assign(kFramesInFlight, Slot{});
        mEnabled = true;
        MIR_LOG_INFO("GPU timing: {} ticks per nanosecond, {} valid bits, output '{}'",
            1.0 / static_cast<double>(mNanosecondsPerTick), mValidBits, mOutputPath.string());
        return true;
    }

    void GpuTiming::Shutdown()
    {
        if (mDevice != nullptr && mPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(mDevice->Handle(), mPool, nullptr);
            mPool = VK_NULL_HANDLE;
        }
        mEnabled = false;
        mDevice = nullptr;
    }

    void GpuTiming::BeginFrame(VkCommandBuffer cmd, uint32_t frameIndex)
    {
        if (!mEnabled || frameIndex >= mSlots.size())
            return;
        Collect();
        vkCmdResetQueryPool(cmd, mPool, frameIndex * kQueriesPerSlot, kQueriesPerSlot);
        mSlots[frameIndex] = Slot{};
        mSlots[frameIndex].serial = ++mFrameSerial;
    }

    void GpuTiming::DiscardFrame(uint32_t frameIndex)
    {
        if (!mEnabled || frameIndex >= mSlots.size())
            return;
        for (bool &written : mSlots[frameIndex].written)
            written = false;
    }

    void GpuTiming::MarkBegin(VkCommandBuffer cmd, uint32_t frameIndex, GpuTimingRegion region)
    {
        if (!mEnabled || frameIndex >= mSlots.size() || region >= GpuTimingRegion::Count)
            return;
        Slot &slot = mSlots[frameIndex];
        slot.frame[static_cast<uint32_t>(region)] = slot.serial;
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mPool,
            QueryIndex(frameIndex, region, 0));
    }

    void GpuTiming::MarkEnd(VkCommandBuffer cmd, uint32_t frameIndex, GpuTimingRegion region, VkExtent2D extent)
    {
        if (!mEnabled || frameIndex >= mSlots.size() || region >= GpuTimingRegion::Count)
            return;
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mPool,
            QueryIndex(frameIndex, region, 1));
        Slot &slot = mSlots[frameIndex];
        slot.written[static_cast<uint32_t>(region)] = true;
        slot.width[static_cast<uint32_t>(region)] = extent.width;
        slot.height[static_cast<uint32_t>(region)] = extent.height;
    }

    void GpuTiming::Collect()
    {
        if (!mEnabled)
            return;
        for (uint32_t frame = 0; frame < mSlots.size(); ++frame)
            PollSlot(frame);
    }

    bool GpuTiming::PollSlot(uint32_t frameIndex)
    {
        Slot &slot = mSlots[frameIndex];
        bool collected = false;
        for (uint32_t region = 0; region < kRegions; ++region)
        {
            if (!slot.written[region])
                continue;

            std::array<uint64_t, kQueriesPerRegion * 2> results{};
            const VkResult result = vkGetQueryPoolResults(mDevice->Handle(), mPool,
                QueryIndex(frameIndex, static_cast<GpuTimingRegion>(region), 0), kQueriesPerRegion,
                sizeof(results), results.data(), sizeof(uint64_t) * 2,
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (result == VK_NOT_READY || results[1] == 0 || results[3] == 0)
            {
                ++mUnavailableCount;
                continue;
            }
            slot.written[region] = false;
            if (result != VK_SUCCESS)
            {
                ++mInvalidCount;
                continue;
            }

            const std::optional<double> milliseconds =
                GpuTimestampMilliseconds(results[0], results[2], mValidBits, mNanosecondsPerTick);
            if (!milliseconds)
            {
                ++mInvalidCount;
                continue;
            }

            ++mAvailableCount;
            mSamples.push_back({ .frame = slot.frame[region],
                                 .region = static_cast<GpuTimingRegion>(region),
                                 .width = slot.width[region],
                                 .height = slot.height[region],
                                 .milliseconds = *milliseconds });
            collected = true;
        }
        return collected;
    }

    void GpuTiming::Flush()
    {
        if (!mEnabled)
            return;
        Collect();

        std::error_code error;
        if (!mOutputPath.parent_path().empty())
            std::filesystem::create_directories(mOutputPath.parent_path(), error);

        std::ofstream output(mOutputPath, std::ios::trunc);
        if (!output)
        {
            MIR_LOG_ERROR("GPU timing: could not open '{}'", mOutputPath.string());
            return;
        }

        output << "frame,region,width,height,milliseconds\n";
        output << std::fixed << std::setprecision(6);
        for (const GpuTimingSample &sample : mSamples)
            output << sample.frame << ',' << GpuTimingRegionName(sample.region) << ',' << sample.width << ','
                   << sample.height << ',' << sample.milliseconds << '\n';

        for (uint32_t region = 0; region < kRegions; ++region)
        {
            std::vector<double> values;
            for (const GpuTimingSample &sample : mSamples)
                if (static_cast<uint32_t>(sample.region) == region)
                    values.push_back(sample.milliseconds);
            if (values.empty())
                continue;
            std::sort(values.begin(), values.end());
            const auto percentile = [&values](double fraction)
            {
                const size_t index = static_cast<size_t>(fraction * static_cast<double>(values.size() - 1));
                return values[std::min(index, values.size() - 1)];
            };
            const double median = values[values.size() / 2];
            output << "# summary," << GpuTimingRegionName(static_cast<GpuTimingRegion>(region)) << ',' << values.size()
                   << ",min=" << values.front() << ",median=" << median << ",p95=" << percentile(0.95)
                   << ",max=" << values.back() << '\n';
        }
        output << "# available=" << mAvailableCount << ",unavailable=" << mUnavailableCount
               << ",invalid=" << mInvalidCount << '\n';
        MIR_LOG_INFO("GPU timing: {} samples written to '{}' ({} unavailable polls, {} invalid)",
            mSamples.size(), mOutputPath.string(), mUnavailableCount, mInvalidCount);
    }
}

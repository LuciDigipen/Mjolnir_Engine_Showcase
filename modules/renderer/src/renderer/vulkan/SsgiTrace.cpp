/**
 * @file SsgiTrace.cpp
 * @author Rahul Nair
 * @brief Owns bounded screen-space tracing resources and dispatches.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "SsgiTrace.h"
#include "Device.h"
#include "RasterSurfaces.h"
#include "VulkanUtil.h"
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <algorithm>
#include <bit>

namespace mir::vk
{
    namespace
    {
        constexpr uint64_t kBudget = 256ull * 1024 * 1024;
        struct Push
        {
            uint32_t offset[2], size[2], output[2], mipCount, mipLevel;
            uint32_t rays, steps, seed, scale;
            float distance, thickness, nearPlane, pad;
        };
        static_assert(sizeof(Push) == 64);
        VkExtent2D MipExtent(VkExtent2D extent, uint32_t mip)
        { return { std::max(1u, extent.width >> mip), std::max(1u, extent.height >> mip) }; }
    }

    uint32_t SsgiTrace::MipCount(VkExtent2D extent)
    { return std::bit_width(std::max(extent.width, extent.height)); }

    VkExtent2D SsgiTrace::TraceExtent(VkExtent2D extent, bool half)
    { return half ? VkExtent2D{ extent.width / 2 + extent.width % 2, extent.height / 2 + extent.height % 2 } : extent; }

    bool SsgiTrace::FitsBudget(std::span<const VkRect2D> rects, bool half, const VkPhysicalDeviceLimits &limits)
    {
        if (rects.empty() || rects.size() > kMaxViewsPerFrame || limits.maxPushConstantsSize < sizeof(Push) ||
            limits.maxComputeWorkGroupInvocations < 64 || limits.maxComputeWorkGroupSize[0] < 8 ||
            limits.maxComputeWorkGroupSize[1] < 8 || limits.maxPerStageDescriptorSampledImages < 4 ||
            limits.maxPerStageDescriptorStorageImages < 2) return false;
        uint64_t bytes = 0;
        for (size_t i = 0; i < rects.size(); ++i)
        {
            const auto &r = rects[i];
            if (r.offset.x < 0 || r.offset.y < 0 || !r.extent.width || !r.extent.height ||
                r.extent.width > limits.maxImageDimension2D || r.extent.height > limits.maxImageDimension2D ||
                (r.extent.width / 8 + (r.extent.width % 8 != 0)) > limits.maxComputeWorkGroupCount[0] ||
                (r.extent.height / 8 + (r.extent.height % 8 != 0)) > limits.maxComputeWorkGroupCount[1]) return false;
            for (size_t j = 0; j < i; ++j)
            {
                const auto &s = rects[j];
                if (int64_t(r.offset.x) < int64_t(s.offset.x) + s.extent.width &&
                    int64_t(s.offset.x) < int64_t(r.offset.x) + r.extent.width &&
                    int64_t(r.offset.y) < int64_t(s.offset.y) + s.extent.height &&
                    int64_t(s.offset.y) < int64_t(r.offset.y) + r.extent.height) return false;
            }
            auto output = TraceExtent(r.extent, half);
            // Check each product before multiplying bytes, including hostile public API inputs.
            uint64_t pixels = uint64_t(output.width) * output.height;
            if (pixels > kBudget / (16 * kFramesInFlight)) return false;
            bytes += pixels * 16 * kFramesInFlight;
            for (uint32_t mip = 0; mip < MipCount(r.extent); ++mip)
            {
                auto e = MipExtent(r.extent, mip);
                pixels = uint64_t(e.width) * e.height;
                if (pixels > kBudget / (4 * kFramesInFlight)) return false;
                bytes += pixels * 4 * kFramesInFlight;
            }
            if (bytes > kBudget) return false;
        }
        return true;
    }

    void SsgiTrace::Initialize(Device &device, VkDescriptorSetLayout cameraLayout)
    { mDevice = &device; mCameraLayout = cameraLayout; }

    void SsgiTrace::Destroy()
    {
        if (!mDevice) return;
        auto dev = mDevice->Handle();
        for (auto &view : mViews)
            for (auto &frame : view.frames)
            {
                for (auto mip : frame.mips) vkDestroyImageView(dev, mip, nullptr);
                frame.mips.clear(); frame.sets.clear();
                DestroyImage2D(*mDevice, frame.pyramid); DestroyImage2D(*mDevice, frame.raw);
            }
        for (auto &pipeline : mPipelines) { vkDestroyPipeline(dev, pipeline, nullptr); pipeline = VK_NULL_HANDLE; }
        vkDestroyDescriptorPool(dev, mPool, nullptr); mPool = VK_NULL_HANDLE;
        vkDestroyPipelineLayout(dev, mLayout, nullptr); mLayout = VK_NULL_HANDLE;
        vkDestroyDescriptorSetLayout(dev, mSetLayout, nullptr); mSetLayout = VK_NULL_HANDLE;
    }

    void SsgiTrace::Shutdown() { Destroy(); mViews.clear(); mFailed = false; }

    void SsgiTrace::Configure(bool requested, VkExtent2D extent, std::span<const RenderView> views, bool half)
    {
        if (!requested && mViews.empty()) return;
        std::vector<VkRect2D> rects;
        if (requested)
            for (const auto &view : views.first(std::min(views.size(), size_t(kMaxViewsPerFrame))))
                rects.push_back(ResolveViewRect(view.rect, extent));
        bool changed = half != mHalfResolution || rects.size() != mViews.size();
        for (size_t i = 0; !changed && i < rects.size(); ++i)
            changed = rects[i].extent.width != mViews[i].rect.extent.width ||
                rects[i].extent.height != mViews[i].rect.extent.height ||
                rects[i].offset.x != mViews[i].rect.offset.x || rects[i].offset.y != mViews[i].rect.offset.y;
        if (changed || !requested)
        {
            if (Active()) mDevice->WaitIdle();
            Destroy(); mViews.clear(); mFailed = false; mHalfResolution = half;
            for (const auto &rect : rects)
            {
                View view{}; view.rect = rect; view.output = TraceExtent(rect.extent, half);
                mViews.push_back(std::move(view));
            }
        }
        if (!requested || rects.empty() || Active() || mFailed) return;
        if (!FitsBudget(rects, half, mDevice->Properties().limits) || !Create())
        {
            Destroy(); mFailed = true;
            MIR_LOG_WARN("SSGI tracing unavailable for this view layout; preserving existing raster lighting");
        }
    }

    bool SsgiTrace::Create()
    {
        auto dev = mDevice->Handle();
        for (auto format : { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT })
        {
            VkFormatProperties p{}; vkGetPhysicalDeviceFormatProperties(mDevice->Physical(), format, &p);
            constexpr auto flags = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
            if ((p.optimalTilingFeatures & flags) != flags) return false;
        }
        const char *files[]{ "SsgiDepth.initMain.spv", "SsgiDepth.reduceMain.spv", "SsgiTrace.traceMain.spv" };
        for (auto file : files)
        {
            std::error_code error;
            if (!std::filesystem::is_regular_file(ShaderPath(file), error)) return false;
        }
        std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
        for (uint32_t i = 0; i < bindings.size(); ++i)
            bindings[i] = { i, i == 2 || i == 5 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        const VkDescriptorSetLayoutCreateInfo setInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = uint32_t(bindings.size()), .pBindings = bindings.data() };
        if (vkCreateDescriptorSetLayout(dev, &setInfo, nullptr, &mSetLayout) != VK_SUCCESS) return false;
        VkDescriptorSetLayout layouts[]{ mSetLayout, mCameraLayout };
        const VkPushConstantRange push{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push) };
        const VkPipelineLayoutCreateInfo layoutInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 2, .pSetLayouts = layouts, .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
        if (vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &mLayout) != VK_SUCCESS) return false;
        const char *entries[]{ "initMain", "reduceMain", "traceMain" };
        for (uint32_t i = 0; i < 3; ++i)
        {
            auto module = CreateShaderModule(dev, ShaderPath(files[i]));
            if (!module) return false;
            const VkComputePipelineCreateInfo info{ .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = entries[i] }, .layout = mLayout };
            auto result = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &mPipelines[i]);
            vkDestroyShaderModule(dev, module, nullptr);
            if (result != VK_SUCCESS) return false;
        }
        uint32_t count = 0;
        for (const auto &view : mViews) count += (MipCount(view.rect.extent) + 1) * kFramesInFlight;
        const VkDescriptorPoolSize sizes[]{ { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count * 4 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count * 2 } };
        const VkDescriptorPoolCreateInfo poolInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = count, .poolSizeCount = 2, .pPoolSizes = sizes };
        if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &mPool) != VK_SUCCESS) return false;
        uint64_t allocated = 0;
        const auto image = [&](Image2D &out, VkFormat format, VkExtent2D extent, uint32_t mips)
        {
            const VkImageCreateInfo info{ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                .format = format, .extent{ extent.width, extent.height, 1 }, .mipLevels = mips, .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
            const VmaAllocationCreateInfo alloc{ .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };
            if (vmaCreateImage(mDevice->Allocator(), &info, &alloc, &out.image, &out.allocation, nullptr) != VK_SUCCESS) return false;
            VmaAllocationInfo allocation{}; vmaGetAllocationInfo(mDevice->Allocator(), out.allocation, &allocation);
            allocated += allocation.size;
            if (allocated > kBudget) return false;
            const VkImageViewCreateInfo v{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = out.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                .subresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 } };
            return vkCreateImageView(dev, &v, nullptr, &out.view) == VK_SUCCESS;
        };
        for (auto &view : mViews)
            for (auto &frame : view.frames)
            {
                auto mips = MipCount(view.rect.extent);
                if (!image(frame.pyramid, VK_FORMAT_R32_SFLOAT, view.rect.extent, mips) ||
                    !image(frame.raw, VK_FORMAT_R32G32B32A32_SFLOAT, view.output, 1)) return false;
                for (uint32_t mip = 0; mip < mips; ++mip)
                {
                    const VkImageViewCreateInfo info{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                        .image = frame.pyramid.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32_SFLOAT,
                        .subresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1 } };
                    VkImageView v{};
                    if (vkCreateImageView(dev, &info, nullptr, &v) != VK_SUCCESS) return false;
                    frame.mips.push_back(v);
                }
                frame.sets.resize(mips + 1);
                for (auto &set : frame.sets)
                    if (!AllocateDescriptorSet(dev, mPool, mSetLayout, set)) return false;
            }
        return true;
    }

    void SsgiTrace::Record(VkCommandBuffer cmd, uint32_t frameIndex, VkImage depth, VkImageView depthView,
        const RasterSurfaces &surfaces, VkDescriptorSet cameraSet, std::span<const uint32_t> offsets,
        const SsgiSettings &settings)
    {
        if (!Active() || offsets.size() < mViews.size()) return;
        constexpr auto tests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        constexpr auto depthAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        ImageBarrier(cmd, depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, tests, depthAccess,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        for (size_t v = 0; v < mViews.size(); ++v)
        {
            auto &view = mViews[v]; auto &frame = view.frames[frameIndex];
            auto mips = uint32_t(frame.mips.size());
            ImageBarrier(cmd, frame.pyramid.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, mips);
            ImageBarrier(cmd, frame.raw.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            Push push{ { uint32_t(view.rect.offset.x), uint32_t(view.rect.offset.y) },
                { view.rect.extent.width, view.rect.extent.height }, {}, mips, 0,
                settings.RaysPerPixel(), settings.MaxRaySteps(), mSeed, mHalfResolution ? 2u : 1u,
                settings.maxTraceDistance, settings.thickness, 0, 0 };
            for (uint32_t pass = 0; pass <= mips; ++pass)
            {
                const bool trace = pass == mips;
                auto e = trace ? view.output : MipExtent(view.rect.extent, pass);
                push.output[0] = e.width; push.output[1] = e.height; push.mipLevel = pass;
                const VkDescriptorImageInfo images[]{
                    { {}, depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                    { {}, trace ? frame.pyramid.view : frame.mips[pass ? pass - 1 : 0], VK_IMAGE_LAYOUT_GENERAL },
                    { {}, frame.mips[trace ? 0 : pass], VK_IMAGE_LAYOUT_GENERAL },
                    { {}, surfaces.Image(RasterSurface::NormalKind, frameIndex).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                    { {}, surfaces.Image(RasterSurface::DiffuseRadiance, frameIndex).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                    { {}, frame.raw.view, VK_IMAGE_LAYOUT_GENERAL } };
                std::array<VkWriteDescriptorSet, 6> writes{};
                for (uint32_t i = 0; i < 6; ++i)
                    writes[i] = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = frame.sets[pass],
                        .dstBinding = i, .descriptorCount = 1,
                        .descriptorType = i == 2 || i == 5 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                        .pImageInfo = &images[i] };
                vkUpdateDescriptorSets(mDevice->Handle(), uint32_t(writes.size()), writes.data(), 0, nullptr);
                VkDescriptorSet sets[]{ frame.sets[pass], cameraSet };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 2, sets, 1, &offsets[v]);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipelines[trace ? 2 : pass ? 1 : 0]);
                vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
                vkCmdDispatch(cmd, (e.width + 7) / 8, (e.height + 7) / 8, 1);
                if (!trace)
                    ImageBarrier(cmd, frame.pyramid.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, mips);
            }
            ImageBarrier(cmd, frame.raw.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        ImageBarrier(cmd, depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, tests, depthAccess);
        ++mSeed;
    }

}

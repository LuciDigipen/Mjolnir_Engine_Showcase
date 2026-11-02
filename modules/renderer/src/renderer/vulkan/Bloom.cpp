/**
 * @file Bloom.cpp
 * @author Rahul Nair
 * @brief Bloom: a bright-pass extract, a downsample chain and an additive
 *        upsample chain, run on whatever linear-HDR source the raster or ray
 *        traced pipeline hands it, ahead of PostProcess's tonemap pass.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Bloom.h"
#include "Device.h"
#include "VulkanPushConstants.h"
#include "VulkanUtil.h"

#include <core/fs/Paths.h>
#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <format>
#include <vector>

namespace mir::vk
{
    namespace
    {
        constexpr uint32_t kSetsPerFrame = 1 + 2 * (kBloomMipLevels - 1);
    }

    bool Bloom::Initialize(Device &device, VkExtent2D extent)
    {
        mDevice = &device;

        const VkSamplerCreateInfo samplerInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .maxLod = 0.0f,
            .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK };

        if (vkCreateSampler(mDevice->Handle(), &samplerInfo, nullptr, &mSampler) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateSampler failed");
            return false;
        }

        const VkDescriptorSetLayoutBinding binding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };

        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &binding };

        if (vkCreateDescriptorSetLayout(mDevice->Handle(), &setLayoutInfo, nullptr, &mSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateDescriptorSetLayout failed");
            return false;
        }

        constexpr uint32_t kTotalSets = kFramesInFlight * kSetsPerFrame;
        const VkDescriptorPoolSize poolSize{
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = kTotalSets };

        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kTotalSets,
            .poolSizeCount = 1,
            .pPoolSizes = &poolSize };

        if (vkCreateDescriptorPool(mDevice->Handle(), &poolInfo, nullptr, &mPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateDescriptorPool failed");
            return false;
        }

        const auto allocate = [&](VkDescriptorSet &outSet)
            { return AllocateDescriptorSet(mDevice->Handle(), mPool, mSetLayout, outSet); };

        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        {
            if (!allocate(mExtractSets[frame]))
            {
                MIR_LOG_CRITICAL("Initialize: extract descriptor set allocation failed for frame {}", frame);
                return false;
            }

            for (uint32_t i = 0; i < kBloomMipLevels - 1; ++i)
            {
                if (!allocate(mDownsampleSets[frame][i]) || !allocate(mUpsampleSets[frame][i]))
                {
                    MIR_LOG_CRITICAL("Initialize: chain descriptor set allocation failed for frame {}", frame);
                    return false;
                }
            }
        }

        if (!CreatePipelines() || !CreateMipChain(extent))
            return false;

        return true;
    }

    bool Bloom::CreatePipelines()
    {
        VkShaderModule vertModule = CreateShaderModule(mDevice->Handle(), ShaderPath("bloom.vertexMain.spv"));
        VkShaderModule extractModule = CreateShaderModule(mDevice->Handle(), ShaderPath("bloom.brightpassMain.spv"));
        VkShaderModule downsampleModule = CreateShaderModule(mDevice->Handle(), ShaderPath("bloom.downsampleMain.spv"));
        VkShaderModule upsampleModule = CreateShaderModule(mDevice->Handle(), ShaderPath("bloom.upsampleMain.spv"));

        const bool allLoaded = vertModule != VK_NULL_HANDLE && extractModule != VK_NULL_HANDLE &&
            downsampleModule != VK_NULL_HANDLE && upsampleModule != VK_NULL_HANDLE;

        bool ok = allLoaded;

        if (ok)
        {
            constexpr uint32_t kPushConstantSize = static_cast<uint32_t>(sizeof(BloomExtractPushConstants));
            static_assert(sizeof(BloomDownsamplePushConstants) <= kPushConstantSize);
            static_assert(sizeof(BloomUpsamplePushConstants) <= kPushConstantSize);

            const VkPushConstantRange pushConstantRange{
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .offset = 0,
                .size = kPushConstantSize };

            const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1,
                .pSetLayouts = &mSetLayout,
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &pushConstantRange };

            if (vkCreatePipelineLayout(mDevice->Handle(), &pipelineLayoutInfo, nullptr, &mPipelineLayout) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreatePipelines: vkCreatePipelineLayout failed");
                ok = false;
            }
        }

        if (ok)
        {
            const VkPipelineColorBlendAttachmentState overwriteBlend{
                .blendEnable = VK_FALSE,
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };

            // Upsample blends the smaller, already-blurred level back onto
            // the level it's writing into instead of overwriting it.
            const VkPipelineColorBlendAttachmentState additiveBlend{
                .blendEnable = VK_TRUE,
                .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
                .colorBlendOp = VK_BLEND_OP_ADD,
                .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                .alphaBlendOp = VK_BLEND_OP_ADD,
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };

            const auto makePipeline = [&](VkShaderModule fragModule, const char *fragEntry,
                const VkPipelineColorBlendAttachmentState &blendAttachment, const char *debugName, VkPipeline &outPipeline)
                {
                    const FullscreenPipelineDesc desc{
                        .vertModule = vertModule,
                        .fragModule = fragModule,
                        .fragEntry = fragEntry,
                        .layout = mPipelineLayout,
                        .blendAttachment = &blendAttachment,
                        .colorFormat = kHdrColorFormat };
                    return CreateFullscreenPipeline(*mDevice, desc, debugName, outPipeline);
                };

            ok = makePipeline(extractModule, "brightpassMain", overwriteBlend, "Bloom extract pipeline", mExtractPipeline);
            if (ok)
                ok = makePipeline(downsampleModule, "downsampleMain", overwriteBlend, "Bloom downsample pipeline",
                    mDownsamplePipeline);
            if (ok)
                ok = makePipeline(upsampleModule, "upsampleMain", additiveBlend, "Bloom upsample pipeline",
                    mUpsamplePipeline);
        }

        vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), extractModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), downsampleModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), upsampleModule, nullptr);

        return ok;
    }

    bool Bloom::CreateMipChain(VkExtent2D extent)
    {
        VkExtent2D levelExtent{ extent.width, extent.height };

        for (uint32_t level = 0; level < kBloomMipLevels; ++level)
        {
            levelExtent.width = std::max(1u, levelExtent.width / 2);
            levelExtent.height = std::max(1u, levelExtent.height / 2);
            mMipExtents[level] = levelExtent;

            for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
            {
                const VkImageCreateInfo imageInfo{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .imageType = VK_IMAGE_TYPE_2D,
                    .format = kHdrColorFormat,
                    .extent{ levelExtent.width, levelExtent.height, 1 },
                    .mipLevels = 1,
                    .arrayLayers = 1,
                    .samples = VK_SAMPLE_COUNT_1_BIT,
                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

                const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };

                if (vmaCreateImage(mDevice->Allocator(), &imageInfo, &allocInfo,
                    &mMipImages[frame][level], &mMipAllocations[frame][level], nullptr) != VK_SUCCESS)
                {
                    MIR_LOG_CRITICAL("CreateMipChain: vmaCreateImage failed for level {} frame {}", level, frame);
                    return false;
                }

                const VkImageViewCreateInfo viewInfo{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                    .image = mMipImages[frame][level],
                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                    .format = kHdrColorFormat,
                    .subresourceRange{
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0,
                        .levelCount = 1,
                        .baseArrayLayer = 0,
                        .layerCount = 1 } };

                if (vkCreateImageView(mDevice->Handle(), &viewInfo, nullptr, &mMipViews[frame][level]) != VK_SUCCESS)
                {
                    MIR_LOG_CRITICAL("CreateMipChain: vkCreateImageView failed for level {} frame {}", level, frame);
                    return false;
                }

                mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mMipImages[frame][level]),
                    std::format("Bloom mip {} ({})", level, frame).c_str());
            }
        }

        const bool transitioned = SubmitAndWait(*mDevice, [this](VkCommandBuffer cmd)
            {
                for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
                    for (uint32_t level = 0; level < kBloomMipLevels; ++level)
                        ImageBarrier(cmd, mMipImages[frame][level], VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            });

        if (!transitioned)
        {
            MIR_LOG_CRITICAL("CreateMipChain: initial layout transition failed");
            return false;
        }

        WriteStaticDescriptors();
        return true;
    }

    void Bloom::DestroyMipChain()
    {
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        {
            for (uint32_t level = 0; level < kBloomMipLevels; ++level)
            {
                if (mMipViews[frame][level] != VK_NULL_HANDLE)
                {
                    vkDestroyImageView(mDevice->Handle(), mMipViews[frame][level], nullptr);
                    mMipViews[frame][level] = VK_NULL_HANDLE;
                }
                if (mMipImages[frame][level] != VK_NULL_HANDLE)
                {
                    vmaDestroyImage(mDevice->Allocator(), mMipImages[frame][level], mMipAllocations[frame][level]);
                    mMipImages[frame][level] = VK_NULL_HANDLE;
                    mMipAllocations[frame][level] = VK_NULL_HANDLE;
                }
            }
        }
    }

    void Bloom::WriteStaticDescriptors()
    {
        std::vector<VkDescriptorImageInfo> imageInfos;
        std::vector<VkWriteDescriptorSet> writes;
        imageInfos.reserve(kFramesInFlight * 2 * (kBloomMipLevels - 1));
        writes.reserve(imageInfos.capacity());

        const auto addWrite = [&](VkDescriptorSet set, VkImageView view)
            {
                imageInfos.push_back(VkDescriptorImageInfo{
                    .sampler = mSampler, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
                writes.push_back(VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = set,
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &imageInfos.back() });
            };

        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        {
            for (uint32_t level = 1; level < kBloomMipLevels; ++level)
                addWrite(mDownsampleSets[frame][level - 1], mMipViews[frame][level - 1]);

            for (uint32_t level = 0; level + 1 < kBloomMipLevels; ++level)
                addWrite(mUpsampleSets[frame][level], mMipViews[frame][level + 1]);
        }

        vkUpdateDescriptorSets(mDevice->Handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    bool Bloom::Resize(VkExtent2D extent)
    {
        DestroyMipChain();
        return CreateMipChain(extent);
    }

    void Bloom::DestroyPipelines()
    {
        if (mExtractPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice->Handle(), mExtractPipeline, nullptr);
            mExtractPipeline = VK_NULL_HANDLE;
        }
        if (mDownsamplePipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice->Handle(), mDownsamplePipeline, nullptr);
            mDownsamplePipeline = VK_NULL_HANDLE;
        }
        if (mUpsamplePipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice->Handle(), mUpsamplePipeline, nullptr);
            mUpsamplePipeline = VK_NULL_HANDLE;
        }
        if (mPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(mDevice->Handle(), mPipelineLayout, nullptr);
            mPipelineLayout = VK_NULL_HANDLE;
        }
    }

    void Bloom::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        DestroyPipelines();
        DestroyMipChain();

        // Freeing the pool frees every set above too.
        if (mPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(mDevice->Handle(), mPool, nullptr);
            mPool = VK_NULL_HANDLE;
        }

        if (mSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice->Handle(), mSetLayout, nullptr);
            mSetLayout = VK_NULL_HANDLE;
        }

        if (mSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(mDevice->Handle(), mSampler, nullptr);
            mSampler = VK_NULL_HANDLE;
        }
    }

    void Bloom::Record(VkCommandBuffer cmd, uint32_t frame, VkImageView source, VkExtent2D extent) const
    {
        const VkDescriptorImageInfo sourceImageInfo{
            .sampler = mSampler, .imageView = source, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkWriteDescriptorSet sourceWrite{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = mExtractSets[frame],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &sourceImageInfo };
        vkUpdateDescriptorSets(mDevice->Handle(), 1, &sourceWrite, 0, nullptr);

        const auto drawFullscreen = [&](VkExtent2D target, VkPipeline pipeline, VkDescriptorSet set,
            const void *pushConstants, uint32_t pushConstantSize)
            {
                const VkViewport viewport{
                    .x = 0.0f, .y = 0.0f,
                    .width = static_cast<float>(target.width), .height = static_cast<float>(target.height),
                    .minDepth = 0.0f, .maxDepth = 1.0f };
                const VkRect2D scissor{ .offset{ 0, 0 }, .extent = target };
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 0, 1, &set, 0, nullptr);
                vkCmdPushConstants(cmd, mPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushConstantSize, pushConstants);

                vkCmdDraw(cmd, 3, 1, 0, 0);
            };

        for (uint32_t level = 0; level < kBloomMipLevels; ++level)
        {
            ColorAttachmentBarrier(cmd, mMipImages[frame][level], /*firstUse*/ true);

            const VkRenderingAttachmentInfo attachment{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = mMipViews[frame][level],
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
            const VkRenderingInfo renderingInfo{
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea{ .offset{ 0, 0 }, .extent = mMipExtents[level] },
                .layerCount = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments = &attachment };
            vkCmdBeginRendering(cmd, &renderingInfo);

            if (level == 0)
            {
                const BloomExtractPushConstants push{
                    mThreshold, mThreshold * 0.5f, InvExtent(extent), InvExtent(mMipExtents[0]) };
                drawFullscreen(mMipExtents[0], mExtractPipeline, mExtractSets[frame], &push, sizeof(push));
            }
            else
            {
                const BloomDownsamplePushConstants push{ InvExtent(mMipExtents[level - 1]), InvExtent(mMipExtents[level]) };
                drawFullscreen(mMipExtents[level], mDownsamplePipeline, mDownsampleSets[frame][level - 1],
                    &push, sizeof(push));
            }

            vkCmdEndRendering(cmd);
            ShaderReadOnlyBarrier(cmd, mMipImages[frame][level]);
        }

        for (uint32_t levelPlusOne = kBloomMipLevels - 1; levelPlusOne > 0; --levelPlusOne)
        {
            const uint32_t level = levelPlusOne - 1;

            ColorAttachmentBarrier(cmd, mMipImages[frame][level], /*firstUse*/ false);

            const VkRenderingAttachmentInfo attachment{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = mMipViews[frame][level],
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                // Loaded, not cleared: the downsample pass's content underneath is what gets blended onto.
                .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
            const VkRenderingInfo renderingInfo{
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea{ .offset{ 0, 0 }, .extent = mMipExtents[level] },
                .layerCount = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments = &attachment };
            vkCmdBeginRendering(cmd, &renderingInfo);

            const BloomUpsamplePushConstants push{
                InvExtent(mMipExtents[level + 1]), InvExtent(mMipExtents[level]), mRadius };
            drawFullscreen(mMipExtents[level], mUpsamplePipeline, mUpsampleSets[frame][level], &push, sizeof(push));

            vkCmdEndRendering(cmd);
            ShaderReadOnlyBarrier(cmd, mMipImages[frame][level]);
        }
    }
    bool Bloom::BuildReloadCandidate(ReloadCandidate &candidate, std::string *error)
    {
        VkShaderModule vertModule = CreateShaderModule(mDevice->Handle(), ShaderPath("bloom.vertexMain.spv"));
        VkShaderModule extractModule = CreateShaderModule(mDevice->Handle(), ShaderPath("bloom.brightpassMain.spv"));
        VkShaderModule downsampleModule = CreateShaderModule(mDevice->Handle(), ShaderPath("bloom.downsampleMain.spv"));
        VkShaderModule upsampleModule = CreateShaderModule(mDevice->Handle(), ShaderPath("bloom.upsampleMain.spv"));
        if (vertModule == VK_NULL_HANDLE || extractModule == VK_NULL_HANDLE ||
            downsampleModule == VK_NULL_HANDLE || upsampleModule == VK_NULL_HANDLE)
        {
            if (vertModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
            if (extractModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), extractModule, nullptr);
            if (downsampleModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), downsampleModule, nullptr);
            if (upsampleModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), upsampleModule, nullptr);
            if (error != nullptr)
                *error = "a bloom shader module is missing";
            return false;
        }

        const VkPipelineColorBlendAttachmentState overwriteBlend{
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
        const VkPipelineColorBlendAttachmentState additiveBlend{
            .blendEnable = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };

        const auto makePipeline = [&](VkShaderModule fragModule, const char *fragEntry,
            const VkPipelineColorBlendAttachmentState &blendAttachment, const char *debugName,
            VkPipeline &outPipeline)
            {
                const FullscreenPipelineDesc desc{
                    .vertModule = vertModule,
                    .fragModule = fragModule,
                    .fragEntry = fragEntry,
                    .layout = mPipelineLayout,
                    .blendAttachment = &blendAttachment,
                    .colorFormat = kHdrColorFormat };
                return CreateFullscreenPipeline(*mDevice, desc, debugName, outPipeline);
            };

        bool ok = makePipeline(extractModule, "brightpassMain", overwriteBlend, "Bloom extract pipeline",
            candidate.mExtract);
        if (ok)
            ok = makePipeline(downsampleModule, "downsampleMain", overwriteBlend,
                "Bloom downsample pipeline", candidate.mDownsample);
        if (ok)
            ok = makePipeline(upsampleModule, "upsampleMain", additiveBlend,
                "Bloom upsample pipeline", candidate.mUpsample);

        vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), extractModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), downsampleModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), upsampleModule, nullptr);
        if (!ok && error != nullptr)
            *error = "a bloom pipeline could not be created";
        return ok;
    }

    void Bloom::DestroyCandidate(ReloadCandidate &candidate)
    {
        if (mDevice == nullptr)
            return;
        for (VkPipeline *pipeline : { &candidate.mExtract, &candidate.mDownsample, &candidate.mUpsample })
        {
            if (*pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(mDevice->Handle(), *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
    }

    bool Bloom::PrepareShaderReload(std::string *error)
    {
        DiscardShaderReload();
        if (!BuildReloadCandidate(mReloadCandidate, error))
        {
            DiscardShaderReload();
            return false;
        }
        mReloadPrepared = true;
        return true;
    }

    void Bloom::CommitShaderReload()
    {
        if (!mReloadPrepared)
            return;
        for (VkPipeline *pipeline : { &mExtractPipeline, &mDownsamplePipeline, &mUpsamplePipeline })
            if (*pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(mDevice->Handle(), *pipeline, nullptr);
        mExtractPipeline = mReloadCandidate.mExtract;
        mDownsamplePipeline = mReloadCandidate.mDownsample;
        mUpsamplePipeline = mReloadCandidate.mUpsample;
        mReloadCandidate = {};
        mReloadPrepared = false;
    }

    void Bloom::DiscardShaderReload()
    {
        DestroyCandidate(mReloadCandidate);
        mReloadPrepared = false;
    }
}

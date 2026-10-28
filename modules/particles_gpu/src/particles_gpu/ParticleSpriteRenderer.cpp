/**
 * @file ParticleSpriteRenderer.cpp
 * @author sumin.park
 * @brief Indirect sprite rendering through the renderer-owned composition hook.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ParticleSpriteRenderer.h"
#include "ParticleDataset.h"
#include "ParticleVulkan.h"

#include <core/log/SetupError.h>
#include <renderer/VulkanRenderer.h>

#include <glm/matrix.hpp>

#include <cstring>
#include <set>

namespace mir::gpu
{
    namespace
    {
        [[noreturn]] void TextureFailure(const char *cause)
        {
            throw SetupError(ErrorCategory::BuildOutput, "Prepare particle texture", "cooked particle texture",
                cause, "Recook the texture and keep particle texture data within the 64 MiB resident/upload budget.");
        }
    }

    void ParticleSpriteRenderer::DestroyTexture(Texture &texture)
    {
        if (texture.staging) vmaDestroyBuffer(mAllocator, texture.staging, texture.stagingAllocation);
        if (texture.view) vkDestroyImageView(mDevice, texture.view, nullptr);
        if (texture.image) vmaDestroyImage(mAllocator, texture.image, texture.allocation);
        mTextureBytes -= texture.bytes;
        texture = {};
    }

    void ParticleSpriteRenderer::UploadTexture(const FxFrameContext &context, Texture &texture,
        uint32_t width, uint32_t height, const uint8_t *pixels)
    {
        const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
        // Reserve both resident pixels and their in-flight upload until retirement.
        if (mTextureBytes + bytes * 2 > 64 * 1024 * 1024)
            TextureFailure("Particle texture memory budget exceeded");
        const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        const VmaAllocationCreateInfo memory{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(mAllocator, &image, &memory, &texture.image, &texture.allocation, nullptr) != VK_SUCCESS)
            TextureFailure("Cannot allocate particle texture image");
        texture.bytes = bytes;
        mTextureBytes += bytes;
        const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = texture.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(mDevice, &view, nullptr, &texture.view) != VK_SUCCESS)
            TextureFailure("Cannot create particle texture view");
        VmaAllocationInfo upload{};
        if (!CreateParticleBuffer(mAllocator, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                texture.staging, texture.stagingAllocation, upload))
            TextureFailure("Cannot allocate particle texture upload");
        texture.bytes += bytes;
        mTextureBytes += bytes;
        std::memcpy(upload.pMappedData, pixels, static_cast<size_t>(bytes));
        vmaFlushAllocation(mAllocator, texture.stagingAllocation, 0, VK_WHOLE_SIZE);
        VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
        vkCmdPipelineBarrier2(context.commands, &dependency);
        const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {width, height, 1}};
        vkCmdCopyBufferToImage(context.commands, texture.staging, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(context.commands, &dependency);
        texture.uploaded = context.signalValue;
    }

    void ParticleSpriteRenderer::PrepareTextures(const FxFrameContext &context, const ParticleDataset &data)
    {
        if (!data.Initialized()) return;
        mDevice = context.device.device;
        mAllocator = context.device.allocator;
        const auto completed = mSignals[context.frame];
        std::set<TextureKey> used;
        for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
        {
            const auto &slot = data.mSlots[i];
            if (!slot.allocated) continue;
            const auto &input = slot.input;
            TextureKey key{};
            if (input.textureKey || !input.texturePixels.empty())
            {
                if (!input.textureKey || !input.textureWidth || !input.textureHeight ||
                    input.textureWidth > 2048 || input.textureHeight > 2048 ||
                    input.texturePixels.size() != size_t(input.textureWidth) * input.textureHeight * 4)
                    TextureFailure("Malformed particle RGBA8 texture or dimensions beyond 2048");
                uint64_t fingerprint = 14695981039346656037ull;
                for (const auto byte : input.texturePixels) { fingerprint ^= byte; fingerprint *= 1099511628211ull; }
                fingerprint ^= uint64_t(input.textureWidth) << 32 | input.textureHeight;
                key = {input.textureKey, fingerprint};
            }
            mSlotTextures[i] = key;
            used.insert(key);
        }
        for (auto it = mTextures.begin(); it != mTextures.end();)
        {
            auto &texture = it->second;
            if (!used.contains(it->first) && texture.lastUse <= completed)
            {
                DestroyTexture(texture);
                it = mTextures.erase(it);
                continue;
            }
            if (texture.staging && texture.uploaded <= completed)
            {
                vmaDestroyBuffer(mAllocator, texture.staging, texture.stagingAllocation);
                texture.staging = VK_NULL_HANDLE;
                texture.stagingAllocation = nullptr;
                mTextureBytes -= texture.bytes / 2;
                texture.bytes /= 2;
            }
            ++it;
        }
        for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
        {
            const auto &slot = data.mSlots[i];
            if (!slot.allocated) continue;
            const auto key = mSlotTextures[i];
            auto [entry, fresh] = mTextures.try_emplace(key);
            if (fresh)
            {
                const uint8_t white[]{255, 255, 255, 255};
                UploadTexture(context, entry->second,
                    key.first ? slot.input.textureWidth : 1, key.first ? slot.input.textureHeight : 1,
                    key.first ? slot.input.texturePixels.data() : white);
            }
            entry->second.lastUse = context.signalValue;
        }
        mSignals[context.frame] = context.signalValue;
    }

    bool ParticleSpriteRenderer::Initialize(const FxRenderContext &context, const ParticleDataset &data)
    {
        mDevice = context.frame.device.device;
        const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        if (vkCreateSampler(mDevice, &sampler, nullptr, &mSampler) != VK_SUCCESS) return false;
        const VkDescriptorSetLayoutBinding bindings[]{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        const VkDescriptorSetLayoutCreateInfo set{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 4, .pBindings = bindings};
        if (vkCreateDescriptorSetLayout(mDevice, &set, nullptr, &mSetLayout) != VK_SUCCESS) return false;
        const VkDescriptorPoolSize sizes[]{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * kFxFramesInFlight * kGpuParticleInstances},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFxFramesInFlight * kGpuParticleInstances}};
        const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kFxFramesInFlight * kGpuParticleInstances, .poolSizeCount = 2, .pPoolSizes = sizes};
        if (vkCreateDescriptorPool(mDevice, &pool, nullptr, &mPool) != VK_SUCCESS) return false;
        for (uint32_t f = 0; f < kFxFramesInFlight; ++f)
        {
            for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
            {
                const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                    .descriptorPool = mPool, .descriptorSetCount = 1, .pSetLayouts = &mSetLayout};
                if (vkAllocateDescriptorSets(mDevice, &allocate, &mSets[f][i]) != VK_SUCCESS) return false;
                const VkDescriptorBufferInfo buffer{data.mParticles.handle, 0, VK_WHOLE_SIZE};
                const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mSets[f][i],
                    .dstBinding = 5, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffer};
                vkUpdateDescriptorSets(mDevice, 1, &write, 0, nullptr);
            }
        }
        const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
        const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &mSetLayout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
        return vkCreatePipelineLayout(mDevice, &layout, nullptr, &mLayout) == VK_SUCCESS &&
            CreateSpritePipeline(mDevice, mLayout, context.colorFormat, mPipeline);
    }
    void ParticleSpriteRenderer::Record(const FxRenderContext &context, const ParticleDataset &data)
    {
        if (!data.Initialized() || context.views.empty()) return;
        if (!mLayout && !Initialize(context, data))
            throw SetupError(ErrorCategory::System, "Initialize particle sprite renderer", "particlePool",
                "Vulkan graphics pipeline creation failed", "Rebuild shaders and inspect the GPU driver log.");
        const auto cmd = context.frame.commands;
        const auto frame = context.frame.frame;
        const auto color = context.sceneColor;
        const auto depth = context.depth;
        const auto extent = context.extent;
        const auto views = context.views;
        const auto jitter = context.jitter;
        const auto rayDistance = context.rayDistance;
        for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
        {
            if (!data.mSlots[i].allocated) continue;
            const auto found = mTextures.find(mSlotTextures[i]);
            if (found == mTextures.end()) TextureFailure("Particle texture was not prepared before rendering");
            const VkDescriptorImageInfo images[]{
                {mSampler, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {mSampler, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {mSampler, found->second.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            const uint32_t bindings[]{0, 1, 7};
            VkWriteDescriptorSet writes[3]{};
            for (uint32_t image = 0; image < 3; ++image)
                writes[image] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mSets[frame][i],
                    .dstBinding = bindings[image], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &images[image]};
            vkUpdateDescriptorSets(mDevice, 3, writes, 0, nullptr);
        }
        struct Push
        {
            glm::mat4 viewProj{1};
            glm::vec4 anchorStart{}, right{}, up{}, sampling{};
        };
        static_assert(sizeof(Push) == 128);
        static_assert(offsetof(Push, anchorStart) == 64);
        static_assert(offsetof(Push, right) == 80);
        static_assert(offsetof(Push, up) == 96);
        static_assert(offsetof(Push, sampling) == 112);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeline);
        Push push;
        const auto stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        for (const auto &view : views)
        {
            const auto rect = ResolveRect(view.rect, extent);
            const VkViewport viewport{float(rect.offset.x), float(rect.offset.y), float(rect.extent.width), float(rect.extent.height), 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &rect);
            const auto inverse = glm::inverse(view.viewProj);
            // Projection scales cancel on normalization, these are the world camera axes.
            push.right = glm::vec4(glm::normalize(glm::vec3(inverse[0])), jitter.x);
            push.up = glm::vec4(glm::normalize(glm::vec3(inverse[1])), jitter.y);
            push.viewProj = view.viewProj;
            push.sampling = {glm::vec2(1.0f / extent.width, 1.0f / extent.height), rayDistance ? 1.0f : 0.0f, 0};
            for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
            {
                const auto &slot = data.mSlots[i];
                if (!slot.allocated) continue;
                const auto anchor = TryCameraRelativeFloat(slot.anchor, view.cameraPosition);
                if (!anchor) continue;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 0, 1, &mSets[frame][i], 0, nullptr);
                push.anchorStart = glm::vec4(*anchor, float(slot.start));
                vkCmdPushConstants(cmd, mLayout, stages, 0, sizeof(push), &push);
                vkCmdDrawIndirect(cmd, data.mCounters.handle, i * sizeof(ParticleDataset::Counters), 1, sizeof(ParticleDataset::Counters));
            }
        }
    }
    void ParticleSpriteRenderer::Shutdown()
    {
        if (!mDevice) return;
        for (auto &[key, texture] : mTextures) DestroyTexture(texture);
        mTextures.clear();
        for (auto &signal : mSignals) signal = 0;
        mAllocator = nullptr;
        if (mPipeline) vkDestroyPipeline(mDevice, mPipeline, nullptr);
        if (mLayout) vkDestroyPipelineLayout(mDevice, mLayout, nullptr);
        if (mPool) vkDestroyDescriptorPool(mDevice, mPool, nullptr);
        if (mSetLayout) vkDestroyDescriptorSetLayout(mDevice, mSetLayout, nullptr);
        if (mSampler) vkDestroySampler(mDevice, mSampler, nullptr);
        mPipeline = VK_NULL_HANDLE; mLayout = VK_NULL_HANDLE; mPool = VK_NULL_HANDLE; mSetLayout = VK_NULL_HANDLE;
        mSampler = VK_NULL_HANDLE; mDevice = VK_NULL_HANDLE;
    }
}

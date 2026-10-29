/**
 * @file ParticleViewSort.cpp
 * @author sumin.park
 * @brief Bounded per-view sort scratch with asynchronous visibility diagnostics.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ParticleViewSort.h"
#include "ParticleDataset.h"
#include "ParticleVulkan.h"

#include <core/log/SetupError.h>
#include <renderer/VulkanRenderer.h>

#include <algorithm>
#include <bit>
#include <cstring>

namespace mir::gpu
{
    namespace
    {
        constexpr VkDeviceSize kArgumentsBytes = kFxMaxViews * kGpuParticleInstances * sizeof(VkDrawIndirectCommand);
        constexpr VkDeviceSize kNodesPerView = kGpuParticleCapacity * 2;
        void RequireSort(bool ok)
        {
            if (!ok) throw SetupError(ErrorCategory::System, "Initialize particle view sorting", "particleViewSort",
                "Vulkan sorting resources could not be created", "Rebuild shaders and check GPU memory and driver logs.");
        }
    }
    void ParticleViewSort::Initialize(const FxFrameContext &context, const ParticleDataset &data)
    {
        mDevice = context.device.device;
        mAllocator = context.device.allocator;
        const VkDescriptorSetLayoutBinding bindings[]{
            {3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {5,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {9,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {11,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
        const VkDescriptorSetLayoutCreateInfo set{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount=4,.pBindings=bindings};
        RequireSort(vkCreateDescriptorSetLayout(mDevice,&set,nullptr,&mSetLayout)==VK_SUCCESS);
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4*kFxFramesInFlight};
        const VkDescriptorPoolCreateInfo pool{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets=kFxFramesInFlight,.poolSizeCount=1,.pPoolSizes=&size};
        RequireSort(vkCreateDescriptorPool(mDevice,&pool,nullptr,&mPool)==VK_SUCCESS);
        for(uint32_t f=0;f<kFxFramesInFlight;++f)
        {
            auto make=[&](Buffer &b,VkDeviceSize bytes,VkBufferUsageFlags usage,bool host)
            {
                VmaAllocationInfo info{};
                RequireSort(CreateParticleBuffer(mAllocator,bytes,usage,
                    host ? VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : 0,
                    b.handle,b.memory,info));
                b.mapped=info.pMappedData;
            };
            make(mNodes[f],kNodesPerView*sizeof(glm::uvec2),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,false);
            mNodeViews[f]=1;
            make(mArguments[f],kArgumentsBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,false);
            make(mReadback[f],kArgumentsBytes+kFxMaxViews*sizeof(glm::uvec2),VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
            const VkDescriptorSetAllocateInfo allocate{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool=mPool,.descriptorSetCount=1,.pSetLayouts=&mSetLayout};
            RequireSort(vkAllocateDescriptorSets(mDevice,&allocate,&mSets[f])==VK_SUCCESS);
            const VkBuffer buffers[]{data.mCounters.handle,data.mParticles.handle,mNodes[f].handle,mArguments[f].handle};
            for(uint32_t i=0;i<4;++i)
            {
                const VkDescriptorBufferInfo buffer{buffers[i],0,VK_WHOLE_SIZE};
                const VkWriteDescriptorSet write{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=mSets[f],
                    .dstBinding=bindings[i].binding,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer};
                vkUpdateDescriptorSets(mDevice,1,&write,0,nullptr);
            }
        }
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,128};
        const VkPipelineLayoutCreateInfo layout{.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount=1,.pSetLayouts=&mSetLayout,.pushConstantRangeCount=1,.pPushConstantRanges=&range};
        RequireSort(vkCreatePipelineLayout(mDevice,&layout,nullptr,&mLayout)==VK_SUCCESS);
        const auto shader=LoadShader(mDevice,"particleViewSort.computeMain.spv");
        const VkComputePipelineCreateInfo pipeline{.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,
                .module=shader,.pName="computeMain"},.layout=mLayout};
        const auto result=shader ? vkCreateComputePipelines(mDevice,VK_NULL_HANDLE,1,&pipeline,nullptr,&mPipeline) : VK_ERROR_INITIALIZATION_FAILED;
        if(shader) vkDestroyShaderModule(mDevice,shader,nullptr);
        RequireSort(result==VK_SUCCESS);
    }
    void ParticleViewSort::Prepare(const FxRenderContext &context,const ParticleDataset &data)
    {
        if(!data.Initialized()) return;
        if(!mDevice) Initialize(context.frame,data);
        const auto f=context.frame.frame;
        const auto cmd=context.frame.commands;
        const auto required=std::max<uint32_t>(1,static_cast<uint32_t>(std::min<size_t>(context.views.size(),kFxMaxViews)));
        if(required>mNodeViews[f])
        {
            // BeginFrame completed this slot, other slots keep their own buffers.
            Buffer replacement; VmaAllocationInfo info{};
            RequireSort(CreateParticleBuffer(mAllocator,uint64_t(required)*kNodesPerView*sizeof(glm::uvec2),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,0,
                replacement.handle,replacement.memory,info));
            vmaDestroyBuffer(mAllocator,mNodes[f].handle,mNodes[f].memory);
            mNodes[f]=replacement;mNodeViews[f]=required;
            const VkDescriptorBufferInfo buffer{replacement.handle,0,VK_WHOLE_SIZE};
            const VkWriteDescriptorSet write{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet=mSets[f],.dstBinding=9,.descriptorCount=1,
                .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer};
            vkUpdateDescriptorSets(mDevice,1,&write,0,nullptr);
        }
        if(mSignals[f])
        {
            vmaInvalidateAllocation(mAllocator,mReadback[f].memory,0,VK_WHOLE_SIZE);
            const auto *arguments=static_cast<const VkDrawIndirectCommand *>(mReadback[f].mapped);
            const auto *first=reinterpret_cast<const glm::uvec2 *>(arguments+kFxMaxViews*kGpuParticleInstances);
            std::fill(std::begin(mVisible),std::end(mVisible),0);
            std::fill(std::begin(mFirst),std::end(mFirst),UINT32_MAX);
            for(uint32_t v=0;v<mReadbackViews[f];++v)
            {
                mVisible[v]=0;
                for(uint32_t i=0;i<kGpuParticleInstances;++i) mVisible[v]+=arguments[v*kGpuParticleInstances+i].instanceCount;
                mFirst[v]=mVisible[v] ? first[v].y : UINT32_MAX;
            }
            mCompleted=mSignals[f];
        }
        MemoryBarrier(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(cmd,mArguments[f].handle,0,VK_WHOLE_SIZE,0);
        MemoryBarrier(cmd,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mPipeline);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mLayout,0,1,&mSets[f],0,nullptr);
        struct Push { glm::mat4 viewProj; glm::vec4 anchor; glm::uvec4 range,step; glm::vec4 limits; };
        static_assert(sizeof(Push)==128);
        uint32_t firstSlot=UINT32_MAX;
        for(uint32_t i=0;i<kGpuParticleInstances;++i) if(data.mSlots[i].allocated) {firstSlot=i;break;}
        const auto viewCount=std::min<size_t>(context.views.size(),kFxMaxViews);
        for(uint32_t v=0;v<viewCount;++v)
        {
            const auto rect=ResolveRect(context.views[v].rect,context.extent);
            if(!rect.extent.width || !rect.extent.height) continue;
            for(uint32_t i=0;i<kGpuParticleInstances;++i)
            {
                const auto &slot=data.mSlots[i];
                if(!slot.allocated) continue;
                const auto anchor=TryCameraRelativeFloat(slot.anchor,context.views[v].cameraPosition);
                if(!anchor) continue;
                const auto count=std::bit_ceil(slot.input.capacity);
                Push push{context.views[v].viewProj,glm::vec4(*anchor,0),{slot.start,slot.input.capacity,count,i},
                    {uint32_t(v*kNodesPerView),v*kGpuParticleInstances,0,0},
                    {slot.input.renderer.maxDistance,float(slot.input.renderer.sort),count<=2048?2.0f:1.0f,0}};
                auto dispatch=[&]
                {
                    vkCmdPushConstants(cmd,mLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
                    vkCmdDispatch(cmd,count<=2048?1:(count+127)/128,1,1);
                    MemoryBarrier(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
                };
                dispatch();
                if(count<=2048) continue;
                push.limits.z=0;
                for(uint32_t stage=2;stage<=count;stage*=2)
                    for(uint32_t pass=stage/2;pass;pass/=2)
                    { push.step.z=stage;push.step.w=pass;dispatch(); }
            }
        }
        MemoryBarrier(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_2_TRANSFER_READ_BIT);
        if(viewCount)
        {
            const VkBufferCopy arguments{0,0,viewCount*kGpuParticleInstances*sizeof(VkDrawIndirectCommand)};
            vkCmdCopyBuffer(cmd,mArguments[f].handle,mReadback[f].handle,1,&arguments);
        }
        if(firstSlot!=UINT32_MAX)
            for(uint32_t v=0;v<viewCount;++v)
            {
                const VkBufferCopy sample{(v*kNodesPerView+data.mSlots[firstSlot].start*2)*sizeof(glm::uvec2),
                    kArgumentsBytes+v*sizeof(glm::uvec2),sizeof(glm::uvec2)};
                vkCmdCopyBuffer(cmd,mNodes[f].handle,mReadback[f].handle,1,&sample);
            }
        MemoryBarrier(cmd,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_READ_BIT);
        mSignals[f]=context.frame.signalValue;
        mReadbackViews[f]=static_cast<uint32_t>(viewCount);
    }
    void ParticleViewSort::Stats(ParticleGpuStats &stats) const
    {
        stats.renderFrame=mCompleted;
        std::copy(std::begin(mVisible),std::end(mVisible),stats.visible);
        std::copy(std::begin(mFirst),std::end(mFirst),stats.firstSortedParticle);
    }
    void ParticleViewSort::Shutdown()
    {
        if(!mDevice) return;
        if(mPipeline) vkDestroyPipeline(mDevice,mPipeline,nullptr);
        if(mLayout) vkDestroyPipelineLayout(mDevice,mLayout,nullptr);
        if(mPool) vkDestroyDescriptorPool(mDevice,mPool,nullptr);
        if(mSetLayout) vkDestroyDescriptorSetLayout(mDevice,mSetLayout,nullptr);
        for(uint32_t f=0;f<kFxFramesInFlight;++f)
            for(auto *b:{&mNodes[f],&mArguments[f],&mReadback[f]}) if(b->handle) vmaDestroyBuffer(mAllocator,b->handle,b->memory);
        mDevice=VK_NULL_HANDLE;
        std::fill(std::begin(mSignals),std::end(mSignals),0);
        std::fill(std::begin(mReadbackViews),std::end(mReadbackViews),0);
    }
}

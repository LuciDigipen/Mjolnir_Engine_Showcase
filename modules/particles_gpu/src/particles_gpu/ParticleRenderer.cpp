/**
 * @file ParticleRenderer.cpp
 * @author sumin.park
 * @brief Shared sprite and ribbon materials and sorted FX composition.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ParticleRenderer.h"
#include "ParticleDataset.h"
#include "particles_gpu/ParticleRibbonGeometry.h"
#include "ParticleVulkan.h"

#include <core/log/SetupError.h>
#include <renderer/VulkanRenderer.h>

#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
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

    void ParticleRenderer::DestroyTexture(Texture &texture)
    {
        if (texture.staging) vmaDestroyBuffer(mAllocator, texture.staging, texture.stagingAllocation);
        if (texture.view) vkDestroyImageView(mDevice, texture.view, nullptr);
        if (texture.image) vmaDestroyImage(mAllocator, texture.image, texture.allocation);
        mTextureBytes -= texture.bytes;
        texture = {};
    }

    void ParticleRenderer::UploadTexture(const FxFrameContext &context, Texture &texture,
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

    void ParticleRenderer::Submit(std::span<const RibbonSubmission> ribbons)
    {
        mRibbons.clear();
        size_t remaining=kGpuParticleCapacity;
        for(const auto &ribbon:ribbons)
        {
            if(mRibbons.size()==kGpuParticleInstances || ribbon.points.size()>remaining) break;
            if(!ValidParticleRenderProperties(ribbon.renderer)) continue;
            mRibbons.push_back(ribbon);
            remaining-=ribbon.points.size();
        }
    }
    void ParticleRenderer::PrepareTextures(const FxFrameContext &context,const ParticleDataset &data)
    {
        if(!data.Initialized() && mRibbons.empty()) return;
        mDevice=context.device.device;
        mAllocator=context.device.allocator;
        const auto completed=mSignals[context.frame];
        struct Input { uint64_t key; uint32_t width,height; std::span<const uint8_t> pixels; };
        std::array<Input,kGroups> inputs{};
        std::array<bool,kGroups> active{};
        for(uint32_t i=0;i<kGpuParticleInstances;++i)
            if(data.mSlots[i].allocated)
            {
                const auto &p=data.mSlots[i].input;
                inputs[i]={p.textureKey,p.textureWidth,p.textureHeight,p.texturePixels};active[i]=true;
            }
        for(uint32_t i=0;i<mRibbons.size();++i)
        {
            const auto &p=mRibbons[i];
            inputs[i+kGpuParticleInstances]={p.textureKey,p.textureWidth,p.textureHeight,p.texturePixels};
            active[i+kGpuParticleInstances]=true;
        }
        std::set<TextureKey> used;
        for(uint32_t i=0;i<kGroups;++i)
        {
            if(!active[i]) continue;
            const auto &input=inputs[i];
            TextureKey key{};
            if(input.key || !input.pixels.empty())
            {
                if(!input.key || !input.width || !input.height || input.width>2048 || input.height>2048 ||
                    input.pixels.size()!=size_t(input.width)*input.height*4)
                    TextureFailure("Malformed particle RGBA8 texture or dimensions beyond 2048");
                uint64_t hash=14695981039346656037ull;
                for(auto byte:input.pixels) {hash^=byte;hash*=1099511628211ull;}
                hash^=uint64_t(input.width)<<32|input.height;
                key={input.key,hash};
            }
            mSlotTextures[i]=key;
            used.insert(key);
        }
        for(auto it=mTextures.begin();it!=mTextures.end();)
        {
            auto &texture=it->second;
            if(!used.contains(it->first) && texture.lastUse<=completed)
            { DestroyTexture(texture);it=mTextures.erase(it);continue; }
            if(texture.staging && texture.uploaded<=completed)
            {
                vmaDestroyBuffer(mAllocator,texture.staging,texture.stagingAllocation);
                texture.staging=VK_NULL_HANDLE;texture.stagingAllocation=nullptr;
                mTextureBytes-=texture.bytes/2;texture.bytes/=2;
            }
            ++it;
        }
        for(uint32_t i=0;i<kGroups;++i)
        {
            if(!active[i]) continue;
            const auto key=mSlotTextures[i];
            auto [entry,fresh]=mTextures.try_emplace(key);
            if(fresh)
            {
                const uint8_t white[]{255,255,255,255};
                UploadTexture(context,entry->second,key.first?inputs[i].width:1,key.first?inputs[i].height:1,
                    key.first?inputs[i].pixels.data():white);
            }
            entry->second.lastUse=context.signalValue;
        }
        mSignals[context.frame]=context.signalValue;
    }
    void ParticleRenderer::Initialize(const FxRenderContext &context)
    {
        auto require=[](bool ok)
        {
            if(!ok) throw SetupError(ErrorCategory::System,"Initialize particle rendering","particle materials",
                "Vulkan graphics resources could not be created","Rebuild shaders and check GPU memory and driver logs.");
        };
        VkSamplerCreateInfo sampler{.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,
            .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        require(vkCreateSampler(mDevice,&sampler,nullptr,&mSampler)==VK_SUCCESS);
        sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
        sampler.addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT;
        require(vkCreateSampler(mDevice,&sampler,nullptr,&mTextureSampler)==VK_SUCCESS);
        const VkDescriptorSetLayoutBinding bindings[]{
            {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
            {5,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},
            {7,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
            {8,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},
            {9,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},
            {10,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
        const VkDescriptorSetLayoutCreateInfo set{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount=6,.pBindings=bindings};
        require(vkCreateDescriptorSetLayout(mDevice,&set,nullptr,&mSetLayout)==VK_SUCCESS);
        const VkDescriptorPoolSize sizes[]{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2*kGroups*kFxFramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4*kGroups*kFxFramesInFlight}};
        const VkDescriptorPoolCreateInfo pool{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets=kGroups*kFxFramesInFlight,.poolSizeCount=2,.pPoolSizes=sizes};
        require(vkCreateDescriptorPool(mDevice,&pool,nullptr,&mPool)==VK_SUCCESS);
        for(uint32_t f=0;f<kFxFramesInFlight;++f)
        {
            auto make=[&](Buffer &buffer,VkDeviceSize bytes)
            {
                VmaAllocationInfo info{};
                require(CreateParticleBuffer(mAllocator,bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                    buffer.handle,buffer.memory,info));
                buffer.mapped=info.pMappedData;
            };
            make(mVertices[f],sizeof(RibbonVertex)*kRibbonVerticesPerFrame);
            make(mViews[f],sizeof(ViewData)*kGroups*kFxMaxViews);
            for(uint32_t i=0;i<kGroups;++i)
            {
                const VkDescriptorSetAllocateInfo allocate{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                    .descriptorPool=mPool,.descriptorSetCount=1,.pSetLayouts=&mSetLayout};
                require(vkAllocateDescriptorSets(mDevice,&allocate,&mSets[f][i])==VK_SUCCESS);
            }
        }
        const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,4};
        const VkPipelineLayoutCreateInfo layout{.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount=1,.pSetLayouts=&mSetLayout,.pushConstantRangeCount=1,.pPushConstantRanges=&range};
        require(vkCreatePipelineLayout(mDevice,&layout,nullptr,&mLayout)==VK_SUCCESS);
        for(uint32_t ribbon=0;ribbon<2;++ribbon)
            for(uint32_t additive=0;additive<2;++additive)
                require(CreateSpritePipeline(mDevice,mLayout,context.colorFormat,mPipelines[ribbon][additive],ribbon!=0,additive!=0));
    }
    void ParticleRenderer::PrepareDraw(const FxRenderContext &context,const ParticleDataset &data)
    {
        mDraws.clear();
        if(!mDevice) return;
        mSort.Prepare(context,data);
        if(!mLayout) Initialize(context);
        const auto f=context.frame.frame;
        auto *views=static_cast<ViewData *>(mViews[f].mapped);
        uint32_t cursor=0;
        const auto viewCount=std::min<size_t>(context.views.size(),kFxMaxViews);
        for(uint32_t v=0;v<viewCount;++v)
        {
            const auto &camera=context.views[v];
            const auto inverse=glm::inverse(camera.viewProj);
            const auto rect=ResolveRect(camera.rect,context.extent);
            if(!rect.extent.width || !rect.extent.height) continue;
            ViewData base{};
            base.viewProj=camera.viewProj;base.inverseViewProj=inverse;
            base.right=glm::vec4(glm::normalize(glm::vec3(inverse[0])),context.jitter.x);
            base.up=glm::vec4(glm::normalize(glm::vec3(inverse[1])),context.jitter.y);
            base.sampling={1.0f/context.extent.width,1.0f/context.extent.height,context.rayDistance?1.0f:0.0f,
                float(v*kGpuParticleCapacity*2)};
            base.rect={rect.offset.x,rect.offset.y,rect.extent.width,rect.extent.height};
            base.sunDirection=context.environment.sunDirection;base.sunColor=context.environment.sunColor;
            base.ambient=context.environment.ambient;base.fogColor=context.environment.fogColor;base.fogRange=context.environment.fogRange;
            const auto first=mDraws.size();
            for(uint32_t slot=0;slot<kGroups;++slot)
            {
                const bool ribbon=slot>=kGpuParticleInstances;
                const auto r=slot-kGpuParticleInstances;
                if(ribbon ? r>=mRibbons.size() : !data.mSlots[slot].allocated) continue;
                auto view=base;
                const auto &properties=ribbon?mRibbons[r].renderer:data.mSlots[slot].input.renderer;
                view.material={properties.softDistance,properties.maxDistance,properties.lit?1.0f:0.0f,properties.fog?1.0f:0.0f};
                view.options={properties.alignment,properties.facing,properties.blend,properties.sort};
                const uint32_t textureWidth=ribbon?mRibbons[r].textureWidth:data.mSlots[slot].input.textureWidth;
                const uint32_t textureHeight=ribbon?mRibbons[r].textureHeight:data.mSlots[slot].input.textureHeight;
                view.texture={properties.textured?1.0f:0.0f,1.0f/std::max(1u,textureWidth),1.0f/std::max(1u,textureHeight),0};
                float distance=0;
                if(ribbon)
                {
                    const auto vertices=BuildRibbonGeometry(std::span(&mRibbons[r],1),camera.cameraPosition,glm::vec3(base.right),
                        kRibbonVerticesPerFrame-cursor,&camera.viewProj);
                    if(vertices.empty()) continue;
                    std::memcpy(static_cast<RibbonVertex *>(mVertices[f].mapped)+cursor,vertices.data(),vertices.size()*sizeof(RibbonVertex));
                    for(const auto &vertex:vertices) distance+=glm::length(glm::vec3(vertex.position))/vertices.size();
                    mDraws.push_back({slot,v,cursor,static_cast<uint32_t>(vertices.size()),true,distance});
                    cursor+=static_cast<uint32_t>(vertices.size());
                }
                else
                {
                    const auto anchor=TryCameraRelativeFloat(data.mSlots[slot].anchor,camera.cameraPosition);
                    if(!anchor) continue;
                    view.anchorStart=glm::vec4(*anchor,float(data.mSlots[slot].start));
                    const auto emission=TryCameraRelativeFloat(data.mSlots[slot].input.anchor,camera.cameraPosition);
                    distance=emission?glm::length(*emission):glm::length(*anchor);
                    mDraws.push_back({slot,v,0,0,false,distance});
                }
                views[v*kGroups+slot]=view;
            }
            std::stable_sort(mDraws.begin()+first,mDraws.end(),[](const Draw &a,const Draw &b){return a.distance>b.distance;});
        }
        vmaFlushAllocation(mAllocator,mViews[f].memory,0,VK_WHOLE_SIZE);
        if(cursor) vmaFlushAllocation(mAllocator,mVertices[f].memory,0,cursor*sizeof(RibbonVertex));
        std::set<uint32_t> written;
        for(const auto &draw:mDraws)
        {
            if(!written.insert(draw.slot).second) continue;
            const auto texture=mTextures.find(mSlotTextures[draw.slot]);
            if(texture==mTextures.end()) TextureFailure("Particle material was not prepared before rendering");
            const VkDescriptorImageInfo images[]{
                {mSampler,context.depth,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {mTextureSampler,texture->second.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            const uint32_t imageBindings[]{1,7};
            for(uint32_t i=0;i<2;++i)
            {
                const VkWriteDescriptorSet write{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=mSets[f][draw.slot],
                    .dstBinding=imageBindings[i],.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[i]};
                vkUpdateDescriptorSets(mDevice,1,&write,0,nullptr);
            }
            const VkBuffer buffers[]{data.mParticles.handle,mVertices[f].handle,mSort.Indices(f),mViews[f].handle};
            const uint32_t bindings[]{5,8,9,10};
            for(uint32_t i=0;i<4;++i)
            {
                if(!buffers[i]) continue;
                const VkDescriptorBufferInfo buffer{buffers[i],0,VK_WHOLE_SIZE};
                const VkWriteDescriptorSet write{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=mSets[f][draw.slot],
                    .dstBinding=bindings[i],.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer};
                vkUpdateDescriptorSets(mDevice,1,&write,0,nullptr);
            }
        }
    }
    void ParticleRenderer::Record(const FxRenderContext &context,const ParticleDataset &data)
    {
        const auto cmd=context.frame.commands;
        const auto f=context.frame.frame;
        for(const auto &draw:mDraws)
        {
            const auto &properties=draw.ribbon?mRibbons[draw.slot-kGpuParticleInstances].renderer:data.mSlots[draw.slot].input.renderer;
            const auto rect=ResolveRect(context.views[draw.view].rect,context.extent);
            const VkViewport viewport{float(rect.offset.x),float(rect.offset.y),float(rect.extent.width),float(rect.extent.height),0,1};
            vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&rect);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,mPipelines[draw.ribbon?1:0][properties.blend]);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,mLayout,0,1,&mSets[f][draw.slot],0,nullptr);
            const uint32_t index=draw.view*kGroups+draw.slot;
            vkCmdPushConstants(cmd,mLayout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,4,&index);
            if(draw.ribbon) vkCmdDraw(cmd,draw.count,1,draw.first,0);
            else vkCmdDrawIndirect(cmd,mSort.Arguments(f),(draw.view*kGpuParticleInstances+draw.slot)*sizeof(VkDrawIndirectCommand),1,sizeof(VkDrawIndirectCommand));
        }
    }
    void ParticleRenderer::Shutdown()
    {
        mSort.Shutdown();
        if(!mDevice) return;
        for(auto &[key,texture]:mTextures) DestroyTexture(texture);
        mTextures.clear();
        for(auto &row:mPipelines) for(auto pipeline:row) if(pipeline) vkDestroyPipeline(mDevice,pipeline,nullptr);
        if(mLayout) vkDestroyPipelineLayout(mDevice,mLayout,nullptr);
        if(mPool) vkDestroyDescriptorPool(mDevice,mPool,nullptr);
        if(mSetLayout) vkDestroyDescriptorSetLayout(mDevice,mSetLayout,nullptr);
        if(mSampler) vkDestroySampler(mDevice,mSampler,nullptr);
        if(mTextureSampler) vkDestroySampler(mDevice,mTextureSampler,nullptr);
        for(uint32_t f=0;f<kFxFramesInFlight;++f)
            for(auto *buffer:{&mVertices[f],&mViews[f]}) if(buffer->handle) vmaDestroyBuffer(mAllocator,buffer->handle,buffer->memory);
        mDevice=VK_NULL_HANDLE;
        mRibbons.clear();mDraws.clear();
    }
}

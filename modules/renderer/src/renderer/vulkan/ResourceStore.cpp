/**
 * @file ResourceStore.cpp
 * @author Sumin Park
 * @brief Mesh and texture uploads, mip generation, samplers and default textures.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ResourceStore.h"
#include "Device.h"
#include "MaterialTableLayout.h"
#include "Slots.h"
#include "VulkanUtil.h"

#include <core/log/Log.h>
#include <renderer/TextureUpload.h>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

namespace mir::vk
{
    bool ResourceStore::Initialize(Device &device)
    {
        mDevice = &device;
        if (AcquireSampler(SamplerDesc{}) == VK_NULL_HANDLE)
            return false;
        return CreateDefaultTextures();
    }

    void ResourceStore::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        ReleaseMeshes();
        DestroyTextures();
        DestroySamplers();
    }

    MeshHandle ResourceStore::CreateMesh(std::span<const Vertex> vertices,
        std::span<const uint32_t> indices)
    {
        if (vertices.empty() || indices.empty())
        {
            MIR_LOG_ERROR("CreateMesh: vertices and indices must both be non-empty");
            return kNullMesh;
        }

        const VkDeviceSize vertexBytes = vertices.size_bytes();
        const VkDeviceSize indexBytes = indices.size_bytes();

        // temporarily pack multiple data to one buffer for faster creation
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo stagingInfo{};

        if (!CreateBuffer(mDevice->Allocator(), vertexBytes + indexBytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
            staging, stagingAllocation, &stagingInfo))
        {
            MIR_LOG_CRITICAL("CreateMesh: staging buffer allocation failed");
            return kNullMesh;
        }

        std::byte *const mapped = static_cast<std::byte *>(stagingInfo.pMappedData);
        std::memcpy(mapped, vertices.data(), vertexBytes);
        std::memcpy(mapped + vertexBytes, indices.data(), indexBytes);
        vmaFlushAllocation(mDevice->Allocator(), stagingAllocation, 0, VK_WHOLE_SIZE);

        GpuMesh mesh{};
        mesh.mIndexCount = static_cast<uint32_t>(indices.size());

        // Ray tracing reads both buffers through device addresses to build the BLAS.
        VkBufferUsageFlags extraUsage = 0;
        if (mDevice->RayTracingSupported())
            extraUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

        // No host-access flags on either: that is what puts them in VRAM.
        const bool buffersCreated =
            CreateBuffer(mDevice->Allocator(), vertexBytes,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | extraUsage,
                0, mesh.mVertexBuffer, mesh.mVertexAllocation, nullptr) &&
            CreateBuffer(mDevice->Allocator(), indexBytes,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | extraUsage,
                0, mesh.mIndexBuffer, mesh.mIndexAllocation, nullptr);

        if (!buffersCreated)
        {
            MIR_LOG_CRITICAL("CreateMesh: device buffer allocation failed");
            FreeMesh(mesh);
            vmaDestroyBuffer(mDevice->Allocator(), staging, stagingAllocation);
            return kNullMesh;
        }

        const bool uploaded = SubmitAndWait(*mDevice, [&](VkCommandBuffer cmd)
            {
                const VkBufferCopy vertexCopy{ .srcOffset = 0, .dstOffset = 0, .size = vertexBytes };
                vkCmdCopyBuffer(cmd, staging, mesh.mVertexBuffer, 1, &vertexCopy);

                // srcOffset picks the second half of the same staging allocation.
                const VkBufferCopy indexCopy{ .srcOffset = vertexBytes, .dstOffset = 0, .size = indexBytes };
                vkCmdCopyBuffer(cmd, staging, mesh.mIndexBuffer, 1, &indexCopy);
            });

        vmaDestroyBuffer(mDevice->Allocator(), staging, stagingAllocation);

        if (!uploaded)
        {
            FreeMesh(mesh);
            return kNullMesh;
        }

        const uint32_t index = PlaceSlot(mMeshes, mFreeMeshIndices, mesh);
        mHighestMeshGeneration = std::max(mHighestMeshGeneration, mMeshes[index].mGeneration);

        mDevice->NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mesh.mVertexBuffer),
            std::format("Mesh {} vertices", index).c_str());
        mDevice->NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mesh.mIndexBuffer),
            std::format("Mesh {} indices", index).c_str());

        MIR_LOG_INFO("Mesh {} uploaded: {} vertices, {} indices, {} bytes",
            index, vertices.size(), indices.size(), vertexBytes + indexBytes);

        return MeshHandle{ index, mMeshes[index].mGeneration };
    }

    void ResourceStore::FreeMesh(GpuMesh &mesh)
    {
        if (mesh.mVertexBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(mDevice->Allocator(), mesh.mVertexBuffer, mesh.mVertexAllocation);
        if (mesh.mIndexBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(mDevice->Allocator(), mesh.mIndexBuffer, mesh.mIndexAllocation);
    }

    bool ResourceStore::DestroyMesh(MeshHandle handle)
    {
        GpuMesh *const mesh = FindMeshSlot(handle);
        if (mesh == nullptr)
            return false;

        FreeMesh(*mesh);
        RetireSlot(*mesh);

        mFreeMeshIndices.push_back(handle.mIndex);
        return true;
    }

    std::size_t ResourceStore::ReleaseMeshes()
    {
        const std::size_t count = mMeshes.size();
        mFreeMeshIndices.clear();
        // Reversed, so the free list hands out index 0 first.
        for (uint32_t i = static_cast<uint32_t>(count); i-- > 0;)
        {
            FreeMesh(mMeshes[i]);
            RetireSlot(mMeshes[i]);
            mFreeMeshIndices.push_back(i);
        }
        return count;
    }

    const GpuMesh *ResourceStore::FindMesh(MeshHandle handle) const
    {
        const GpuMesh *const mesh = FindSlot(mMeshes, handle);
        return mesh != nullptr && mesh->mState == GpuMeshState::Ready ? mesh : nullptr;
    }

    GpuMesh *ResourceStore::FindMeshSlot(MeshHandle handle)
    {
        return const_cast<GpuMesh *>(FindSlot(mMeshes, handle));
    }

    MeshHandle ResourceStore::ReserveMesh()
    {
        GpuMesh mesh{};
        mesh.mState = GpuMeshState::Pending;
        const uint32_t index = PlaceSlot(mMeshes, mFreeMeshIndices, mesh);
        mHighestMeshGeneration = std::max(mHighestMeshGeneration, mMeshes[index].mGeneration);
        return MeshHandle{index, mMeshes[index].mGeneration};
    }

    bool ResourceStore::CreateMeshBuffers(GpuMesh &mesh, uint32_t slot, VkDeviceSize vertexBytes,
                                          VkDeviceSize indexBytes, bool rayTracing)
    {
        const VkBufferUsageFlags extraUsage =
            rayTracing ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                       : 0;
        const bool created =
            CreateBuffer(mDevice->Allocator(), vertexBytes,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | extraUsage, 0,
                         mesh.mVertexBuffer, mesh.mVertexAllocation, nullptr) &&
            CreateBuffer(mDevice->Allocator(), indexBytes,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | extraUsage, 0,
                         mesh.mIndexBuffer, mesh.mIndexAllocation, nullptr);
        if (!created)
        {
            FreeMesh(mesh);
            mesh.mVertexBuffer = mesh.mIndexBuffer = VK_NULL_HANDLE;
            mesh.mVertexAllocation = mesh.mIndexAllocation = VK_NULL_HANDLE;
            return false;
        }
        mDevice->NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mesh.mVertexBuffer),
                            std::format("Mesh {} vertices", slot).c_str());
        mDevice->NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mesh.mIndexBuffer),
                            std::format("Mesh {} indices", slot).c_str());
        return true;
    }

    GpuMesh ResourceStore::TakeMesh(MeshHandle handle)
    {
        GpuMesh *const mesh = FindMeshSlot(handle);
        if (mesh == nullptr)
            return GpuMesh{};
        const GpuMesh taken = *mesh;
        RetireSlot(*mesh);
        mFreeMeshIndices.push_back(handle.mIndex);
        return taken;
    }

    uint32_t ResourceStore::HighestMeshGeneration() const
    {
        return mHighestMeshGeneration;
    }

    VkSampler ResourceStore::AcquireSampler(const SamplerDesc &desc, uint32_t *outSlot)
    {
        const bool anisotropySupported = mDevice->SamplerAnisotropySupported();
        const float anisotropyLimit = mDevice->MaxSamplerAnisotropy();
        const float anisotropy = EffectiveAnisotropy(desc, anisotropySupported, anisotropyLimit);
        const uint64_t key = SamplerCacheKey(desc, anisotropySupported, anisotropyLimit);

        if (const auto it = mSamplerLookup.find(key); it != mSamplerLookup.end())
        {
            if (outSlot != nullptr)
                *outSlot = it->second;
            return mSamplerTable[it->second];
        }

        if (mSamplerTable.size() >= kMaxSharedSamplers)
        {
            MIR_LOG_ERROR("AcquireSampler: sampler table is full at {} rows", kMaxSharedSamplers);
            return VK_NULL_HANDLE;
        }

        const VkSamplerCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = desc.magFilter,
            .minFilter = desc.minFilter,
            .mipmapMode = desc.mipmapMode,
            .addressModeU = desc.addressModeU,
            .addressModeV = desc.addressModeV,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .anisotropyEnable = anisotropy > 1.0f ? VK_TRUE : VK_FALSE,
            .maxAnisotropy = anisotropy,
            .compareEnable = VK_FALSE,
            .minLod = 0.0f,
            .maxLod = desc.useMips ? VK_LOD_CLAMP_NONE : 0.0f,
            .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK };

        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(mDevice->Handle(), &info, nullptr, &sampler) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("AcquireSampler: vkCreateSampler failed");
            return VK_NULL_HANDLE;
        }

        const uint32_t slot = static_cast<uint32_t>(mSamplerTable.size());
        mSamplerTable.push_back(sampler);
        mSamplerLookup.emplace(key, slot);
        ++mSamplerTableVersion;
        if (slot == 0)
            mDefaultSampler = sampler;

        if (outSlot != nullptr)
            *outSlot = slot;
        return sampler;
    }

    VkSampler ResourceStore::SamplerAt(uint32_t slot) const
    {
        return slot < mSamplerTable.size() ? mSamplerTable[slot] : mDefaultSampler;
    }

    void ResourceStore::DestroySamplers()
    {
        for (const VkSampler sampler : mSamplerTable)
            vkDestroySampler(mDevice->Handle(), sampler, nullptr);
        mSamplerTable.clear();
        mSamplerLookup.clear();
        mDefaultSampler = VK_NULL_HANDLE;
    }

    bool ResourceStore::CreateDefaultTextures()
    {
        const std::array<std::byte, 4> white{
            std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 } };

        const std::array<std::byte, 4> flatNormal{
            std::byte{ 128 }, std::byte{ 128 }, std::byte{ 255 }, std::byte{ 255 } };

        const std::array<std::byte, 4> magenta{
            std::byte{ 255 }, std::byte{ 0 }, std::byte{ 255 }, std::byte{ 255 } };

        mDefaultWhiteTexture = CreateTexture(TextureDesc{
            .width = 1, .height = 1, .pixels = white, .srgb = true });
        mDefaultNormalTexture = CreateTexture(TextureDesc{
            .width = 1, .height = 1, .pixels = flatNormal, .srgb = false });
        mErrorTexture = CreateTexture(TextureDesc{
            .width = 1, .height = 1, .pixels = magenta, .srgb = true });

        if (mDefaultWhiteTexture.IsNull() || mDefaultNormalTexture.IsNull() || mErrorTexture.IsNull())
        {
            MIR_LOG_CRITICAL("CreateDefaultTextures: default texture upload failed");
            return false;
        }

        mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mTextures[mDefaultWhiteTexture.mIndex].mImage), "Default white texture");
        mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mTextures[mDefaultNormalTexture.mIndex].mImage), "Default normal texture");
        mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mTextures[mErrorTexture.mIndex].mImage), "Error Texture");

        return true;
    }

    TextureHandle ResourceStore::CreateTexture(const TextureDesc &desc)
    {
        const VkDeviceSize expectedBytes = VkDeviceSize{ desc.width } * desc.height * 4;
        if (desc.width == 0 || desc.height == 0 || desc.pixels.size_bytes() != expectedBytes)
        {
            MIR_LOG_ERROR("CreateTexture: {}x{} expects {} bytes of RGBA8, got {}",
                desc.width, desc.height, expectedBytes, desc.pixels.size_bytes());
            return kNullTexture;
        }

        const uint32_t maxDimension = mDevice->Properties().limits.maxImageDimension2D;
        if (desc.width > maxDimension || desc.height > maxDimension)
        {
            MIR_LOG_ERROR("CreateTexture: {}x{} is past this device's maxImageDimension2D of {}",
                desc.width, desc.height, maxDimension);
            return kNullTexture;
        }

        const TextureUploadSource source = PrepareTextureUpload(desc);
        if (source.desc.width == 0)
        {
            MIR_LOG_ERROR("CreateTexture: invalid mip layout");
            return kNullTexture;
        }
        std::vector<std::byte> pixels(source.desc.pixels.begin(), source.desc.pixels.end());
        std::vector<VkBufferImageCopy> regions;
        const uint32_t levelCount = static_cast<uint32_t>(source.desc.mips.size()) + 1;
        uint32_t width = desc.width, height = desc.height;
        VkDeviceSize offset = 0;
        for (uint32_t level = 0; level < levelCount; ++level)
        {
            regions.push_back(VkBufferImageCopy{ .bufferOffset = offset,
                .imageSubresource{ VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
                .imageExtent{ width, height, 1 } });
            if (level + 1 == levelCount)
                break;
            const auto &mip = source.desc.mips[level];
            offset = pixels.size();
            pixels.insert(pixels.end(), mip.pixels.begin(), mip.pixels.end());
            width = mip.width;
            height = mip.height;
        }

        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;

        if (!CreateUploadBuffer(mDevice->Allocator(), pixels,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, stagingAllocation))
        {
            MIR_LOG_CRITICAL("CreateTexture: staging buffer allocation failed");
            return kNullTexture;
        }

        const TextureHandle handle = ReserveTexture(desc, levelCount);
        if (handle.IsNull())
        {
            vmaDestroyBuffer(mDevice->Allocator(), staging, stagingAllocation);
            return kNullTexture;
        }
        GpuTexture &texture = *FindTextureSlot(handle);
        const bool uploaded = SubmitAndWait(*mDevice, [&](VkCommandBuffer cmd)
            {
                ImageBarrier(cmd, texture.mImage, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, levelCount);

                vkCmdCopyBufferToImage(cmd, staging, texture.mImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    static_cast<uint32_t>(regions.size()), regions.data());

                ImageBarrier(cmd, texture.mImage, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, levelCount);
            });

        vmaDestroyBuffer(mDevice->Allocator(), staging, stagingAllocation);

        if (!uploaded)
        {
            DestroyTexture(handle);
            return kNullTexture;
        }

        MakeTextureReady(handle);

        MIR_LOG_INFO("Texture {} uploaded: {}x{}, {} level(s), {} bytes",
            handle.mIndex, desc.width, desc.height, levelCount, pixels.size());

        return handle;
    }

    TextureHandle ResourceStore::ReserveTexture(const TextureDesc &desc, uint32_t levelCount)
    {
        if (desc.width == 0 || desc.height == 0 || levelCount == 0 ||
            desc.width > mDevice->Properties().limits.maxImageDimension2D ||
            desc.height > mDevice->Properties().limits.maxImageDimension2D)
            return kNullTexture;
        GpuTexture texture{};
        texture.mLevels = levelCount;
        texture.mState = TextureUploadState::Pending;

        const VkFormat format = desc.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        const VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent{ desc.width, desc.height, 1 },
            .mipLevels = levelCount,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

        const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };
        VmaAllocationInfo memory{};

        if (vmaCreateImage(mDevice->Allocator(), &imageInfo, &allocInfo,
            &texture.mImage, &texture.mAllocation, &memory) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateTexture: vmaCreateImage failed");
            return kNullTexture;
        }

        texture.mBytes = memory.size;
        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = texture.mImage,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = levelCount,
                .baseArrayLayer = 0,
                .layerCount = 1 } };

        if (vkCreateImageView(mDevice->Handle(), &viewInfo, nullptr, &texture.mView) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateTexture: vkCreateImageView failed");
            FreeTexture(texture);
            return kNullTexture;
        }

        const TextureHandle handle = mTextureTable.Add();
        if (handle.IsNull())
        {
            MIR_LOG_ERROR("CreateTexture: image table is full at {} rows", mTextureTable.Capacity());
            FreeTexture(texture);
            return kNullTexture;
        }

        texture.mGeneration = handle.mGeneration;
        if (handle.mIndex >= mTextures.size())
            mTextures.resize(handle.mIndex + 1);
        mTextures[handle.mIndex] = texture;
        ++mTextureTableVersion;

        mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(texture.mImage),
            std::format("Texture {}", handle.mIndex).c_str());

        return handle;
    }

    GpuTexture *ResourceStore::FindTextureSlot(TextureHandle handle)
    {
        return mTextureTable.Contains(handle) ? &mTextures[handle.mIndex] : nullptr;
    }

    void ResourceStore::MakeTextureReady(TextureHandle handle)
    {
        if (GpuTexture *texture = FindTextureSlot(handle))
        {
            texture->mState = TextureUploadState::Ready;
            ++mTextureTableVersion;
        }
    }

    GpuTexture ResourceStore::TakeTexture(TextureHandle handle)
    {
        if (handle == mDefaultWhiteTexture || handle == mDefaultNormalTexture || handle == mErrorTexture ||
            !mTextureTable.Contains(handle))
            return {};
        GpuTexture texture = mTextures[handle.mIndex];
        mTextureTable.Remove(handle);
        mTextures[handle.mIndex] = GpuTexture{};
        mTextures[handle.mIndex].mGeneration = mTextureTable.Generation(handle.mIndex);
        ++mTextureTableVersion;
        return texture;
    }

    void ResourceStore::FreeTexture(Device &device, GpuTexture &texture)
    {
        if (texture.mView != VK_NULL_HANDLE)
            vkDestroyImageView(device.Handle(), texture.mView, nullptr);
        if (texture.mImage != VK_NULL_HANDLE)
            vmaDestroyImage(device.Allocator(), texture.mImage, texture.mAllocation);
    }

    bool ResourceStore::DestroyTexture(TextureHandle handle)
    {
        GpuTexture texture = TakeTexture(handle);
        if (texture.mImage == VK_NULL_HANDLE)
            return false;
        FreeTexture(texture);
        return true;
    }

    std::size_t ResourceStore::ReleaseTextures()
    {
        std::size_t released = 0;
        for (std::size_t i = 0; i < mTextures.size(); ++i)
        {
            const TextureHandle handle{ static_cast<uint32_t>(i), mTextures[i].mGeneration };
            if (handle == mDefaultWhiteTexture || handle == mDefaultNormalTexture || handle == mErrorTexture ||
                !mTextureTable.Contains(handle))
                continue;

            FreeTexture(mTextures[i]);
            mTextureTable.Remove(handle);
            mTextures[i] = GpuTexture{};
            mTextures[i].mGeneration = mTextureTable.Generation(i);
            ++released;
        }
        if (released > 0)
            ++mTextureTableVersion;
        return released;
    }

    const GpuTexture *ResourceStore::FindTexture(TextureHandle handle) const
    {
        if (!mTextureTable.Contains(handle))
            return nullptr;
        const auto &texture = mTextures[handle.mIndex];
        return texture.mState == TextureUploadState::Ready ? &texture : nullptr;
    }

    uint32_t ResourceStore::TextureSlot(TextureHandle handle) const
    {
        if (!mTextureTable.Contains(handle))
            return mDefaultWhiteTexture.mIndex;
        return handle.mIndex;
    }

    uint64_t ResourceStore::TextureAllocatedBytes() const
    {
        uint64_t bytes = 0;
        for (uint32_t index = 0; index < mTextures.size(); ++index)
            if (mTextureTable.Contains({ index, mTextures[index].mGeneration }))
                bytes += mTextures[index].mBytes;
        return bytes;
    }

    VkImageView ResourceStore::ImageViewAt(uint32_t slot) const
    {
        if (slot < mTextures.size())
        {
            const TextureHandle handle{ slot, mTextures[slot].mGeneration };
            if (mTextureTable.Contains(handle) && mTextures[slot].mView != VK_NULL_HANDLE)
                return mTextures[slot].mState == TextureUploadState::Ready ? mTextures[slot].mView :
                    (mErrorTexture.IsNull() ? VK_NULL_HANDLE : mTextures[mErrorTexture.mIndex].mView);
        }
        return mDefaultWhiteTexture.IsNull() ? VK_NULL_HANDLE : mTextures[mDefaultWhiteTexture.mIndex].mView;
    }

    void ResourceStore::DestroyTextures()
    {
        for (GpuTexture &texture : mTextures)
            FreeTexture(texture);
        mTextures.clear();
        mTextureTable = TextureTable{};
        mTextureTableVersion = 0;
        mDefaultWhiteTexture = kNullTexture;
        mDefaultNormalTexture = kNullTexture;
        mErrorTexture = kNullTexture;
    }
}

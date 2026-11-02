/**
 * @file MeshUploader.cpp
 * @author Jackey Cen
 * @brief Records queued mesh uploads and their BLAS builds into frames, and frees retired meshes after their last
 * frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MeshUploader.h"
#include "Device.h"
#include "VulkanUtil.h"

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>

namespace mir::vk
{
    bool MeshUploader::Initialize(Device &device)
    {
        mDevice = &device;
        return Configure({.stagingBytes = kDefaultStagingBytes, .frameBytes = kDefaultFrameBytes});
    }

    bool MeshUploader::RequestStagingBytes(uint64_t bytes)
    {
        if (!Configured())
        {
            MIR_LOG_ERROR("RequestStagingBytes: mesh streaming is not initialized");
            return false;
        }
        constexpr uint64_t kAlign = MeshUploadQueue::kStagingAlignment;
        const MeshStreamingDesc &current = mQueue.Desc();
        if (bytes > UINT64_MAX - (kAlign - 1))
        {
            MIR_LOG_ERROR("RequestStagingBytes: {} bytes is more than a staging ring can hold", bytes);
            return false;
        }
        const uint64_t wanted = (bytes + kAlign - 1) / kAlign * kAlign;
        if (wanted <= current.stagingBytes)
            return true;
        return Configure({.stagingBytes = wanted, .frameBytes = current.frameBytes});
    }

    bool MeshUploader::SetFrameBytes(uint64_t frameBytes)
    {
        if (frameBytes == 0)
        {
            MIR_LOG_ERROR("SetUploadFrameBudget: a budget of zero bytes would admit nothing, nothing changed");
            return false;
        }
        mQueue.SetFrameBytes(frameBytes);
        return true;
    }

    bool MeshUploader::Configure(const MeshStreamingDesc &desc)
    {
        if (desc.stagingBytes < MeshUploadQueue::kStagingAlignment || desc.frameBytes == 0)
        {
            MIR_LOG_ERROR("Mesh streaming: {} staging bytes and {} bytes a frame are too few", desc.stagingBytes,
                          desc.frameBytes);
            return false;
        }
        if (mQueue.Pending() > 0 || mQueue.Uploading() > 0)
        {
            MIR_LOG_ERROR("Mesh streaming: uploads are queued or in flight, so the staging ring stays {} bytes",
                          mQueue.Desc().stagingBytes);
            return false;
        }

        // The old ring is kept until the new one exists.
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaAllocationInfo info{};
        if (!CreateBuffer(mDevice->Allocator(), desc.stagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                          buffer, allocation, &info))
        {
            MIR_LOG_ERROR("Mesh streaming: the {} byte staging ring could not be allocated", desc.stagingBytes);
            return false;
        }
        DestroyStaging();
        mStaging = buffer;
        mStagingAllocation = allocation;
        mStagingMapped = static_cast<std::byte *>(info.pMappedData);
        mDevice->NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mStaging), "Mesh staging ring");
        mQueue = MeshUploadQueue(desc);
        MIR_LOG_INFO("Mesh streaming: {} byte staging ring, {} bytes a frame", desc.stagingBytes, desc.frameBytes);
        return true;
    }

    void MeshUploader::DestroyStaging()
    {
        if (mStaging != VK_NULL_HANDLE)
            vmaDestroyBuffer(mDevice->Allocator(), mStaging, mStagingAllocation);
        mStaging = VK_NULL_HANDLE;
        mStagingAllocation = VK_NULL_HANDLE;
        mStagingMapped = nullptr;
    }

    void MeshUploader::Shutdown(RayTracer &rayTracer)
    {
        if (mDevice == nullptr)
            return;
        Clear(rayTracer);
        DestroyStaging();
    }

    uint64_t MeshUploader::Footprint(const RayTracer &rayTracer, uint32_t vertexCount, uint32_t indexCount,
                                     const MeshUploadOptions &options) const
    {
        uint64_t bytes = uint64_t(vertexCount) * sizeof(Vertex) + uint64_t(indexCount) * sizeof(uint32_t);
        if (options.rayTracing && mDevice->RayTracingSupported() && vertexCount > 0)
            bytes += rayTracer.MeshBlasSizes(vertexCount, indexCount).accelerationStructure;
        return bytes;
    }

    MeshHandle MeshUploader::Queue(ResourceStore &resources, const RayTracer &rayTracer, MeshUploadSource source,
                                   const MeshUploadOptions &options)
    {
        if (!Configured())
        {
            MIR_LOG_ERROR("QueueMesh: mesh streaming is not configured");
            return kNullMesh;
        }
        if (source.vertices.empty() || source.indices.empty())
        {
            MIR_LOG_ERROR("QueueMesh: vertices and indices must both be non-empty");
            return kNullMesh;
        }

        const bool blas = options.rayTracing && mDevice->RayTracingSupported();
        const uint64_t staging = source.vertices.size() * sizeof(Vertex) + source.indices.size() * sizeof(uint32_t);
        const uint64_t gpu =
            Footprint(rayTracer, uint32_t(source.vertices.size()), uint32_t(source.indices.size()), options);
        const MeshHandle handle = resources.ReserveMesh();
        resources.FindMeshSlot(handle)->mBytes = gpu;
        mQueue.Push({handle, staging, gpu});
        mQueued[handle.mIndex] = Queued{handle, std::move(source), blas};
        ++mQueuedCount;
        return handle;
    }

    bool MeshUploader::Retire(ResourceStore &resources, RayTracer &rayTracer, MeshHandle handle, uint64_t lastUse)
    {
        GpuMesh *const mesh = resources.FindMeshSlot(handle);
        if (mesh == nullptr)
            return false;
        if (mesh->mState == GpuMeshState::Pending)
        {
            Forget(handle);
            resources.TakeMesh(handle);
            return true;
        }

        const GpuMesh taken = resources.TakeMesh(handle);
        Garbage garbage;
        garbage.blas = rayTracer.TakeMeshBlas(handle.mIndex);
        if (taken.mVertexBuffer != VK_NULL_HANDLE)
            garbage.buffers.emplace_back(taken.mVertexBuffer, taken.mVertexAllocation);
        if (taken.mIndexBuffer != VK_NULL_HANDLE)
            garbage.buffers.emplace_back(taken.mIndexBuffer, taken.mIndexAllocation);
        const bool allocated = !garbage.buffers.empty() || garbage.blas.mBuffer != VK_NULL_HANDLE;
        mRetired.Push(lastUse, allocated ? taken.mBytes : 0, std::move(garbage));
        ++mRetiredCount;
        return true;
    }

    TextureHandle MeshUploader::QueueTexture(ResourceStore &resources, TextureUploadSource source)
    {
        const auto &desc = source.desc;
        const uint64_t texels = uint64_t(desc.width) * desc.height;
        if (!Configured() || desc.width == 0 || desc.height == 0 || texels > SIZE_MAX / 4 ||
            desc.pixels.size_bytes() != texels * 4 ||
            (!desc.mips.empty() && !TextureMipLayoutValid(desc)) ||
            (desc.generateMips && desc.mips.empty() && (desc.width > 1 || desc.height > 1)) ||
            uint64_t(desc.width) * 4 > mQueue.Desc().stagingBytes)
        {
            MIR_LOG_ERROR("QueueTexture: invalid or unprepared pixels, mip chain or staging row size");
            return kNullTexture;
        }
        const uint32_t levels = static_cast<uint32_t>(desc.mips.size()) + 1;
        const TextureHandle handle = resources.ReserveTexture(desc, levels);
        if (handle.IsNull())
            return handle;
        const uint64_t bytes = source.Bytes();
        for (uint32_t level = 0; level < levels; ++level)
        {
            const uint32_t width = level == 0 ? desc.width : desc.mips[level - 1].width;
            const uint32_t height = level == 0 ? desc.height : desc.mips[level - 1].height;
            const uint64_t size = uint64_t(width) * height * 4;
            mQueue.Push({ {}, size, size, handle, level, 0, height, uint64_t(width) * 4 });
        }
        mTextures.emplace(handle.mIndex, QueuedTexture{ handle, std::move(source), bytes });
        ++mTextureQueuedCount;
        return handle;
    }

    bool MeshUploader::RetireTexture(ResourceStore &resources, TextureHandle handle, uint64_t lastUse)
    {
        const auto *slot = resources.FindTextureSlot(handle);
        if (slot == nullptr)
            return false;
        const bool pending = slot->mState == TextureUploadState::Pending;
        GpuTexture texture = resources.TakeTexture(handle);
        if (texture.mImage == VK_NULL_HANDLE)
            return false;
        mQueue.Cancel(handle);
        mTextures.erase(handle.mIndex);
        mTextureFinishes.erase(handle.mIndex);
        if (pending)
            resources.FreeTexture(texture);
        else
        {
            Garbage garbage;
            garbage.textures.push_back(texture);
            mRetiredTextures.Push(lastUse, texture.mBytes, std::move(garbage));
        }
        return true;
    }

    void MeshUploader::RecordTexture(VkCommandBuffer cmd, uint64_t frame, ResourceStore &resources,
        const MeshUploadQueue::Admitted &band)
    {
        const auto &item = band.item;
        const auto found = mTextures.find(item.texture.mIndex);
        auto *texture = resources.FindTextureSlot(item.texture);
        if (found == mTextures.end() || found->second.mHandle != item.texture || texture == nullptr)
            return;
        auto &queued = found->second;
        const auto &desc = queued.mSource.desc;
        const auto pixels = item.mip == 0 ? desc.pixels : desc.mips[item.mip - 1].pixels;
        const uint32_t width = item.mip == 0 ? desc.width : desc.mips[item.mip - 1].width;
        const uint64_t offset = *band.stagingOffset;
        std::memcpy(mStagingMapped + offset, pixels.data() + uint64_t(item.firstRow) * item.rowBytes,
            static_cast<size_t>(item.stagingBytes));
        vmaFlushAllocation(mDevice->Allocator(), mStagingAllocation, offset, item.stagingBytes);
        if (texture->mState == TextureUploadState::Pending)
        {
            ImageBarrier(cmd, texture->mImage, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, texture->mLevels);
            texture->mState = TextureUploadState::Uploading;
        }
        const VkBufferImageCopy region{ .bufferOffset = offset,
            .imageSubresource{ VK_IMAGE_ASPECT_COLOR_BIT, item.mip, 0, 1 },
            .imageOffset{ 0, static_cast<int32_t>(item.firstRow), 0 },
            .imageExtent{ width, item.rowCount, 1 } };
        vkCmdCopyBufferToImage(cmd, mStaging, texture->mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        queued.mRemaining -= item.stagingBytes;
        if (queued.mRemaining == 0)
        {
            ImageBarrier(cmd, texture->mImage, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, texture->mLevels);
            mTextureFinishes[item.texture.mIndex] = { item.texture, frame };
            mTextures.erase(found);
        }
    }

    void MeshUploader::Forget(MeshHandle handle)
    {
        if (!mQueue.Cancel(handle))
            return;
        mQueued.erase(handle.mIndex);
        ++mCanceled;
    }

    void MeshUploader::Collect(ResourceStore &resources, RayTracer &rayTracer, uint64_t completed)
    {
        for (const auto &item : mQueue.CompleteUploads(completed))
        {
            if (!item.texture.IsNull())
                continue;
            const MeshHandle handle = item.mesh;
            GpuMesh *const mesh = resources.FindMeshSlot(handle);
            if (mesh == nullptr || mesh->mState != GpuMeshState::Uploading)
                continue;
            mesh->mState = GpuMeshState::Ready;
            ++mUploaded;
        }
        std::erase_if(mTextureFinishes, [&](const auto &entry)
            {
                if (entry.second.mFrame > completed)
                    return false;
                resources.MakeTextureReady(entry.second.mHandle);
                ++mTextureUploadedCount;
                return true;
            });
        mRetired.Collect(completed, [&](Garbage &garbage) { Free(rayTracer, garbage); });
        mRetiredTextures.Collect(completed, [&](Garbage &garbage) { Free(rayTracer, garbage); });
    }

    void MeshUploader::Record(VkCommandBuffer cmd, uint64_t frame, ResourceStore &resources, RayTracer &rayTracer)
    {
        if (!Configured())
            return;
        const std::vector<MeshUploadQueue::Admitted> admitted = mQueue.Admit(frame);
        mMaxFrameBytes = std::max(mMaxFrameBytes, mQueue.LastFrameBytes());
        if (admitted.empty())
            return;

        std::vector<RayTracer::MeshBlasBuild> builds;
        for (const MeshUploadQueue::Admitted &entry : admitted)
        {
            if (!entry.item.texture.IsNull())
            {
                RecordTexture(cmd, frame, resources, entry);
                continue;
            }
            const MeshHandle handle = entry.item.mesh;
            const auto found = mQueued.find(handle.mIndex);
            GpuMesh *const mesh = resources.FindMeshSlot(handle);
            if (found == mQueued.end() || found->second.mesh != handle || mesh == nullptr ||
                mesh->mState != GpuMeshState::Pending)
                continue;
            Queued queued = std::move(found->second);
            mQueued.erase(found);

            const uint32_t vertexCount = uint32_t(queued.source.vertices.size());
            const uint32_t indexCount = uint32_t(queued.source.indices.size());
            const VkDeviceSize vertexBytes = VkDeviceSize(vertexCount) * sizeof(Vertex);
            const VkDeviceSize indexBytes = VkDeviceSize(indexCount) * sizeof(uint32_t);
            if (!resources.CreateMeshBuffers(*mesh, handle.mIndex, vertexBytes, indexBytes, queued.blas))
            {
                MIR_LOG_ERROR("Mesh {}: device buffer allocation failed, the upload is dropped", handle.mIndex);
                mesh->mState = GpuMeshState::Failed;
                ++mFailed;
                continue;
            }

            VkBuffer source = mStaging;
            VkDeviceSize offset = 0;
            std::byte *target = nullptr;
            VmaAllocation targetAllocation = mStagingAllocation;
            if (entry.stagingOffset)
            {
                offset = *entry.stagingOffset;
                target = mStagingMapped + offset;
            }
            else
            {
                VmaAllocationInfo info{};
                VkBuffer own = VK_NULL_HANDLE;
                VmaAllocation ownAllocation = VK_NULL_HANDLE;
                if (!CreateBuffer(mDevice->Allocator(), vertexBytes + indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                      VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  own, ownAllocation, &info))
                {
                    MIR_LOG_ERROR("Mesh {}: staging buffer allocation failed, the upload is dropped", handle.mIndex);
                    resources.FreeMesh(*mesh);
                    mesh->mVertexBuffer = mesh->mIndexBuffer = VK_NULL_HANDLE;
                    mesh->mVertexAllocation = mesh->mIndexAllocation = VK_NULL_HANDLE;
                    mesh->mState = GpuMeshState::Failed;
                    ++mFailed;
                    continue;
                }
                source = own;
                target = static_cast<std::byte *>(info.pMappedData);
                targetAllocation = ownAllocation;
                mRetired.Push(frame, vertexBytes + indexBytes, Garbage{{}, {{own, ownAllocation}}});
            }
            std::memcpy(target, queued.source.vertices.data(), vertexBytes);
            std::memcpy(target + vertexBytes, queued.source.indices.data(), indexBytes);
            // CPU data can be released here, the acceleration structure build uses GPU buffers.
            queued.source.keepalive.reset();
            vmaFlushAllocation(mDevice->Allocator(), targetAllocation, entry.stagingOffset ? offset : 0,
                               vertexBytes + indexBytes);

            const VkBufferCopy vertexCopy{.srcOffset = offset, .dstOffset = 0, .size = vertexBytes};
            vkCmdCopyBuffer(cmd, source, mesh->mVertexBuffer, 1, &vertexCopy);
            const VkBufferCopy indexCopy{.srcOffset = offset + vertexBytes, .dstOffset = 0, .size = indexBytes};
            vkCmdCopyBuffer(cmd, source, mesh->mIndexBuffer, 1, &indexCopy);

            mesh->mIndexCount = indexCount;
            mesh->mState = GpuMeshState::Uploading;
            mUploadedBytes += entry.item.gpuBytes;
            if (queued.blas)
                builds.push_back({handle.mIndex, mesh, vertexCount});
        }

        if (!builds.empty())
        {
            const RayTracer::BlasBatch batch = rayTracer.RecordMeshBlases(cmd, builds);
            // A requested BLAS is part of readiness; failed copies still retire after this frame completes.
            for (const RayTracer::MeshBlasBuild &build : builds)
                if (!rayTracer.HasMeshBlas(build.meshIndex))
                    if (GpuMesh *mesh = resources.FindMeshSlot({build.meshIndex, build.mesh->mGeneration}))
                    {
                        mesh->mState = GpuMeshState::Failed;
                        ++mFailed;
                    }
            mBlasBuilt += batch.built;
            mBlasBytes += batch.blasBytes;
            if (batch.scratchBuffer != VK_NULL_HANDLE)
                mRetired.Push(frame, batch.scratchBytes, Garbage{{}, {{batch.scratchBuffer, batch.scratchAllocation}}});
        }

        // Later frames draw and trace from these, in submission order on the one queue.
        VkPipelineStageFlags2 srcStages = VK_PIPELINE_STAGE_2_COPY_BIT;
        VkAccessFlags2 srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkPipelineStageFlags2 dstStages =
            VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        VkAccessFlags2 dstAccess = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
        if (mDevice->RayTracingSupported())
        {
            srcStages |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            srcAccess |= VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            dstStages |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                         VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
            dstAccess |= VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT;
        }
        const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                       .srcStageMask = srcStages,
                                       .srcAccessMask = srcAccess,
                                       .dstStageMask = dstStages,
                                       .dstAccessMask = dstAccess};
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    void MeshUploader::Clear(RayTracer &rayTracer)
    {
        mQueue.Clear();
        mQueued.clear();
        mTextures.clear();
        mTextureFinishes.clear();
        mRetired.ReleaseAll([&](Garbage &garbage) { Free(rayTracer, garbage); });
        mRetiredTextures.ReleaseAll([&](Garbage &garbage) { Free(rayTracer, garbage); });
    }

    void MeshUploader::Free(RayTracer &rayTracer, Garbage &garbage)
    {
        if (garbage.blas.mBlas != VK_NULL_HANDLE || garbage.blas.mBuffer != VK_NULL_HANDLE)
            rayTracer.DestroyBLAS(garbage.blas);
        for (const auto &[buffer, allocation] : garbage.buffers)
            vmaDestroyBuffer(mDevice->Allocator(), buffer, allocation);
        garbage.buffers.clear();
        for (auto &texture : garbage.textures)
            ResourceStore::FreeTexture(*mDevice, texture);
        garbage.textures.clear();
    }

    void MeshUploader::FillStats(MeshStreamingStats &stats) const
    {
        stats.desc = mQueue.Desc();
        stats.pending = mQueue.Pending();
        stats.pendingBytes = mQueue.PendingBytes();
        stats.uploading = mQueue.Uploading();
        stats.uploadingBytes = mQueue.UploadingBytes();
        stats.retiring = mRetired.Size() + mRetiredTextures.Size();
        stats.retiringMeshBytes = mRetired.Bytes();
        stats.retiringTextureBytes = mRetiredTextures.Bytes();
        stats.retiringBytes = stats.retiringMeshBytes + stats.retiringTextureBytes;
        stats.stagingUsedBytes = mQueue.StagingUsed();
        stats.queued = mQueuedCount;
        stats.uploaded = mUploaded;
        stats.canceled = mCanceled;
        stats.retired = mRetiredCount;
        stats.failed = mFailed;
        stats.uploadedBytes = mUploadedBytes;
        stats.textureQueued = mTextureQueuedCount;
        stats.textureUploaded = mTextureUploadedCount;
        for (const auto &[key, texture] : mTextures)
            stats.textureSourceBytes += texture.mSource.Bytes();
        stats.blasBuilt = mBlasBuilt;
        stats.blasBytes = mBlasBytes;
        stats.lastFrameBytes = mQueue.LastFrameBytes();
        stats.maxFrameBytes = mMaxFrameBytes;
    }
} // namespace mir::vk

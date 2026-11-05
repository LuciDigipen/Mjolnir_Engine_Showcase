/**
 * @file ResourceStore.h
 * @author Sumin Park
 * @brief Meshes, textures, samplers and the default textures, behind slot/generation handles.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Material.h>
#include <renderer/Mesh.h>
#include <renderer/Texture.h>
#include <renderer/TextureTable.h>
#include <renderer/components/MeshBounds.h>

#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;

    // Where a mesh is on its way to the GPU; CreateMesh's are Ready at once.
    enum class GpuMeshState : uint8_t
    {
        Ready,
        Pending,
        Uploading,
        Failed,
    };

    // One buffer pair per mesh
    struct GpuMesh
    {
        MeshBounds mBounds;
        VkBuffer mVertexBuffer = VK_NULL_HANDLE;
        VmaAllocation mVertexAllocation = VK_NULL_HANDLE;
        VkBuffer mIndexBuffer = VK_NULL_HANDLE;
        VmaAllocation mIndexAllocation = VK_NULL_HANDLE;
        uint32_t mIndexCount = 0;
        // Set while this mesh is owed a BLAS, which the first trace drawing it builds; queued meshes get theirs with the upload.
        uint32_t mBlasVertexCount = 0;
        uint32_t mGeneration = 0; //< 0 = slot never filled
        GpuMeshState mState = GpuMeshState::Ready;
        // Buffers plus any BLAS; set for queued meshes.
        uint64_t mBytes = 0;
    };

    // One image per texture.
    struct GpuTexture
    {
        VkImage mImage = VK_NULL_HANDLE;
        VmaAllocation mAllocation = VK_NULL_HANDLE;
        VkImageView mView = VK_NULL_HANDLE;
        uint32_t mLevels = 1;
        uint32_t mGeneration = 0; //< 0 = slot never filled
        TextureUploadState mState = TextureUploadState::Ready;
        uint64_t mBytes = 0;
    };

    // Destroy* and Release* do not wait: the caller idles the device first,
    // since in-flight frames may still read what they free.
    class ResourceStore
    {
    public:
        // Uploads the default textures.
        bool Initialize(Device &device);
        void Shutdown();

        MeshHandle CreateMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices);
        // False when the handle is null or stale.
        bool DestroyMesh(MeshHandle handle);
        // Returns how many slots there were. Every slot's generation moves on, so no old handle resolves.
        std::size_t ReleaseMeshes();
        // Ready meshes only: what draws and acceleration structures may read.
        const GpuMesh *FindMesh(MeshHandle handle) const;
        // In any state.
        GpuMesh *FindMeshSlot(MeshHandle handle);

        // A Pending slot without buffers, for an asynchronous upload.
        MeshHandle ReserveMesh();
        // Device buffers for a reserved slot; rayTracing makes them BLAS build inputs.
        bool CreateMeshBuffers(GpuMesh &mesh, uint32_t slot, VkDeviceSize vertexBytes, VkDeviceSize indexBytes,
                               bool rayTracing);
        // Empties the slot and moves its generation on; the caller frees the returned buffers later.
        GpuMesh TakeMesh(MeshHandle handle);
        // GPU objects only; the slot and its generation are the caller's.
        void FreeMesh(GpuMesh &mesh);
        uint32_t HighestMeshGeneration() const;

        TextureHandle CreateTexture(const TextureDesc &desc);
        TextureHandle ReserveTexture(const TextureDesc &desc, uint32_t levels);
        GpuTexture *FindTextureSlot(TextureHandle handle);
        GpuTexture TakeTexture(TextureHandle handle);
        void FreeTexture(GpuTexture &texture) { FreeTexture(*mDevice, texture); }
        static void FreeTexture(Device &device, GpuTexture &texture);
        void MakeTextureReady(TextureHandle handle);
        // False when the handle is null, stale or a default texture.
        bool DestroyTexture(TextureHandle handle);
        // Every texture but the defaults; returns how many were released.
        std::size_t ReleaseTextures();
        const GpuTexture *FindTexture(TextureHandle handle) const;

        // The shared image-table row a handle names; the default white row for a stale handle.
        uint32_t TextureSlot(TextureHandle handle) const;
        VkImageView ImageViewAt(uint32_t slot) const;
        uint64_t TextureTableVersion() const { return mTextureTableVersion; }
        uint64_t TextureAllocatedBytes() const;

        TextureHandle DefaultWhiteTexture() const { return mDefaultWhiteTexture; }
        TextureHandle DefaultNormalTexture() const { return mDefaultNormalTexture; }
        TextureHandle ErrorTexture() const { return mErrorTexture; }

        // One VkSampler per distinct effective SamplerDesc, shared by every
        // material that asks for it; VK_NULL_HANDLE on failure. outSlot receives
        // the sampler table row.
        VkSampler AcquireSampler(const SamplerDesc &desc, uint32_t *outSlot = nullptr);
        VkSampler SamplerAt(uint32_t slot) const;
        uint32_t SamplerSlotCount() const { return static_cast<uint32_t>(mSamplerTable.size()); }
        VkSampler DefaultSampler() const { return mDefaultSampler; }
        uint64_t SamplerTableVersion() const { return mSamplerTableVersion; }

    private:
        bool CreateDefaultTextures();
        void DestroyTextures();
        void DestroySamplers();

        Device *mDevice = nullptr;

        std::vector<GpuMesh> mMeshes;
        std::vector<uint32_t> mFreeMeshIndices;
        uint32_t mHighestMeshGeneration = 0;

        TextureTable mTextureTable;
        std::vector<GpuTexture> mTextures;
        TextureHandle mDefaultWhiteTexture;
        TextureHandle mDefaultNormalTexture;
        TextureHandle mErrorTexture;
        uint64_t mTextureTableVersion = 0;

        // Row order is the sampler table's stable identity. The lookup maps the
        // effective sampler state to that row.
        std::vector<VkSampler> mSamplerTable;
        std::unordered_map<uint64_t, uint32_t> mSamplerLookup;
        VkSampler mDefaultSampler = VK_NULL_HANDLE;
        uint64_t mSamplerTableVersion = 0;
    };
}

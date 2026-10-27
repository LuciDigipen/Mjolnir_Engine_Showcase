/**
 * @file RayTracer.h
 * @author Sumin Park
 * @brief Ray tracing: per-mesh BLAS, per-frame TLAS, RT pipeline, SBT and output images.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/VulkanRenderer.h>
#include "Image2D.h"
#include "VulkanConstants.h"

#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;
    class ResourceStore;
    class MaterialStore;
    struct GpuMesh;

    // Every image the trace writes, one per frame in flight each. Adding one is a row in
    // RayTracer.cpp's kRtImages (format, binding, clear value, stage) plus the shader side.
    // The always-present outputs come first, then Ray Reconstruction's guide buffers.
    enum class RtImage : uint32_t
    {
        Output,
        // DepthOfField's linear ray distance.
        Distance,
        // MotionBlur's and Ray Reconstruction's per-pixel motion.
        Velocity,
        Albedo,
        Normal,
        SpecularAlbedo,
        // Reversed-Z projected depth, separate from Distance.
        DlssDepth,
        Count
    };

    // Only initialized when the device supports ray tracing.
    class RayTracer
    {
    public:
        // The output images match `extent`; the camera set is borrowed as set 1 of the pipeline layout.
        bool Initialize(Device &device, VkExtent2D extent, VkDescriptorSetLayout cameraSetLayout);
        void Shutdown();

        // Recreates the output images at the swapchain's new extent; the caller idles the device first.
        bool Resize(VkExtent2D extent);

        struct MeshBlas
        {
            VkAccelerationStructureKHR mBlas = VK_NULL_HANDLE;
            VkBuffer mBuffer = VK_NULL_HANDLE;
            VmaAllocation mAllocation = VK_NULL_HANDLE;
            VkDeviceAddress mAddress = 0;
        };

        struct BlasSizes
        {
            VkDeviceSize accelerationStructure = 0;
            VkDeviceSize scratch = 0;
        };

        // Indexed by mesh slot, so the resource store never sees ray tracing. False leaves the mesh raster-only.
        bool BuildMeshBlas(uint32_t meshIndex, const GpuMesh &mesh, uint32_t vertexCount);
        struct MeshBlasBuild
        {
            uint32_t meshIndex = 0;
            const GpuMesh *mesh = nullptr;
            uint32_t vertexCount = 0;
        };

        struct BlasBatch
        {
            size_t built = 0;
            VkDeviceSize blasBytes = 0;
            // Shared by the batch; it must outlive the frame.
            VkBuffer scratchBuffer = VK_NULL_HANDLE;
            VmaAllocation scratchAllocation = VK_NULL_HANDLE;
            VkDeviceSize scratchBytes = 0;
        };

        // One copy-to-build barrier, then every build in one call; callers check which BLASes were created.
        BlasBatch RecordMeshBlases(VkCommandBuffer cmd, std::span<const MeshBlasBuild> builds);
        bool HasMeshBlas(uint32_t meshIndex) const;
        BlasSizes MeshBlasSizes(uint32_t vertexCount, uint32_t indexCount) const;
        // No-op for a slot that never had a BLAS.
        void DestroyMeshBlas(uint32_t meshIndex);
        // Moves the slot's BLAS out for a later DestroyBLAS; empty when it has none.
        MeshBlas TakeMeshBlas(uint32_t meshIndex);
        void DestroyBLAS(MeshBlas &blas);
        void DestroyBlases();

        struct PreparedScene
        {
            VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
            VkDescriptorSet descriptors = VK_NULL_HANDLE;
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            uint32_t instanceCount = 0;
            bool truncated = false;
        };

        PreparedScene PrepareScene(VkCommandBuffer cmd, uint32_t frame, std::span<const DrawItem> items,
            const ResourceStore &resources, const MaterialStore &materials);

        // Traces the already prepared frame's TLAS into the output image,
        // which ends in SHADER_READ_ONLY for the caller to sample (PostProcess's tonemap pass).
        // aaMode selects rayGenMain's branch: Off/Dlss trace one sample per pixel (Dlss's jitter
        // already rode in through cameraSet's CameraData), Msaa loops rtSampleCount (1/2/4/8) standard-pattern
        // sub-pixel samples and averages color only - distance/velocity keep the first sample.
        void RecordTrace(VkCommandBuffer cmd, uint32_t frame, const VkRect2D &rect,
            VkDescriptorSet cameraSet, uint32_t cameraOffset, AntiAliasingMode aaMode, uint32_t rtSampleCount);

        // VK_NULL_HANDLE for a guide buffer while Dlss is inactive.
        VkImage Image(RtImage image, uint32_t frame) const { return Slot(image, frame).image; }
        VkImageView View(RtImage image, uint32_t frame) const { return Slot(image, frame).view; }

        // Ray Reconstruction's guide buffers. They exist only while Dlss is the active mode -
        // their bindings are partially bound, so the set stays valid without them. The caller
        // idles the device first; Resize keeps whichever state it finds.
        bool CreateDlssGuides();
        void DestroyDlssGuides();
        bool HasDlssGuides() const { return Image(RtImage::Albedo, 0) != VK_NULL_HANDLE; }

    private:
        bool CreateAccelerationStructureObject(VkDeviceSize size, VkAccelerationStructureTypeKHR type,
            VkBuffer &outBuffer, VmaAllocation &outAllocation, VkAccelerationStructureKHR &outAS);
        const Image2D &Slot(RtImage image, uint32_t frame) const
        {
            return mImages[static_cast<uint32_t>(image)][frame];
        }
        // The always-present outputs or the guide buffers, at extent.
        bool CreateImages(bool guides, VkExtent2D extent);
        void DestroyImages(bool guides);
        bool CreateOutputImages(VkExtent2D extent);
        // At mExtent, which CreateOutputImages records.
        bool CreateGuideImages() { return CreateImages(true, mExtent); }
        bool CreateScene();
        void DestroyScene();
        bool CreatePipeline(VkDescriptorSetLayout cameraSetLayout);
        void WriteDescriptorSets();
        bool BuildBLAS(const GpuMesh &mesh, uint32_t vertexCount, VkCommandBuffer cmd, MeshBlas &outBlas,
            VkBuffer &outScratchBuffer, VmaAllocation &outScratchAllocation);
        // Aligned to minAccelerationStructureScratchOffsetAlignment.
        bool CreateScratchBuffer(VkDeviceSize size, VkBuffer &outBuffer, VmaAllocation &outAllocation);
        PreparedScene RecordTlasBuild(VkCommandBuffer cmd, uint32_t frame, std::span<const DrawItem> items,
            const ResourceStore &resources, const MaterialStore &materials);

        Device *mDevice = nullptr;
        VkDeviceSize mScratchAlignment = 1;

        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{};
        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;

        VkBuffer mSbtBuffer = VK_NULL_HANDLE;
        VmaAllocation mSbtAllocation = VK_NULL_HANDLE;
        VkStridedDeviceAddressRegionKHR mSbtRaygenRegion{};
        VkStridedDeviceAddressRegionKHR mSbtMissRegion{};
        VkStridedDeviceAddressRegionKHR mSbtHitRegion{};
        VkStridedDeviceAddressRegionKHR mSbtCallableRegion{};

        constexpr static uint32_t kMaxInstances = 16384;
        bool mWarnedInstanceCap = false;

        std::vector<VkDescriptorImageInfo> mTextureInfos;
        bool mWarnedTextureCap = false;

        VkExtent2D mExtent{};

        Image2D mImages[static_cast<uint32_t>(RtImage::Count)][kFramesInFlight]{};

        VkAccelerationStructureKHR mTlas[kFramesInFlight]{};
        VkBuffer mTlasBuffers[kFramesInFlight]{};
        VmaAllocation mTlasAllocations[kFramesInFlight]{};

        VkBuffer mTlasScratchBuffers[kFramesInFlight]{};
        VmaAllocation mTlasScratchAllocations[kFramesInFlight]{};
        VkDeviceAddress mTlasScratchAddresses[kFramesInFlight]{};

        VkBuffer mInstanceBuffers[kFramesInFlight]{};
        VmaAllocation mInstanceAllocations[kFramesInFlight]{};
        std::byte *mInstanceMapped[kFramesInFlight]{};

        VkBuffer mInstanceInfoBuffers[kFramesInFlight]{};
        VmaAllocation mInstanceInfoAllocations[kFramesInFlight]{};
        std::byte *mInstanceInfoMapped[kFramesInFlight]{};

        std::vector<MeshBlas> mBlases;
    };
}

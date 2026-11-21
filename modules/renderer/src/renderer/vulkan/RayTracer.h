/**
 * @file RayTracer.h
 * @author Sumin Park
 * @brief Ray tracing: per-mesh BLAS, per-frame TLAS, RT pipeline, SBT and output images.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/SurfaceMaterialCatalog.h>
#include <renderer/VulkanRenderer.h>
#include "Image2D.h"
#include "VulkanConstants.h"

#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;
    class ResourceStore;
    class MaterialStore;
    class MaterialTable;
    struct GpuMesh;

    // Static mesh BLASes are compacted after their build; deformation BLASes are rebuilt every frame.
    constexpr VkBuildAccelerationStructureFlagsKHR kMeshBlasBuildFlags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    constexpr VkBuildAccelerationStructureFlagsKHR kDeformationBlasBuildFlags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;

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
        // The output images match `extent`; the camera set is borrowed as set 1 of the pipeline
        // layout. The shared table supplies set 0's image, sampler and record rows.
        bool Initialize(Device &device, VkExtent2D extent, VkDescriptorSetLayout cameraSetLayout,
            MaterialTable &table, std::span<const SurfaceMaterialRegistration> surfaceMaterials);
        void Shutdown();

        // Recreates the output images at the swapchain's new extent; the caller idles the device first.
        bool Resize(VkExtent2D extent);

        // Shader reload builds a whole replacement pipeline and SBT off to the
        // side. Descriptor sets, images, TLAS and BLAS stay untouched.
        bool PrepareShaderReload(std::span<const SurfaceMaterialRegistration> candidateMaterials,
            std::string *error = nullptr);
        void CommitShaderReload();
        void DiscardShaderReload();
        bool ShaderReloadPrepared() const { return mReloadPrepared; }
        // The caller idles the device first. Keeps SBT group order aligned with
        // the surface catalog the material store just reloaded.
        void SetSurfaceMaterials(std::span<const SurfaceMaterialRegistration> materials);

        struct MeshBlas
        {
            VkAccelerationStructureKHR mBlas = VK_NULL_HANDLE;
            VkBuffer mBuffer = VK_NULL_HANDLE;
            VmaAllocation mAllocation = VK_NULL_HANDLE;
            VkDeviceAddress mAddress = 0;
            VkDeviceSize mBytes = 0;
            // Proves a scheduled compaction still names this BLAS, not one that reused the slot.
            uint64_t mSerial = 0;
        };

        struct BlasSizes
        {
            VkDeviceSize accelerationStructure = 0;
            VkDeviceSize scratch = 0;
        };

        // Indexed by mesh slot, so the resource store never sees ray tracing. False leaves the mesh raster-only.
        // The compacted size is zero when it could not be read; the caller schedules the compaction.
        bool BuildMeshBlas(uint32_t meshIndex, const GpuMesh &mesh, uint32_t vertexCount,
            VkDeviceSize &outCompactedBytes);
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
            // Slots built in this batch, and the scratch the batch shares; the scratch must outlive the frame.
            std::vector<uint32_t> slots;
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

        struct MeshBlasInfo
        {
            bool exists = false;
            uint64_t serial = 0;
            VkDeviceSize bytes = 0;
        };
        MeshBlasInfo BlasInfo(uint32_t meshIndex) const;

        // Records the build barrier and the compacted-size queries for the slots just built; the pool is the caller's.
        void RecordMeshBlasCompactedSizes(VkCommandBuffer cmd, VkQueryPool pool,
            std::span<const uint32_t> meshIndices);

        struct BlasCompaction
        {
            bool compacted = false;
            // The uncompacted BLAS the slot held; the caller retires it once its frame has completed.
            MeshBlas retired;
        };
        // Replaces the slot's BLAS with a compacted copy; false leaves the slot as it was.
        BlasCompaction CompactMeshBlas(VkCommandBuffer cmd, uint32_t meshIndex, uint64_t serial,
            VkDeviceSize compactedSize);

        struct BlasCompactionStats
        {
            uint64_t compacted = 0;
            uint64_t savedBytes = 0;
        };
        BlasCompactionStats CompactionStats() const { return { mBlasCompacted, mBlasCompactedSavedBytes }; }

        struct PreparedScene
        {
            VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
            VkDescriptorSet descriptors = VK_NULL_HANDLE;
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            uint32_t instanceCount = 0;
            bool truncated = false; // Requested scene exceeded capacity, even if camera fallback succeeded.
            // False when an instance offset is outside the ordered material catalog.
            bool giCompatible = true;
        };

        PreparedScene PrepareScene(VkCommandBuffer cmd, uint32_t frame, std::span<const DrawItem> items,
            const ResourceStore &resources, const MaterialStore &materials,
            std::optional<std::span<const DrawItem>> fallbackItems = std::nullopt);
        VkDescriptorSetLayout SceneLayout() const { return mSetLayout; }

        // Borrowed from Ddgi for this frame's final camera trace only.
        struct GiShading
        {
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkPipelineLayout layout = VK_NULL_HANDLE;
            VkDescriptorSet descriptors = VK_NULL_HANDLE;
            VkStridedDeviceAddressRegionKHR raygen{}, miss{}, hit{}, callable{};
            float intensity = 1.0f;
            float weight = 0.0f;
        };

        // Traces the already prepared frame's TLAS into the output image,
        // which ends in SHADER_READ_ONLY for the caller to sample (PostProcess's tonemap pass).
        // aaMode selects rayGenMain's branch: Off/Dlss trace one sample per pixel (Dlss's jitter
        // already rode in through cameraSet's CameraData), Msaa loops rtSampleCount (1/2/4/8) standard-pattern
        // sub-pixel samples and averages color only - distance/velocity keep the first sample.
        void RecordTrace(VkCommandBuffer cmd, uint32_t frame, const VkRect2D &rect,
            VkDescriptorSet cameraSet, uint32_t cameraOffset, AntiAliasingMode aaMode, uint32_t rtSampleCount,
            const GiShading *gi = nullptr);

        // VK_NULL_HANDLE for a guide buffer while Dlss is inactive.
        VkImage Image(RtImage image, uint32_t frame) const { return Slot(image, frame).image; }
        VkImageView View(RtImage image, uint32_t frame) const { return Slot(image, frame).view; }

        // Ray Reconstruction's guide buffers. They exist only while Dlss is the active mode -
        // their bindings are partially bound, so the set stays valid without them. The caller
        // idles the device first; Resize keeps whichever state it finds.
        bool CreateDlssGuides();
        void DestroyDlssGuides();
        bool HasDlssGuides() const { return Image(RtImage::Albedo, 0) != VK_NULL_HANDLE; }

        // Called only after the frame slot's fence has completed.
        void ResetDeformations(uint32_t frame, const ResourceStore *resources = nullptr);
        RtDeformationStats DeformationStats() const { return mDeformationStats; }

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
        void WriteMaterialTableDescriptors(uint32_t frame);
        bool BuildBLAS(const GpuMesh &mesh, uint32_t vertexCount, VkCommandBuffer cmd, MeshBlas &outBlas,
            VkBuffer &outScratchBuffer, VmaAllocation &outScratchAllocation,
            VkBuildAccelerationStructureFlagsKHR flags);
        // Aligned to minAccelerationStructureScratchOffsetAlignment.
        bool CreateScratchBuffer(VkDeviceSize size, VkBuffer &outBuffer, VmaAllocation &outAllocation);
        bool InitializeDeformation();
        bool CreateDeformationPipeline(VkPipeline &pipeline);
        void ShutdownDeformation();

        struct DeformedMesh
        {
            VkBuffer vertices = VK_NULL_HANDLE;
            VmaAllocation vertexAllocation = VK_NULL_HANDLE;
            VkBuffer deformation = VK_NULL_HANDLE;
            VmaAllocation deformationAllocation = VK_NULL_HANDLE;
            VkBuffer previousVertices = VK_NULL_HANDLE;
            VmaAllocation previousVertexAllocation = VK_NULL_HANDLE;
            VkBuffer previousDeformation = VK_NULL_HANDLE;
            VmaAllocation previousDeformationAllocation = VK_NULL_HANDLE;
            MeshBlas blas;
            VkBuffer scratch = VK_NULL_HANDLE;
            VmaAllocation scratchAllocation = VK_NULL_HANDLE;
        };
        struct SettledMesh
        {
            MeshHandle source;
            std::vector<glm::vec4> sample;
            DeformedMesh geometry;
            uint64_t lastUsed = 0;
        };
        void DestroyDeformedMesh(DeformedMesh &mesh);
        // Slot-local caches avoid cross-frame writes/lifetime hazards. Each source
        // retains two poses (e.g. gear up/down), shared by draws and instances.
        std::vector<SettledMesh> mSettledMeshes[kFramesInFlight];
        uint64_t mDeformationEpoch[kFramesInFlight]{};
        RtDeformationStats mDeformationStats;
        const DeformedMesh *RecordDeformedMesh(VkCommandBuffer cmd, uint32_t frame,
            const GpuMesh &mesh, std::span<const glm::vec4> deformation,
            std::span<const glm::vec4> previousDeformation);
        const DeformedMesh *RecordSettledMesh(VkCommandBuffer cmd, uint32_t frame,
            MeshHandle source, const GpuMesh &mesh, std::span<const glm::vec4> deformation);
        PreparedScene RecordTlasBuild(VkCommandBuffer cmd, uint32_t frame, std::span<const DrawItem> items,
            const ResourceStore &resources, const MaterialStore &materials,
            std::optional<std::span<const DrawItem>> fallbackItems);

        Device *mDevice = nullptr;
        MaterialTable *mTable = nullptr;
        std::vector<SurfaceMaterialRegistration> mSurfaceMaterials;
        VkDeviceSize mScratchAlignment = 1;

        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{};
        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;
        VkPipelineLayout mDeformationLayout = VK_NULL_HANDLE;
        VkPipeline mDeformationPipeline = VK_NULL_HANDLE;
        std::vector<DeformedMesh> mDeformedMeshes[kFramesInFlight];

        VkBuffer mSbtBuffer = VK_NULL_HANDLE;
        VmaAllocation mSbtAllocation = VK_NULL_HANDLE;
        VkStridedDeviceAddressRegionKHR mSbtRaygenRegion{};
        VkStridedDeviceAddressRegionKHR mSbtMissRegion{};
        VkStridedDeviceAddressRegionKHR mSbtHitRegion{};
        VkStridedDeviceAddressRegionKHR mSbtCallableRegion{};

        constexpr static uint32_t kMaxInstances = 16384;
        bool mWarnedInstanceCap = false;

        uint64_t mTableVersion[kFramesInFlight]{};

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
        uint64_t mBlasSerial = 0;
        uint64_t mBlasCompacted = 0;
        uint64_t mBlasCompactedSavedBytes = 0;

        struct ShaderReloadCandidate
        {
            VkPipeline mPipeline = VK_NULL_HANDLE;
            VkPipeline mDeformationPipeline = VK_NULL_HANDLE;
            VkBuffer mSbtBuffer = VK_NULL_HANDLE;
            VmaAllocation mSbtAllocation = VK_NULL_HANDLE;
            VkStridedDeviceAddressRegionKHR mRaygenRegion{};
            VkStridedDeviceAddressRegionKHR mMissRegion{};
            VkStridedDeviceAddressRegionKHR mHitRegion{};
            VkStridedDeviceAddressRegionKHR mCallableRegion{};
            bool mValid = false;
        };

        bool BuildShaderReloadBundle(ShaderReloadCandidate &candidate, std::string *error);
        std::vector<SurfaceMaterialRegistration> mReloadSurfaceMaterials;
        ShaderReloadCandidate mReloadCandidate;
        bool mReloadPrepared = false;
    };
}

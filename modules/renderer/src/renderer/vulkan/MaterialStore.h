/**
 * @file MaterialStore.h
 * @author Sumin Park
 * @brief Materials, their pipelines, set 0 and the pipeline layout every material shares.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Material.h>
#include <renderer/MaterialRecordTable.h>
#include <renderer/MaterialReflection.h>
#include <renderer/MeshStreaming.h>
#include <renderer/SurfaceMaterialCatalog.h>

#include "VulkanConstants.h"

#include <glm/vec4.hpp>
#include <volk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;
    class ResourceStore;
    class MaterialTable;

    struct GpuMaterial
    {
        VkPipeline mPipeline = VK_NULL_HANDLE;
        VkDescriptorSet mDescriptorSet = VK_NULL_HANDLE;
        // alphaMode, alphaCutoff, doubleSided, unused. Handed to the model
        // shader through set 2, since the push constant range is full.
        glm::vec4 mParameters{};
        bool mModelShader = false;
        bool mSurfaceMaterial = false;
        uint32_t mRayTracingSbtOffset = 0;
        uint32_t mGeneration = 0;
        bool mUnlit = false;
        VkCullModeFlags mCullMode = VK_CULL_MODE_BACK_BIT;
        VkFrontFace mFrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        VkImageView mBaseColorView = VK_NULL_HANDLE;
        VkSampler mSampler = VK_NULL_HANDLE;
        uint32_t mBaseColorSlot = 0;
        uint32_t mSamplerSlot = 0;
        bool mError = false;

        // Absent on a device without the shared-table features.
        MaterialRecordId mRecordId;
        uint32_t mRecordOffset = kInvalidMaterialRecordOffset;
        uint32_t mMaterialIndex = 0;
        // Backing bytes for a record packed from the catalog at creation.
        std::vector<std::byte> mRecordBytes;

        // Replayable recipe: everything CreateMaterial received, minus the
        // borrowed record span, so a reload can rebuild the same pipeline.
        MaterialDesc mDesc;
        MaterialLayout mAcceptedLayout;
        bool mHasAcceptedLayout = false;
        // Authoritative current values: an explicit desc.record or the latest
        // UpdateRecord, never the stale initial catalog bytes.
        std::vector<std::byte> mCurrentRecordBytes;
        VkPipeline mSurfacePipeline = VK_NULL_HANDLE;
        bool mSurfaceTried = false;
    };

    // Destroy* and Release* do not wait: the caller idles the device first,
    // since in-flight frames may still use the pipelines and sets they free.
    class MaterialStore
    {
    public:
        static constexpr uint32_t kMaxMaterials = 1024;

        // The published record location a surface draw reads this frame.
        MaterialRecordRef RecordRef(const GpuMaterial &material) const;

        // sharedLayouts are sets 1-3, borrowed for the pipeline layouts; their owners destroy them.
        // table is null on a device without descriptor indexing. drawRecordSetLayout is set 5 and
        // only joins the layout beside the table. Also creates the default and error materials
        // and the sky pipeline, which reads set 1 alone.
        bool Initialize(Device &device, ResourceStore &resources, MaterialTable *table, VkFormat colorFormat,
            const std::array<VkDescriptorSetLayout, 3> &sharedLayouts,
            VkDescriptorSetLayout drawRecordSetLayout);
        void Shutdown();

        MaterialHandle CreateMaterial(MaterialDesc desc, ResourceStore &resources, VkFormat colorFormat);
        // False when the handle is null, stale, the default or the error material.
        bool DestroyMaterial(MaterialHandle handle);
        // Raw record bytes for this material; published at the next frame.
        bool UpdateRecord(MaterialHandle handle, std::span<const std::byte> bytes);
        // All-or-none writes: Prepare validates every update and copies its bytes
        // off to the side; Commit moves the prepared bytes in without allocating,
        // so no frontend is left changed by a later failure.
        bool PrepareRecordBatch(std::span<const MaterialRecordUpdate> updates, std::string *error = nullptr);
        void CommitRecordBatch();
        void DiscardRecordBatch();
        bool RecordBatchPrepared() const { return mRecordBatchPrepared; }
        // The generation-checked accepted layout of a live surface material and
        // the authoritative bytes a draw reads. The store keeps the handle.
        bool AcceptedRecord(MaterialHandle handle, MaterialLayout &layout, std::span<const std::byte> &bytes) const;
        void SetRecordValidator(MaterialRecordValidator validator);
        // After BeginFrame's wait, before recording: publishes staged record bytes.
        void PublishRecords(uint64_t completedFrame, uint64_t frame);
        // Publishes replacement sets after texture readiness, old sets survive their submitted frames.
        void RefreshTextures(ResourceStore &resources, uint64_t completed, uint64_t lastUse);
        // Every material but the default and error ones, which move to slots 0 and 1; returns how many were released.
        std::size_t ReleaseMaterials();

        // All-or-none shader reload. Prepare builds every replacement pipeline
        // and migrated record off to the side; only Commit swaps them. The
        // caller idles the device before Prepare and again before Commit.
        bool PrepareShaderReload(const SurfaceMaterialCatalog &candidateCatalog, std::string *error = nullptr);
        void CommitShaderReload();
        void DiscardShaderReload();
        bool ShaderReloadPrepared() const { return mReloadPrepared; }
        // Reloads surface_materials.json so a newly compiled material is
        // registered before the batch rebuilds pipelines and RT hit groups.
        bool ReloadSurfaceMaterialCatalog(std::string *error = nullptr);

        // No free slot: released materials give theirs back when their frames complete
        bool SlotsExhausted() const { return mFreeIndices.empty() && mMaterials.size() >= kMaxMaterials; }
        const GpuMaterial *FindMaterial(MaterialHandle handle) const;
        // What a draw with `handle` uses: the material; else the default for a null handle and the error
        // material for a stale one; null only when both are gone.
        const GpuMaterial *FindOrDefault(MaterialHandle handle) const;
        bool SupportsSurfaceInputs(MaterialHandle handle) const;
        VkPipeline SurfacePipeline(MaterialHandle handle);
        void ReleaseSurfacePipelines(); // Caller has idled the device.

        MaterialHandle DefaultMaterial() const { return mDefaultMaterial; }
        MaterialHandle ErrorMaterial() const { return mErrorMaterial; }
        const SurfaceMaterialCatalog &SurfaceMaterials() const { return mSurfaceMaterials; }
        VkPipelineLayout PipelineLayout() const { return mPipelineLayout; }

        VkPipeline SkyPipeline() const { return mSkyPipeline; }
        // Merged opaque pipeline for the sky, built on first use.
        VkPipeline SkySurfacePipeline();
        VkPipelineLayout SkyPipelineLayout() const { return mSkyPipelineLayout; }

    private:
        bool CreatePipelineLayout(const std::array<VkDescriptorSetLayout, 3> &sharedLayouts,
            VkDescriptorSetLayout drawRecordSetLayout);
        // The state a material pipeline and the sky pipeline differ in.
        struct PipelineState
        {
            std::string_view shaderName;
            VkPipelineVertexInputStateCreateInfo vertexInput{};
            VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineRasterizationStateCreateInfo rasterizer{};
            VkPipelineColorBlendAttachmentState blend{};
            VkPipelineDepthStencilStateCreateInfo depthStencil{};
            VkPipelineLayout layout = VK_NULL_HANDLE;
        };

        enum class PipelineTarget { Color, OpaqueSsgi };

        VkPipeline CreateGraphicsPipeline(std::string_view vertexStem, std::string_view fragmentStem,
            const PipelineState &state,
            std::span<const VkFormat> colorFormats, std::span<const VkPipelineColorBlendAttachmentState> blends);
        VkPipeline BuildPipeline(const MaterialDesc &desc, VkFormat colorFormat,
            PipelineTarget target = PipelineTarget::Color);
        VkPipeline BuildSkyPipeline(bool surfaces = false);
        bool CreateSkyPipeline(VkDescriptorSetLayout cameraSetLayout);
        // GPU objects only; the slot and its generation are the caller's.
        void FreeMaterial(GpuMaterial &material);
        void DestroyMaterials();

        struct ReloadEntry
        {
            uint32_t mIndex = 0;
            VkPipeline mPipeline = VK_NULL_HANDLE;
            VkPipeline mSurfacePipeline = VK_NULL_HANDLE;
            MaterialRecordId mRecordId;
            uint32_t mRecordOffset = kInvalidMaterialRecordOffset;
            std::vector<std::byte> mRecordBytes;
            MaterialLayout mLayout;
            bool mHasLayout = false;
            uint32_t mRayTracingSbtOffset = UINT32_MAX;
        };

        bool PrepareMaterialReloadEntry(GpuMaterial &material, ReloadEntry &entry, std::string *error);
        void DiscardReloadEntries();

        Device *mDevice = nullptr;
        MaterialTable *mTable = nullptr;
        SurfaceMaterialCatalog mSurfaceMaterials;
        MaterialRecordValidator mRecordValidator;
        VkFormat mColorFormat = VK_FORMAT_UNDEFINED;

        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;

        VkPipelineLayout mSkyPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mSkyPipeline = VK_NULL_HANDLE;
        VkPipeline mSkySurfacePipeline = VK_NULL_HANDLE;
        bool mSkySurfaceTried = false;

        // set 0: binding 0 = base color, binding 1 = normal map.
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mDescriptorPool = VK_NULL_HANDLE;

        std::vector<GpuMaterial> mMaterials;
        std::vector<MaterialHandle> mTextureWaiting;
        FrameRetireList<VkDescriptorSet> mRetiredTextureSets;
        std::vector<uint32_t> mFreeIndices;

        MaterialHandle mDefaultMaterial;
        MaterialHandle mErrorMaterial;

        struct PreparedRecord
        {
            MaterialHandle mHandle;
            MaterialRecordId mRecordId;
            std::vector<std::byte> mBytes;
        };

        std::vector<ReloadEntry> mReloadEntries;
        std::vector<PreparedRecord> mPreparedRecords;
        bool mRecordBatchPrepared = false;
        SurfaceMaterialCatalog mReloadCatalog;
        bool mReloadCatalogStaged = false;
        VkPipeline mReloadSkyPipeline = VK_NULL_HANDLE;
        VkPipeline mReloadSkySurfacePipeline = VK_NULL_HANDLE;
        bool mReloadPrepared = false;
    };
}

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
    };

    // Destroy* and Release* do not wait: the caller idles the device first,
    // since in-flight frames may still use the pipelines and sets they free.
    class MaterialStore
    {
    public:
        static constexpr uint32_t kMaxMaterials = 1024;
        static constexpr std::string_view kDefaultSurfaceShaderName = "default_DefaultMaterial";

        // Set 0's dynamic record-reference row for a material in one frame slot.
        // The stride is aligned to minUniformBufferOffsetAlignment at Initialize.
        uint32_t RecordRefDynamicOffset(uint32_t frame, uint32_t material) const
        {
            return frame * mRecordRefFrameStride + material * mRecordRefStride;
        }

        // sharedLayouts are sets 1-3, borrowed for the pipeline layouts; their owners destroy them.
        // table is null on a device without descriptor indexing. Also creates the default and
        // error materials and the sky pipeline, which reads set 1 alone.
        bool Initialize(Device &device, ResourceStore &resources, MaterialTable *table, VkFormat colorFormat,
            const std::array<VkDescriptorSetLayout, 3> &sharedLayouts);
        void Shutdown();

        MaterialHandle CreateMaterial(const MaterialDesc &desc, ResourceStore &resources, VkFormat colorFormat);
        // False when the handle is null, stale, the default or the error material.
        bool DestroyMaterial(MaterialHandle handle);
        // Raw record bytes for this material; published at the next frame.
        bool UpdateRecord(MaterialHandle handle, std::span<const std::byte> bytes);
        // After BeginFrame's wait, before recording: publishes staged record bytes,
        // then refreshes this frame slot's record-reference rows.
        void PublishRecords(uint64_t completedFrame, uint64_t frame, uint32_t frameIndex);
        // Every material but the default and error ones, which move to slots 0 and 1; returns how many were released.
        std::size_t ReleaseMaterials();

        const GpuMaterial *FindMaterial(MaterialHandle handle) const;
        // What a draw with `handle` uses: the material; else the default for a null handle and the error
        // material for a stale one; null only when both are gone.
        const GpuMaterial *FindOrDefault(MaterialHandle handle) const;

        MaterialHandle DefaultMaterial() const { return mDefaultMaterial; }
        MaterialHandle ErrorMaterial() const { return mErrorMaterial; }
        VkPipelineLayout PipelineLayout() const { return mPipelineLayout; }

        VkPipeline SkyPipeline() const { return mSkyPipeline; }
        VkPipelineLayout SkyPipelineLayout() const { return mSkyPipelineLayout; }

    private:
        bool CreatePipelineLayout(const std::array<VkDescriptorSetLayout, 3> &sharedLayouts);
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

        VkPipeline CreateGraphicsPipeline(const PipelineState &state, VkFormat colorFormat);
        VkPipeline BuildPipeline(const MaterialDesc &desc, VkFormat colorFormat);
        bool CreateSkyPipeline(VkDescriptorSetLayout cameraSetLayout, VkFormat colorFormat);
        bool CreateRecordRefBuffer();
        void DestroyRecordRefBuffer();
        void WriteRecordRef(uint32_t frame, uint32_t material);
        void WriteRecordRefEverywhere(uint32_t material);
        void RefreshRecordRefs(uint32_t frame);
        // GPU objects only; the slot and its generation are the caller's.
        void FreeMaterial(GpuMaterial &material);
        void DestroyMaterials();

        Device *mDevice = nullptr;
        MaterialTable *mTable = nullptr;

        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;

        VkPipelineLayout mSkyPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mSkyPipeline = VK_NULL_HANDLE;

        // set 0: binding 0 = base color, binding 1 = normal map,
        // binding 2 = this frame's record-reference row (dynamic).
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mDescriptorPool = VK_NULL_HANDLE;

        VkBuffer mRecordRefBuffer = VK_NULL_HANDLE;
        VmaAllocation mRecordRefAllocation = VK_NULL_HANDLE;
        std::byte *mRecordRefMapped = nullptr;
        uint32_t mRecordRefStride = sizeof(MaterialRecordRef);
        uint32_t mRecordRefFrameStride = 0;

        std::vector<GpuMaterial> mMaterials;
        std::vector<uint32_t> mFreeIndices;

        MaterialHandle mDefaultMaterial;
        MaterialHandle mErrorMaterial;
    };
}

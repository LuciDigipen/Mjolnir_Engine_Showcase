/**
 * @file RayTracer.cpp
 * @author Sumin Park
 * @brief Ray tracing: BLAS, TLAS, SBT, RT pipeline and output images.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "RayTracer.h"
#include "RayFacing.h"
#include "Device.h"
#include "GpuTiming.h"
#include <renderer/RtInstanceInfo.h>
#include "MaterialStore.h"
#include "MaterialTable.h"
#include "MaterialTableLayout.h"
#include "ResourceStore.h"
#include "VulkanUtil.h"
#include "VulkanPushConstants.h"

#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/math/Align.h>

#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace mir::vk
{
    namespace
    {
        // RtInstanceInfo::flags; keep in sync with raytrace.slang.
        constexpr uint32_t kInstanceUnlit = 1u;
        constexpr uint32_t kInstanceTextured = 2u;
        constexpr uint32_t kInstanceError = 4u;
        constexpr uint32_t kInstanceAlphaTest = 8u;
        constexpr uint32_t kInstanceDoubleSided = 16u;
        constexpr uint32_t kInstanceRasterFacingCorrection = 32u;

        VkTransformMatrixKHR ToTransformMatrixKHR(const glm::mat4 &m)
        {
            VkTransformMatrixKHR out{};
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 4; ++col)
                    out.matrix[row][col] = m[col][row];
            return out;
        }

        VkDeviceAddress GetBufferAddress(VkDevice device, VkBuffer buffer)
        {
            const VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .buffer = buffer };
            return vkGetBufferDeviceAddress(device, &info);
        }

        struct AccelBuildSizes
        {
            VkDeviceSize accelerationStructureSize = 0;
            VkDeviceSize buildScratchSize = 0;
        };

        AccelBuildSizes QueryAccelBuildSizes(VkDevice device,
            const VkAccelerationStructureBuildGeometryInfoKHR &buildInfo, uint32_t maxPrimitiveCount)
        {
            VkAccelerationStructureBuildSizesInfoKHR sizes{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
            vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                &buildInfo, &maxPrimitiveCount, &sizes);
            return { sizes.accelerationStructureSize, sizes.buildScratchSize };
        }
    }

    bool RayTracer::Initialize(Device &device, VkExtent2D extent, VkDescriptorSetLayout cameraSetLayout,
        MaterialTable &table, std::span<const SurfaceMaterialRegistration> surfaceMaterials)
    {
        mDevice = &device;
        mTable = &table;
        mSurfaceMaterials.assign(surfaceMaterials.begin(), surfaceMaterials.end());
        std::fill(std::begin(mTableVersion), std::end(mTableVersion), UINT64_MAX);

        VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 properties{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               .pNext = &accelerationProperties };
        vkGetPhysicalDeviceProperties2(mDevice->Physical(), &properties);
        mScratchAlignment =
            std::max<VkDeviceSize>(accelerationProperties.minAccelerationStructureScratchOffsetAlignment, 1);

        if (!CreateOutputImages(extent) || !CreateScene() || !CreatePipeline(cameraSetLayout))
            return false;

        WriteDescriptorSets();
        return true;
    }

    void RayTracer::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        DestroyBlases();
        DestroyScene();

        if (mSbtBuffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(mDevice->Allocator(), mSbtBuffer, mSbtAllocation);
            mSbtBuffer = VK_NULL_HANDLE;
            mSbtAllocation = VK_NULL_HANDLE;
        }

        if (mPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice->Handle(), mPipeline, nullptr);
            mPipeline = VK_NULL_HANDLE;
        }

        if (mPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(mDevice->Handle(), mPipelineLayout, nullptr);
            mPipelineLayout = VK_NULL_HANDLE;
        }

        if (mPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(mDevice->Handle(), mPool, nullptr);
            mPool = VK_NULL_HANDLE;
        }

        if (mSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice->Handle(), mSetLayout, nullptr);
            mSetLayout = VK_NULL_HANDLE;
        }

        DestroyImages(/*guides*/ true);
        DestroyImages(/*guides*/ false);
    }

    bool RayTracer::Resize(VkExtent2D extent)
    {
        const bool hadGuides = HasDlssGuides();
        DestroyImages(/*guides*/ true);
        DestroyImages(/*guides*/ false);
        if (!CreateOutputImages(extent) || (hadGuides && !CreateGuideImages()))
            return false;

        WriteDescriptorSets();
        return true;
    }

    bool RayTracer::CreateDlssGuides()
    {
        if (mDevice == nullptr)
            return false;
        if (HasDlssGuides())
            return true;
        if (!CreateGuideImages())
        {
            DestroyImages(/*guides*/ true);
            return false;
        }

        WriteDescriptorSets();
        return true;
    }

    void RayTracer::DestroyDlssGuides()
    {
        // The guide bindings keep pointing at the destroyed views; partially bound makes that legal
        // as long as no Dlss-mode trace runs, and Record never traces Dlss without guides.
        if (mDevice != nullptr)
            DestroyImages(/*guides*/ true);
    }

    bool RayTracer::BuildMeshBlas(uint32_t meshIndex, const GpuMesh &mesh, uint32_t vertexCount)
    {
        if (mBlases.size() <= meshIndex)
            mBlases.resize(meshIndex + 1);
        MeshBlas &blas = mBlases[meshIndex];

        VkBuffer scratch = VK_NULL_HANDLE;
        VmaAllocation scratchAllocation = VK_NULL_HANDLE;
        bool built = false;
        const bool submitted = SubmitAndWait(*mDevice, [&](VkCommandBuffer cmd)
            {
                built = BuildBLAS(mesh, vertexCount, cmd, blas, scratch, scratchAllocation);
            });

        if (scratch != VK_NULL_HANDLE)
            vmaDestroyBuffer(mDevice->Allocator(), scratch, scratchAllocation);

        if (!built || !submitted)
        {
            DestroyBLAS(blas);
            return false;
        }
        return true;
    }

    RayTracer::BlasBatch RayTracer::RecordMeshBlases(VkCommandBuffer cmd, std::span<const MeshBlasBuild> builds)
    {
        BlasBatch batch;
        std::vector<VkAccelerationStructureGeometryKHR> geometries(builds.size());
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        std::vector<VkDeviceSize> offsets;
        std::vector<uint32_t> slots;
        for (const MeshBlasBuild &build : builds)
        {
            const GpuMesh &mesh = *build.mesh;
            VkAccelerationStructureGeometryKHR &geometry = geometries[infos.size()];
            geometry = VkAccelerationStructureGeometryKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                .geometry{ .triangles =
                              VkAccelerationStructureGeometryTrianglesDataKHR{
                                  .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                  .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                                  .vertexData{ .deviceAddress = GetBufferAddress(mDevice->Handle(), mesh.mVertexBuffer) },
                                  .vertexStride = sizeof(Vertex),
                                  .maxVertex = build.vertexCount - 1,
                                  .indexType = VK_INDEX_TYPE_UINT32,
                                  .indexData{ .deviceAddress = GetBufferAddress(mDevice->Handle(), mesh.mIndexBuffer) } } },
                .flags = VK_GEOMETRY_OPAQUE_BIT_KHR };
            VkAccelerationStructureBuildGeometryInfoKHR info{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
                .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                .geometryCount = 1,
                .pGeometries = &geometry };
            const uint32_t primitiveCount = mesh.mIndexCount / 3;
            const AccelBuildSizes sizes = QueryAccelBuildSizes(mDevice->Handle(), info, primitiveCount);

            if (mBlases.size() <= build.meshIndex)
                mBlases.resize(build.meshIndex + 1);
            MeshBlas &blas = mBlases[build.meshIndex];
            if (!CreateAccelerationStructureObject(sizes.accelerationStructureSize,
                VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, blas.mBuffer,
                blas.mAllocation, blas.mBlas))
            {
                MIR_LOG_ERROR("RecordMeshBlases: acceleration structure creation failed for mesh {}", build.meshIndex);
                continue;
            }
            info.dstAccelerationStructure = blas.mBlas;
            const VkDeviceSize offset =
                (batch.scratchBytes + mScratchAlignment - 1) / mScratchAlignment * mScratchAlignment;
            batch.scratchBytes = offset + sizes.buildScratchSize;
            batch.blasBytes += sizes.accelerationStructureSize;
            infos.push_back(info);
            ranges.push_back({ .primitiveCount = primitiveCount });
            offsets.push_back(offset);
            slots.push_back(build.meshIndex);
        }
        if (infos.empty())
            return batch;

        if (!CreateScratchBuffer(batch.scratchBytes, batch.scratchBuffer, batch.scratchAllocation))
        {
            MIR_LOG_ERROR("RecordMeshBlases: scratch buffer allocation failed for {} meshes", slots.size());
            for (const uint32_t slot : slots)
                DestroyBLAS(mBlases[slot]);
            return BlasBatch{};
        }
        const VkDeviceAddress scratchBase = GetBufferAddress(mDevice->Handle(), batch.scratchBuffer);
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR *> rangePointers;
        for (size_t i = 0; i < infos.size(); ++i)
        {
            infos[i].scratchData.deviceAddress = scratchBase + offsets[i];
            rangePointers.push_back(&ranges[i]);
        }

        const VkMemoryBarrier2 copied{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                      .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                      .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT |
                                                       VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR };
        const VkDependencyInfo copiedDep{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &copied };
        vkCmdPipelineBarrier2(cmd, &copiedDep);
        // Disjoint scratch ranges, so the builds may run side by side.
        vkCmdBuildAccelerationStructuresKHR(cmd, uint32_t(infos.size()), infos.data(), rangePointers.data());

        for (const uint32_t slot : slots)
        {
            const VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                .accelerationStructure = mBlases[slot].mBlas };
            mBlases[slot].mAddress = vkGetAccelerationStructureDeviceAddressKHR(mDevice->Handle(), &addressInfo);
        }
        batch.built = slots.size();
        return batch;
    }

    bool RayTracer::HasMeshBlas(uint32_t meshIndex) const
    {
        return meshIndex < mBlases.size() && mBlases[meshIndex].mBlas != VK_NULL_HANDLE;
    }

    RayTracer::BlasSizes RayTracer::MeshBlasSizes(uint32_t vertexCount, uint32_t indexCount) const
    {
        const VkAccelerationStructureGeometryTrianglesDataKHR triangles{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
            .vertexStride = sizeof(Vertex),
            .maxVertex = vertexCount > 0 ? vertexCount - 1 : 0,
            .indexType = VK_INDEX_TYPE_UINT32 };
        const VkAccelerationStructureGeometryKHR geometry{ .sType =
                                                              VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                                                          .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                                                          .geometry{ .triangles = triangles },
                                                          .flags = VK_GEOMETRY_OPAQUE_BIT_KHR };
        const VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = 1,
            .pGeometries = &geometry };
        const AccelBuildSizes sizes = QueryAccelBuildSizes(mDevice->Handle(), buildInfo, indexCount / 3);
        return { sizes.accelerationStructureSize, sizes.buildScratchSize };
    }

    RayTracer::MeshBlas RayTracer::TakeMeshBlas(uint32_t meshIndex)
    {
        if (meshIndex >= mBlases.size())
            return MeshBlas{};
        const MeshBlas taken = mBlases[meshIndex];
        mBlases[meshIndex] = MeshBlas{};
        return taken;
    }

    namespace
    {
        // Every RT target is written as a storage image, sampled downstream and cleared by transfer.
        constexpr VkImageUsageFlags kRtImageUsage =
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        constexpr VkClearColorValue kBlackClear{ .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
        // Past any real hit, so a miss pixel reads as infinitely far to DepthOfField.
        constexpr VkClearColorValue kFarClear{ .float32 = { 1.0e8f, 0.0f, 0.0f, 0.0f } };

        constexpr VkShaderStageFlags kRaygen = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        constexpr VkShaderStageFlags kClosestHit = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

        struct RtImageDesc
        {
            const char *name;
            VkFormat format;
            // Set 0 binding; see raytrace.slang. 0 and 2 are the TLAS and instance info.
            uint32_t binding;
            VkShaderStageFlags stages;
            VkClearColorValue clear;
            // Allocated only while Dlss is active, so its binding is partially bound.
            bool guide;
        };

        constexpr uint32_t kRtImageCount = static_cast<uint32_t>(RtImage::Count);
        static_assert(kRtImageCount == kRtSetStorageImages, "RT image table and descriptor budget differ");

        // Indexed by RtImage.
        constexpr std::array<RtImageDesc, kRtImageCount> kRtImages{ {
            { "output", kHdrColorFormat, 1, kRaygen, kBlackClear, false },
            { "distance", kDistanceFormat, 3, kRaygen, kFarClear, false },
            { "velocity", kRtVelocityFormat, 4, kRaygen, kBlackClear, false },
            // Guides are written by closestHitMain only - see gAlbedo/gNormal in raytrace.slang.
            // Background (miss) pixels never run it, so without the clear a sky pixel's guide
            // data would be whatever the image last held. Neutral zero: zero albedo, zero normal/
            // roughness - a background pixel's guide data just doesn't mean much to Ray Reconstruction.
            { "albedo", kAlbedoFormat, 5, kClosestHit, kBlackClear, true },
            { "normal", kHdrColorFormat, 6, kClosestHit, kBlackClear, true },
            { "specularAlbedo", kHdrColorFormat, 7, kClosestHit, kBlackClear, true },
            { "dlssDepth", kDistanceFormat, 8, kRaygen, kBlackClear, true } } };

        // The always-present outputs, which precede the guides.
        constexpr uint32_t kOutputImageCount = static_cast<uint32_t>(RtImage::Albedo);

        constexpr bool OutputsPrecedeGuides()
        {
            for (uint32_t i = 0; i < kRtImageCount; ++i)
                if (kRtImages[i].guide != (i >= kOutputImageCount))
                    return false;
            return true;
        }
        static_assert(OutputsPrecedeGuides(), "kRtImages: outputs first, then guides, matching RtImage");

        uint32_t FirstImage(bool guides) { return guides ? kOutputImageCount : 0; }
        uint32_t EndImage(bool guides) { return guides ? kRtImageCount : kOutputImageCount; }
    }

    bool RayTracer::CreateImages(bool guides, VkExtent2D extent)
    {
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
            for (uint32_t i = FirstImage(guides); i < EndImage(guides); ++i)
                if (!CreateImage2D(*mDevice, kRtImages[i].format, extent, kRtImageUsage,
                    std::format("RT {} image {}", kRtImages[i].name, frame).c_str(), mImages[i][frame]))
                    return false;

        return true;
    }

    void RayTracer::DestroyImages(bool guides)
    {
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
            for (uint32_t i = FirstImage(guides); i < EndImage(guides); ++i)
                DestroyImage2D(*mDevice, mImages[i][frame]);
    }

    bool RayTracer::CreateOutputImages(VkExtent2D extent)
    {
        if (!CreateImages(/*guides*/ false, extent))
            return false;

        mExtent = extent;
        return true;
    }

    bool RayTracer::CreatePipeline(VkDescriptorSetLayout cameraSetLayout)
    {
        // Indexed by stage: raygen, miss, shadow miss, legacy closest hit, legacy any hit,
        // shadow any hit, then one closest hit per registered surface material.
        constexpr std::array<const char *, 6> kLegacyEntryPoints{
            "rayGenMain", "missMain", "shadowMissMain", "closestHitMain", "anyHitMain", "shadowAnyHitMain" };
        constexpr std::array<VkShaderStageFlagBits, 6> kLegacyStages{
            VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, VK_SHADER_STAGE_ANY_HIT_BIT_KHR, VK_SHADER_STAGE_ANY_HIT_BIT_KHR };

        std::vector<std::string> entryPoints(kLegacyEntryPoints.begin(), kLegacyEntryPoints.end());
        std::vector<VkShaderStageFlagBits> stageFlags(kLegacyStages.begin(), kLegacyStages.end());
        std::vector<std::string> shaderFiles;
        for (const char *entry : kLegacyEntryPoints)
            shaderFiles.push_back(std::format("raytrace.{}.spv", entry));
        for (const SurfaceMaterialRegistration &material : mSurfaceMaterials)
        {
            entryPoints.push_back("closestHitSurface");
            stageFlags.push_back(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
            shaderFiles.push_back(std::format("{}.closestHitSurface.spv", material.stem));
        }

        std::vector<VkShaderModule> modules(shaderFiles.size(), VK_NULL_HANDLE);
        const auto destroyModules = [&]
            {
                for (VkShaderModule module : modules)
                    if (module != VK_NULL_HANDLE)
                        vkDestroyShaderModule(mDevice->Handle(), module, nullptr);
            };

        for (size_t i = 0; i < modules.size(); ++i)
        {
            modules[i] = CreateShaderModule(mDevice->Handle(), ShaderPath(shaderFiles[i]));
            if (modules[i] == VK_NULL_HANDLE)
            {
                MIR_LOG_CRITICAL("CreatePipeline: missing RT material shader '{}'", shaderFiles[i]);
                destroyModules();
                return false;
            }
        }

        constexpr VkShaderStageFlags kHitStages = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

        // The TLAS, the instance info, every image, then the shared image,
        // sampler and material-record rows.
        std::vector<VkDescriptorSetLayoutBinding> setBindings;
        std::vector<VkDescriptorBindingFlags> bindingFlags;

        const auto addBinding = [&](VkDescriptorSetLayoutBinding binding, VkDescriptorBindingFlags flags)
        {
            setBindings.push_back(binding);
            bindingFlags.push_back(flags);
        };

        addBinding({ .binding = 0,
                        .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                        .descriptorCount = 1,
                        .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR },
            0);
        addBinding({ .binding = 2,
                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                        .descriptorCount = 1,
                        .stageFlags = kHitStages },
            0);

        for (uint32_t i = 0; i < kRtImageCount; ++i)
        {
            addBinding({ .binding = kRtImages[i].binding,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                            .descriptorCount = 1,
                            .stageFlags = kRtImages[i].stages },
                kRtImages[i].guide ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT : 0);
        }

        addBinding({ .binding = kRtMaterialImageBinding,
                        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                        .descriptorCount = kMaxSharedImages,
                        .stageFlags = kHitStages },
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
        addBinding({ .binding = kRtMaterialSamplerBinding,
                        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                        .descriptorCount = kMaxSharedSamplers,
                        .stageFlags = kHitStages },
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
        addBinding({ .binding = kRtMaterialRecordBinding,
                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                        .descriptorCount = 1,
                        .stageFlags = kHitStages },
            0);

        const VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = static_cast<uint32_t>(bindingFlags.size()),
            .pBindingFlags = bindingFlags.data() };

        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &bindingFlagsInfo,
            .bindingCount = static_cast<uint32_t>(setBindings.size()),
            .pBindings = setBindings.data() };

        if (vkCreateDescriptorSetLayout(mDevice->Handle(), &setLayoutInfo, nullptr, &mSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipeline: vkCreateDescriptorSetLayout failed");
            destroyModules();
            return false;
        }

        const std::array<VkDescriptorPoolSize, 5> poolSizes{ {
            { .type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
              .descriptorCount = kFramesInFlight * kRtSetAccelerationStructures },
            { .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
              .descriptorCount = kFramesInFlight * kRtSetStorageImages },
            { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              .descriptorCount = kFramesInFlight * kRtSetStorageBuffers },
            { .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
              .descriptorCount = kFramesInFlight * kMaxSharedImages },
            { .type = VK_DESCRIPTOR_TYPE_SAMPLER,
              .descriptorCount = kFramesInFlight * kMaxSharedSamplers } } };

        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kFramesInFlight,
            .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
            .pPoolSizes = poolSizes.data() };

        if (vkCreateDescriptorPool(mDevice->Handle(), &poolInfo, nullptr, &mPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipeline: vkCreateDescriptorPool failed");
            destroyModules();
            return false;
        }

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkDescriptorSetAllocateInfo setAllocInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = mPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &mSetLayout };

            if (vkAllocateDescriptorSets(mDevice->Handle(), &setAllocInfo, &mSets[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreatePipeline: descriptor set allocation failed for frame {}", i);
                destroyModules();
                return false;
            }
        }

        static_assert(kRtPipelineSetCount == 2);
        const std::array<VkDescriptorSetLayout, kRtPipelineSetCount> setLayouts{ mSetLayout, cameraSetLayout };

        const VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            .offset = 0,
            .size = sizeof(RtPushConstants) };

        const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
            .pSetLayouts = setLayouts.data(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRange };

        if (vkCreatePipelineLayout(mDevice->Handle(), &pipelineLayoutInfo, nullptr, &mPipelineLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipeline: vkCreatePipelineLayout failed");
            destroyModules();
            return false;
        }

        std::vector<VkPipelineShaderStageCreateInfo> stages(entryPoints.size());
        for (size_t i = 0; i < stages.size(); ++i)
            stages[i] = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                         .stage = stageFlags[i],
                         .module = modules[i],
                         .pName = entryPoints[i].c_str() };

        const auto generalGroup = [](uint32_t shader)
            {
                return VkRayTracingShaderGroupCreateInfoKHR{
                    .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                    .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
                    .generalShader = shader,
                    .closestHitShader = VK_SHADER_UNUSED_KHR,
                    .anyHitShader = VK_SHADER_UNUSED_KHR,
                    .intersectionShader = VK_SHADER_UNUSED_KHR };
            };
        const auto hitGroup = [](uint32_t closestHit, uint32_t anyHit)
            {
                return VkRayTracingShaderGroupCreateInfoKHR{
                    .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                    .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
                    .generalShader = VK_SHADER_UNUSED_KHR,
                    .closestHitShader = closestHit,
                    .anyHitShader = anyHit,
                    .intersectionShader = VK_SHADER_UNUSED_KHR };
            };

        // Raygen, the two misses, legacy camera/shadow groups 0/1, then a
        // primary+shadow record pair per surface material. An instance's even
        // base offset makes TraceRay's 0/1 record offset address both records.
        std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups{
            generalGroup(0), generalGroup(1), generalGroup(2), hitGroup(3, 4), hitGroup(VK_SHADER_UNUSED_KHR, 5) };
        for (size_t i = 0; i < mSurfaceMaterials.size(); ++i)
        {
            groups.push_back(hitGroup(static_cast<uint32_t>(6 + i), 4));
            groups.push_back(hitGroup(VK_SHADER_UNUSED_KHR, 5));
        }

        const VkRayTracingPipelineCreateInfoKHR pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
            .stageCount = static_cast<uint32_t>(stages.size()),
            .pStages = stages.data(),
            .groupCount = static_cast<uint32_t>(groups.size()),
            .pGroups = groups.data(),
            .maxPipelineRayRecursionDepth = 2,
            .layout = mPipelineLayout };

        const VkResult pipelineResult = vkCreateRayTracingPipelinesKHR(
            mDevice->Handle(), VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &mPipeline);

        destroyModules();

        if (pipelineResult != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipeline: vkCreateRayTracingPipelinesKHR failed: {}", static_cast<int>(pipelineResult));
            return false;
        }

        const uint32_t kGroupCount = static_cast<uint32_t>(groups.size());
        const uint32_t handleSize = mDevice->RtProperties().shaderGroupHandleSize;
        const uint32_t handleSizeAligned = AlignUp(handleSize, mDevice->RtProperties().shaderGroupHandleAlignment);
        const VkDeviceSize regionSize = AlignUp(handleSizeAligned, mDevice->RtProperties().shaderGroupBaseAlignment);
        const VkDeviceSize sbtSize = regionSize * kGroupCount;

        std::vector<std::byte> handles(static_cast<size_t>(handleSize) * kGroupCount);
        if (vkGetRayTracingShaderGroupHandlesKHR(mDevice->Handle(), mPipeline, 0, kGroupCount,
            handles.size(), handles.data()) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipeline: vkGetRayTracingShaderGroupHandlesKHR failed");
            return false;
        }

        const VkBufferCreateInfo sbtBufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sbtSize,
            .usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE };

        const VmaAllocationCreateInfo sbtAllocInfo{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO };

        VmaAllocationInfo sbtInfo{};
        if (vmaCreateBufferWithAlignment(mDevice->Allocator(), &sbtBufferInfo, &sbtAllocInfo,
            mDevice->RtProperties().shaderGroupBaseAlignment, &mSbtBuffer, &mSbtAllocation, &sbtInfo) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipeline: SBT buffer allocation failed");
            return false;
        }

        std::byte *const sbtMapped = static_cast<std::byte *>(sbtInfo.pMappedData);
        for (uint32_t i = 0; i < kGroupCount; ++i)
            std::memcpy(sbtMapped + i * regionSize, handles.data() + i * handleSize, handleSize);

        const VkDeviceAddress sbtBase = GetBufferAddress(mDevice->Handle(), mSbtBuffer);
        mSbtRaygenRegion = { .deviceAddress = sbtBase + 0 * regionSize, .stride = regionSize, .size = regionSize };
        mSbtMissRegion = { .deviceAddress = sbtBase + 1 * regionSize, .stride = regionSize, .size = 2 * regionSize };
        mSbtHitRegion = { .deviceAddress = sbtBase + 3 * regionSize, .stride = regionSize,
            .size = static_cast<VkDeviceSize>(kGroupCount - 3) * regionSize };
        mSbtCallableRegion = {};

        return true;
    }

    void RayTracer::WriteDescriptorSets()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkWriteDescriptorSetAccelerationStructureKHR accelWrite{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                .accelerationStructureCount = 1,
                .pAccelerationStructures = &mTlas[i] };

            const VkDescriptorBufferInfo instanceInfoBufferInfo{
                .buffer = mInstanceInfoBuffers[i],
                .offset = 0,
                .range = VK_WHOLE_SIZE };

            std::array<VkDescriptorImageInfo, kRtImageCount> imageInfos{};
            std::array<VkWriteDescriptorSet, 2 + kRtImageCount> writes{ {
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .pNext = &accelWrite,
                 .dstSet = mSets[i],
                 .dstBinding = 0,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = mSets[i],
                 .dstBinding = 2,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 .pBufferInfo = &instanceInfoBufferInfo } } };

            // Without guides their bindings are left as they were.
            const uint32_t imageCount = EndImage(HasDlssGuides());
            for (uint32_t image = 0; image < imageCount; ++image)
            {
                imageInfos[image] = { .imageView = mImages[image][i].view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
                writes[2 + image] = {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = mSets[i],
                    .dstBinding = kRtImages[image].binding,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .pImageInfo = &imageInfos[image] };
            }

            vkUpdateDescriptorSets(mDevice->Handle(), 2 + imageCount, writes.data(), 0, nullptr);
        }
    }

    void RayTracer::WriteMaterialTableDescriptors(uint32_t frame)
    {
        if (mTable == nullptr)
            return;

        const VkDescriptorBufferInfo &recordBuffer = mTable->RecordBufferInfo();
        const VkWriteDescriptorSet writes[]{
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = mSets[frame],
             .dstBinding = kRtMaterialImageBinding,
             .descriptorCount = kMaxSharedImages,
             .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
             .pImageInfo = mTable->ImageTable().data() },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = mSets[frame],
             .dstBinding = kRtMaterialSamplerBinding,
             .descriptorCount = kMaxSharedSamplers,
             .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
             .pImageInfo = mTable->SamplerTable().data() },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = mSets[frame],
             .dstBinding = kRtMaterialRecordBinding,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .pBufferInfo = &recordBuffer } };
        vkUpdateDescriptorSets(mDevice->Handle(), static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
        mTableVersion[frame] = mTable->TableVersion();
    }

    bool RayTracer::CreateScene()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            VmaAllocationInfo instanceInfo{};
            if (!CreateBuffer(mDevice->Allocator(), sizeof(VkAccelerationStructureInstanceKHR) * kMaxInstances,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                mInstanceBuffers[i], mInstanceAllocations[i], &instanceInfo))
            {
                MIR_LOG_CRITICAL("CreateScene: instance buffer allocation failed for frame {}", i);
                return false;
            }
            mInstanceMapped[i] = static_cast<std::byte *>(instanceInfo.pMappedData);

            VmaAllocationInfo instanceRtInfo{};
            if (!CreateBuffer(mDevice->Allocator(), sizeof(RtInstanceInfo) * kMaxInstances,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                mInstanceInfoBuffers[i], mInstanceInfoAllocations[i], &instanceRtInfo))
            {
                MIR_LOG_CRITICAL("CreateScene: instance info buffer allocation failed for frame {}", i);
                return false;
            }
            mInstanceInfoMapped[i] = static_cast<std::byte *>(instanceRtInfo.pMappedData);

            const VkAccelerationStructureGeometryInstancesDataKHR instancesData{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                .arrayOfPointers = VK_FALSE };

            const VkAccelerationStructureGeometryKHR geometry{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
                .geometry{ .instances = instancesData } };

            const VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
                .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                .geometryCount = 1,
                .pGeometries = &geometry };

            const AccelBuildSizes sizes = QueryAccelBuildSizes(mDevice->Handle(), buildInfo, kMaxInstances);

            if (!CreateAccelerationStructureObject(sizes.accelerationStructureSize,
                VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                mTlasBuffers[i], mTlasAllocations[i], mTlas[i]))
            {
                MIR_LOG_CRITICAL("CreateScene: TLAS creation failed for frame {}", i);
                return false;
            }

            if (!CreateScratchBuffer(sizes.buildScratchSize, mTlasScratchBuffers[i], mTlasScratchAllocations[i]))
            {
                MIR_LOG_CRITICAL("CreateScene: TLAS scratch buffer allocation failed for frame {}", i);
                return false;
            }
            mTlasScratchAddresses[i] = GetBufferAddress(mDevice->Handle(), mTlasScratchBuffers[i]);

            mDevice->NameObject(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, reinterpret_cast<uint64_t>(mTlas[i]),
                std::format("TLAS {}", i).c_str());
        }

        return true;
    }

    void RayTracer::DestroyScene()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (mTlas[i] != VK_NULL_HANDLE)
            {
                vkDestroyAccelerationStructureKHR(mDevice->Handle(), mTlas[i], nullptr);
                mTlas[i] = VK_NULL_HANDLE;
            }
            if (mTlasBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mDevice->Allocator(), mTlasBuffers[i], mTlasAllocations[i]);
                mTlasBuffers[i] = VK_NULL_HANDLE;
                mTlasAllocations[i] = VK_NULL_HANDLE;
            }
            if (mTlasScratchBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mDevice->Allocator(), mTlasScratchBuffers[i], mTlasScratchAllocations[i]);
                mTlasScratchBuffers[i] = VK_NULL_HANDLE;
                mTlasScratchAllocations[i] = VK_NULL_HANDLE;
            }
            if (mInstanceBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mDevice->Allocator(), mInstanceBuffers[i], mInstanceAllocations[i]);
                mInstanceBuffers[i] = VK_NULL_HANDLE;
                mInstanceAllocations[i] = VK_NULL_HANDLE;
                mInstanceMapped[i] = nullptr;
            }
            if (mInstanceInfoBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mDevice->Allocator(), mInstanceInfoBuffers[i], mInstanceInfoAllocations[i]);
                mInstanceInfoBuffers[i] = VK_NULL_HANDLE;
                mInstanceInfoAllocations[i] = VK_NULL_HANDLE;
                mInstanceInfoMapped[i] = nullptr;
            }
            mTlasScratchAddresses[i] = 0;
        }
    }

    RayTracer::PreparedScene RayTracer::RecordTlasBuild(VkCommandBuffer cmd, uint32_t frameIndex, std::span<const DrawItem> items,
        const ResourceStore &resources, const MaterialStore &materials,
        std::optional<std::span<const DrawItem>> fallbackItems)
    {
        auto *const instances = reinterpret_cast<VkAccelerationStructureInstanceKHR *>(mInstanceMapped[frameIndex]);
        auto *const instanceInfos = reinterpret_cast<RtInstanceInfo *>(mInstanceInfoMapped[frameIndex]);

        uint32_t instanceCount = 0;
        bool truncated = false;
        bool giCompatible = true;
        for (const DrawItem &item : items)
        {
            const GpuMesh *const mesh = resources.FindMesh(item.mesh);
            if (mesh == nullptr || item.mesh.mIndex >= mBlases.size() || mBlases[item.mesh.mIndex].mBlas == VK_NULL_HANDLE)
                continue;

            const GpuMaterial *const material = materials.FindOrDefault(item.material);
            const auto facing = RtTriangleFacingFlags(
                material ? material->mCullMode : VK_CULL_MODE_BACK_BIT,
                material ? material->mFrontFace : VK_FRONT_FACE_COUNTER_CLOCKWISE, item.model);
            if (!facing)
                continue;

            if (instanceCount >= kMaxInstances)
            {
                truncated = true;
                if (!mWarnedInstanceCap)
                {
                    MIR_LOG_WARN("RecordTlasBuild: more than {} draws; the rest are not traced", kMaxInstances);
                    mWarnedInstanceCap = true;
                }
                break;
            }

            // Only the model shader samples its texture and reads its alpha and side parameters.
            uint32_t flags = 0;
            uint32_t textureSlot = 0;
            uint32_t samplerSlot = 0;
            uint32_t materialRecord = kInvalidMaterialRecordOffset;
            if (material != nullptr)
            {
                if (material->mUnlit)
                    flags |= kInstanceUnlit;
                if (material->mError)
                    flags |= kInstanceError;
                if (material->mModelShader)
                {
                    textureSlot = material->mBaseColorSlot;
                    samplerSlot = material->mSamplerSlot;
                    flags |= kInstanceTextured;

                    if (static_cast<MaterialAlphaMode>(static_cast<uint32_t>(material->mParameters.x)) !=
                        MaterialAlphaMode::Opaque)
                        flags |= kInstanceAlphaTest;
                    if (material->mParameters.z > 0.0f)
                        flags |= kInstanceDoubleSided;
                }
                materialRecord = material->mRecordOffset;
                // The DDGI probe and camera pipelines keep one legacy hit pair;
                // any per-material hit group would index past their SBT.
                if (material->mRayTracingSbtOffset != 0)
                    giCompatible = false;
            }

            if (RtRasterFacingCorrection(material ? material->mFrontFace : VK_FRONT_FACE_COUNTER_CLOCKWISE,
                    item.model, *facing))
                flags |= kInstanceRasterFacingCorrection;

            // Non-opaque instances run the any-hit shaders, which do the alpha test.
            const VkGeometryInstanceFlagsKHR opacity =
                (flags & kInstanceAlphaTest) != 0 ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : 0;

            instances[instanceCount] = VkAccelerationStructureInstanceKHR{
                .transform = ToTransformMatrixKHR(item.model),
                .instanceCustomIndex = instanceCount,
                .mask = 0xFF,
                .instanceShaderBindingTableRecordOffset = material != nullptr ? material->mRayTracingSbtOffset : 0,
                .flags = *facing | opacity,
                .accelerationStructureReference = mBlases[item.mesh.mIndex].mAddress };

            instanceInfos[instanceCount] = RtInstanceInfo{
                .vertexAddress = GetBufferAddress(mDevice->Handle(), mesh->mVertexBuffer),
                .indexAddress = GetBufferAddress(mDevice->Handle(), mesh->mIndexBuffer),
                .tint = item.tint,
                .normalCol0 = glm::vec4(item.normalMatrix[0], 0.0f),
                .normalCol1 = glm::vec4(item.normalMatrix[1], 0.0f),
                .normalCol2 = glm::vec4(item.normalMatrix[2], 0.0f),
                .flags = flags,
                .baseColorTexture = textureSlot,
                .baseColorSampler = samplerSlot,
                .materialRecord = materialRecord,
                .alphaCutoff = material != nullptr ? material->mParameters.y : 0.0f,
                .prevModel = item.previousModel,
                .modelCol0 = item.model[0],
                .modelCol1 = item.model[1],
                .modelCol2 = item.model[2] };

            ++instanceCount;
        }

        if (truncated && fallbackItems)
        {
            // Refill before recording a build or updating descriptors. Only the
            // camera scene reaches the GPU; GI must still reject this frame.
            PreparedScene scene = RecordTlasBuild(cmd, frameIndex, *fallbackItems, resources, materials, std::nullopt);
            scene.truncated = true;
            return scene;
        }

        const VkDeviceSize instanceBytes = sizeof(VkAccelerationStructureInstanceKHR) * instanceCount;
        if (instanceBytes > 0)
            vmaFlushAllocation(mDevice->Allocator(), mInstanceAllocations[frameIndex], 0, instanceBytes);

        const VkDeviceSize instanceInfoBytes = sizeof(RtInstanceInfo) * instanceCount;
        if (instanceInfoBytes > 0)
            vmaFlushAllocation(mDevice->Allocator(), mInstanceInfoAllocations[frameIndex], 0, instanceInfoBytes);

        const VkAccelerationStructureGeometryInstancesDataKHR instancesData{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
            .arrayOfPointers = VK_FALSE,
            .data{ .deviceAddress = GetBufferAddress(mDevice->Handle(), mInstanceBuffers[frameIndex]) } };

        const VkAccelerationStructureGeometryKHR geometry{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
            .geometry{ .instances = instancesData } };

        const VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .dstAccelerationStructure = mTlas[frameIndex],
            .geometryCount = 1,
            .pGeometries = &geometry,
            .scratchData{ .deviceAddress = mTlasScratchAddresses[frameIndex] } };

        const VkAccelerationStructureBuildRangeInfoKHR range{ .primitiveCount = instanceCount };
        const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;

        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRange);
        return { mTlas[frameIndex], mSets[frameIndex], mSetLayout, instanceCount, truncated, giCompatible };
    }

    bool RayTracer::CreateScratchBuffer(VkDeviceSize size, VkBuffer &outBuffer, VmaAllocation &outAllocation)
    {
        const VkBufferCreateInfo bufferInfo{ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                            .size = size,
                                            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };
        return vmaCreateBufferWithAlignment(mDevice->Allocator(), &bufferInfo, &allocInfo, mScratchAlignment,
            &outBuffer, &outAllocation, nullptr) == VK_SUCCESS;
    }

    bool RayTracer::CreateAccelerationStructureObject(VkDeviceSize size, VkAccelerationStructureTypeKHR type,
        VkBuffer &outBuffer, VmaAllocation &outAllocation, VkAccelerationStructureKHR &outAS)
    {
        if (!CreateBuffer(mDevice->Allocator(), size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            0, outBuffer, outAllocation, nullptr))
            return false;

        const VkAccelerationStructureCreateInfoKHR createInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = outBuffer,
            .size = size,
            .type = type };

        if (vkCreateAccelerationStructureKHR(mDevice->Handle(), &createInfo, nullptr, &outAS) != VK_SUCCESS)
        {
            vmaDestroyBuffer(mDevice->Allocator(), outBuffer, outAllocation);
            outBuffer = VK_NULL_HANDLE;
            outAllocation = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    void RayTracer::DestroyBLAS(MeshBlas &blas)
    {
        if (blas.mBlas != VK_NULL_HANDLE)
        {
            vkDestroyAccelerationStructureKHR(mDevice->Handle(), blas.mBlas, nullptr);
            blas.mBlas = VK_NULL_HANDLE;
        }
        if (blas.mBuffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(mDevice->Allocator(), blas.mBuffer, blas.mAllocation);
            blas.mBuffer = VK_NULL_HANDLE;
            blas.mAllocation = VK_NULL_HANDLE;
        }
        blas.mAddress = 0;
    }

    void RayTracer::DestroyMeshBlas(uint32_t meshIndex)
    {
        if (meshIndex < mBlases.size())
            DestroyBLAS(mBlases[meshIndex]);
    }

    void RayTracer::DestroyBlases()
    {
        for (MeshBlas &blas : mBlases)
            DestroyBLAS(blas);
        mBlases.clear();
    }

    bool RayTracer::BuildBLAS(const GpuMesh &mesh, uint32_t vertexCount, VkCommandBuffer cmd, MeshBlas &outBlas,
        VkBuffer &outScratchBuffer, VmaAllocation &outScratchAllocation)
    {
        const VkDeviceAddress vertexAddress = GetBufferAddress(mDevice->Handle(), mesh.mVertexBuffer);
        const VkDeviceAddress indexAddress = GetBufferAddress(mDevice->Handle(), mesh.mIndexBuffer);

        const VkAccelerationStructureGeometryTrianglesDataKHR triangles{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
            .vertexData{ .deviceAddress = vertexAddress },
            .vertexStride = sizeof(Vertex),
            .maxVertex = vertexCount - 1,
            .indexType = VK_INDEX_TYPE_UINT32,
            .indexData{ .deviceAddress = indexAddress } };

        const VkAccelerationStructureGeometryKHR geometry{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .geometry{ .triangles = triangles },
            .flags = VK_GEOMETRY_OPAQUE_BIT_KHR };

        const uint32_t primitiveCount = mesh.mIndexCount / 3;

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = 1,
            .pGeometries = &geometry };

        const AccelBuildSizes sizes = QueryAccelBuildSizes(mDevice->Handle(), buildInfo, primitiveCount);

        if (!CreateAccelerationStructureObject(sizes.accelerationStructureSize,
            VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            outBlas.mBuffer, outBlas.mAllocation, outBlas.mBlas))
        {
            MIR_LOG_CRITICAL("BuildBLAS: acceleration structure creation failed");
            return false;
        }

        if (!CreateScratchBuffer(sizes.buildScratchSize, outScratchBuffer, outScratchAllocation))
        {
            MIR_LOG_CRITICAL("BuildBLAS: scratch buffer allocation failed");
            DestroyBLAS(outBlas);
            return false;
        }

        buildInfo.dstAccelerationStructure = outBlas.mBlas;
        buildInfo.scratchData.deviceAddress = GetBufferAddress(mDevice->Handle(), outScratchBuffer);

        const VkAccelerationStructureBuildRangeInfoKHR range{ .primitiveCount = primitiveCount };
        const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;

        const VkMemoryBarrier2 preBuild{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT };
        const VkDependencyInfo preBuildDep{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &preBuild };
        vkCmdPipelineBarrier2(cmd, &preBuildDep);

        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRange);

        const VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = outBlas.mBlas };
        outBlas.mAddress = vkGetAccelerationStructureDeviceAddressKHR(mDevice->Handle(), &addressInfo);

        return true;
    }

    RayTracer::PreparedScene RayTracer::PrepareScene(VkCommandBuffer cmd, uint32_t frame,
        std::span<const DrawItem> items, const ResourceStore &resources, const MaterialStore &materials,
        std::optional<std::span<const DrawItem>> fallbackItems)
    {
        const PreparedScene scene = RecordTlasBuild(cmd, frame, items, resources, materials, fallbackItems);

        if (mTable != nullptr && mTableVersion[frame] != mTable->TableVersion())
            WriteMaterialTableDescriptors(frame);

        const VkMemoryBarrier2 tlasBarrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR };
        const VkDependencyInfo tlasDep{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &tlasBarrier };
        vkCmdPipelineBarrier2(cmd, &tlasDep);

        return scene;
    }

    void RayTracer::RecordTrace(VkCommandBuffer cmd, uint32_t frame, const VkRect2D &rect,
        VkDescriptorSet cameraSet, uint32_t cameraOffset, AntiAliasingMode aaMode, uint32_t rtSampleCount,
        const GiShading *gi, GpuTiming *timing)
    {
        // Without guide images their bindings are unwritten, so the shader's Dlss branches must not run.
        if (aaMode == AntiAliasingMode::Dlss && !HasDlssGuides())
            aaMode = AntiAliasingMode::Msaa;
        const bool dlssActive = aaMode == AntiAliasingMode::Dlss;

        // The outputs every frame, then the guide buffers while Dlss is active.
        const uint32_t writtenCount = EndImage(dlssActive);
        std::array<VkImage, kRtImageCount> images{};
        for (uint32_t i = 0; i < writtenCount; ++i)
            images[i] = mImages[i][frame].image;
        const std::span<const VkImage> written = std::span(images).first(writtenCount);
        const std::span<const VkImage> outputs = written.first(kOutputImageCount);
        const std::span<const VkImage> guides = written.subspan(kOutputImageCount);

        for (VkImage image : written)
            ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        const VkImageSubresourceRange outputRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        for (uint32_t i = 0; i < writtenCount; ++i)
            vkCmdClearColorImage(cmd, images[i], VK_IMAGE_LAYOUT_GENERAL, &kRtImages[i].clear, 1, &outputRange);

        for (VkImage image : written)
            ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        if (rect.extent.width > 0 && rect.extent.height > 0)
        {
            const VkPipelineLayout layout = gi ? gi->layout : mPipelineLayout;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, gi ? gi->pipeline : mPipeline);

            const std::array<VkDescriptorSet, 3> sets{ mSets[frame], cameraSet, gi ? gi->descriptors : VK_NULL_HANDLE };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout,
                0, gi ? 3u : 2u, sets.data(), 1, &cameraOffset);

            const RtPushConstants pushConstants{
                .outputOffset{ static_cast<uint32_t>(rect.offset.x), static_cast<uint32_t>(rect.offset.y) },
                .aaMode = static_cast<uint32_t>(aaMode),
                .sampleCount = rtSampleCount,
                .giIntensity = gi ? gi->intensity : 0.0f,
                .giWeight = gi ? gi->weight : 0.0f };
            vkCmdPushConstants(cmd, layout,
                VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
                0, sizeof(RtPushConstants), &pushConstants);

            if (timing != nullptr)
                timing->MarkBegin(cmd, frame, GpuTimingRegion::RtTraceRays);
            vkCmdTraceRaysKHR(cmd, gi ? &gi->raygen : &mSbtRaygenRegion, gi ? &gi->miss : &mSbtMissRegion,
                gi ? &gi->hit : &mSbtHitRegion, gi ? &gi->callable : &mSbtCallableRegion,
                rect.extent.width, rect.extent.height, 1);
            if (timing != nullptr)
                timing->MarkEnd(cmd, frame, GpuTimingRegion::RtTraceRays, rect.extent);
        }

        for (VkImage image : outputs)
            ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

        // Guides remain GENERAL here. The RR pass transitions them to sampled-image
        // layouts and synchronizes both shader writes and transfer-cleared background.
        for (VkImage image : guides)
            ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }
}

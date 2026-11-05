/**
 * @file RayTracerDeformation.cpp
 * @author Jackey Cen
 * @brief Per-frame model deformation and instance BLAS builds for ray tracing.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 */
#include "RayTracer.h"
#include "Device.h"
#include "ResourceStore.h"
#include "VulkanUtil.h"

#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <vk_mem_alloc.h>

#include <cmath>

namespace mir::vk
{
    namespace
    {
        struct DeformationPush
        {
            VkDeviceAddress source;
            VkDeviceAddress deformation;
            VkDeviceAddress output;
            uint32_t vertexCount;
            uint32_t padding = 0;
        };
        static_assert(sizeof(DeformationPush) == 32);

        VkDeviceAddress Address(VkDevice device, VkBuffer buffer)
        {
            const VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .buffer = buffer };
            return vkGetBufferDeviceAddress(device, &info);
        }
    }

    bool RayTracer::InitializeDeformation()
    {
        const VkPushConstantRange push{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(DeformationPush) };
        const VkPipelineLayoutCreateInfo layout{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push };
        if (vkCreatePipelineLayout(mDevice->Handle(), &layout, nullptr, &mDeformationLayout) != VK_SUCCESS)
            return false;
        return CreateDeformationPipeline(mDeformationPipeline);
    }

    bool RayTracer::CreateDeformationPipeline(VkPipeline &pipeline)
    {
        const VkShaderModule shader = CreateShaderModule(mDevice->Handle(), ShaderPath("rayDeform.deformMain.spv"));
        if (shader == VK_NULL_HANDLE)
            return false;
        const VkComputePipelineCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                       .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                       .module = shader,
                       .pName = "deformMain" },
            .layout = mDeformationLayout };
        const VkResult result = vkCreateComputePipelines(mDevice->Handle(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        vkDestroyShaderModule(mDevice->Handle(), shader, nullptr);
        return result == VK_SUCCESS;
    }

    void RayTracer::ResetDeformations(uint32_t frame)
    {
        for (auto &mesh : mDeformedMeshes[frame])
        {
            DestroyBLAS(mesh.blas);
            if (mesh.scratch != VK_NULL_HANDLE)
                vmaDestroyBuffer(mDevice->Allocator(), mesh.scratch, mesh.scratchAllocation);
            if (mesh.vertices != VK_NULL_HANDLE)
                vmaDestroyBuffer(mDevice->Allocator(), mesh.vertices, mesh.vertexAllocation);
            if (mesh.deformation != VK_NULL_HANDLE)
                vmaDestroyBuffer(mDevice->Allocator(), mesh.deformation, mesh.deformationAllocation);
            if (mesh.previousVertices != VK_NULL_HANDLE)
                vmaDestroyBuffer(mDevice->Allocator(), mesh.previousVertices, mesh.previousVertexAllocation);
            if (mesh.previousDeformation != VK_NULL_HANDLE)
                vmaDestroyBuffer(mDevice->Allocator(), mesh.previousDeformation, mesh.previousDeformationAllocation);
        }
        mDeformedMeshes[frame].clear();
    }

    void RayTracer::ShutdownDeformation()
    {
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
            ResetDeformations(frame);
        if (mDeformationPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(mDevice->Handle(), mDeformationPipeline, nullptr);
        if (mDeformationLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(mDevice->Handle(), mDeformationLayout, nullptr);
        mDeformationPipeline = VK_NULL_HANDLE;
        mDeformationLayout = VK_NULL_HANDLE;
    }

    const RayTracer::DeformedMesh *RayTracer::RecordDeformedMesh(VkCommandBuffer cmd, uint32_t frame,
        const GpuMesh &mesh, std::span<const glm::vec4> deformation,
        std::span<const glm::vec4> previousDeformation)
    {
        if (deformation.size() < 3 || !std::isfinite(deformation[0].x) ||
            deformation[0].x < 1 || deformation[0].x >= 16777216)
        {
            MIR_LOG_ERROR("RT deformation: invalid vertex count");
            return nullptr;
        }
        const uint32_t count = static_cast<uint32_t>(deformation[0].x);
        if (!previousDeformation.empty() &&
            (previousDeformation.size() < 3 || previousDeformation[0].x != deformation[0].x))
        {
            MIR_LOG_ERROR("RT deformation: incompatible previous vertex count");
            return nullptr;
        }
        auto &posed = mDeformedMeshes[frame].emplace_back();
        const auto dispatch = [&](std::span<const glm::vec4> sample, VkBuffer &data, VmaAllocation &dataAllocation,
                                  VkBuffer &vertices, VmaAllocation &vertexAllocation)
        {
            if (!CreateUploadBuffer(mDevice->Allocator(), std::as_bytes(sample),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                data, dataAllocation) ||
                !CreateBuffer(mDevice->Allocator(), VkDeviceSize(count) * sizeof(Vertex),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                0, vertices, vertexAllocation, nullptr))
                return false;
            const DeformationPush push{
                Address(mDevice->Handle(), mesh.mVertexBuffer), Address(mDevice->Handle(), data),
                Address(mDevice->Handle(), vertices), count };
            vkCmdPushConstants(cmd, mDeformationLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
            return true;
        };
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mDeformationPipeline);
        if (!dispatch(deformation, posed.deformation, posed.deformationAllocation,
                posed.vertices, posed.vertexAllocation) ||
            (!previousDeformation.empty() && previousDeformation.data() != deformation.data() &&
                !dispatch(previousDeformation, posed.previousDeformation, posed.previousDeformationAllocation,
                    posed.previousVertices, posed.previousVertexAllocation)))
        {
            MIR_LOG_ERROR("RT deformation: frame buffer allocation failed");
            return nullptr;
        }

        const VkMemoryBarrier2 deformed{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT };
        const VkDependencyInfo afterDeformation{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &deformed };
        vkCmdPipelineBarrier2(cmd, &afterDeformation);

        GpuMesh geometry = mesh;
        geometry.mVertexBuffer = posed.vertices;
        if (!BuildBLAS(geometry, count, cmd, posed.blas, posed.scratch, posed.scratchAllocation))
        {
            MIR_LOG_ERROR("RT deformation: instance BLAS build failed");
            return nullptr;
        }
        const VkMemoryBarrier2 built{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR };
        const VkDependencyInfo beforeTlas{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &built };
        vkCmdPipelineBarrier2(cmd, &beforeTlas);
        return &posed;
    }
}

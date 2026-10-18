/**
 * @file ImGuiOverlay.cpp
 * @author Sumin Park
 * @brief ImGui Vulkan backend lifetime and overlay recording.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ImGuiOverlay.h"
#include "Device.h"
#include "Swapchain.h"
#include "VulkanConstants.h"

#include <core/log/Log.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

namespace mir::vk
{
    namespace
    {
        void CheckImGuiVulkanResult(VkResult result)
        {
            if (result != VK_SUCCESS)
                MIR_LOG_ERROR("CheckImGuiVulkanResult: ImGui Vulkan backend call failed: {}", static_cast<int>(result));
        }
    }

    bool ImGuiOverlay::Initialize(Device &device, const Swapchain &swapchain)
    {
        mDevice = &device;

        const VkFormat colorFormat = swapchain.Format();
        VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &colorFormat,
            .depthAttachmentFormat = kDepthFormat };

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = Device::kApiVersion;
        initInfo.Instance = mDevice->Instance();
        initInfo.PhysicalDevice = mDevice->Physical();
        initInfo.Device = mDevice->Handle();
        initInfo.QueueFamily = mDevice->QueueFamily();
        initInfo.Queue = mDevice->Queue();
        initInfo.DescriptorPoolSize = 64;
        initInfo.MinImageCount = swapchain.ImageCount();
        initInfo.ImageCount = swapchain.ImageCount();
        initInfo.UseDynamicRendering = true;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo;
        initInfo.CheckVkResultFn = &CheckImGuiVulkanResult;

        if (!ImGui_ImplVulkan_Init(&initInfo))
        {
            MIR_LOG_ERROR("Initialize: ImGui_ImplVulkan_Init failed");
            return false;
        }

        mBackendInitialized = true;
        return true;
    }

    void ImGuiOverlay::Shutdown()
    {
        if (!mBackendInitialized)
            return;

        vkDeviceWaitIdle(mDevice->Handle());
        ImGui_ImplVulkan_Shutdown();
        mBackendInitialized = false;
        mDrawData = nullptr;
    }

    void ImGuiOverlay::RecordInPass(VkCommandBuffer cmd) const
    {
        if (mDrawData != nullptr)
            ImGui_ImplVulkan_RenderDrawData(mDrawData, cmd);
    }
}

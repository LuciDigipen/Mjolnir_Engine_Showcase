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

#include <algorithm>

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
        initInfo.DescriptorPoolSize = 4096;
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

        mDevice->WaitIdle();
        ReleaseTextures();
        ImGui_ImplVulkan_Shutdown();
        mBackendInitialized = false;
        mDrawData = nullptr;
    }

    uint64_t ImGuiOverlay::BindTexture(uint64_t key, VkImageView view)
    {
        if (!mBackendInitialized || view == VK_NULL_HANDLE)
            return 0;
        for (const auto &binding : mTextures)
            if (binding.mKey == key && binding.mRetireAfter == UINT64_MAX)
                return reinterpret_cast<uint64_t>(binding.mSet);
        const auto set = ImGui_ImplVulkan_AddTexture(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (set == VK_NULL_HANDLE)
            return 0;
        mTextures.push_back({ key, set });
        return reinterpret_cast<uint64_t>(set);
    }

    void ImGuiOverlay::RetireTexture(uint64_t key, uint64_t lastUse)
    {
        for (auto &binding : mTextures)
            if (binding.mKey == key && binding.mRetireAfter == UINT64_MAX)
                binding.mRetireAfter = lastUse;
    }

    void ImGuiOverlay::ReleaseTextures()
    {
        for (const auto &binding : mTextures)
            ImGui_ImplVulkan_RemoveTexture(binding.mSet);
        mTextures.clear();
    }

    void ImGuiOverlay::Collect(uint64_t completed)
    {
        std::erase_if(mTextures, [completed](const TextureBinding &binding)
            {
                if (binding.mRetireAfter > completed)
                    return false;
                ImGui_ImplVulkan_RemoveTexture(binding.mSet);
                return true;
            });
    }

    void ImGuiOverlay::RecordInPass(VkCommandBuffer cmd) const
    {
        if (mDrawData != nullptr)
            ImGui_ImplVulkan_RenderDrawData(mDrawData, cmd);
    }
}

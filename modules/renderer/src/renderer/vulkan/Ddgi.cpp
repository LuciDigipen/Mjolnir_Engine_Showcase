/**
 * @file Ddgi.cpp
 * @author Rahul Nair
 * @brief Renderer-owned DDGI resources and ordered probe updates.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "Ddgi.h"
#include "Device.h"
#include "VulkanUtil.h"
#include "VulkanPushConstants.h"

#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/math/Align.h>
#include <renderer/ViewSubmission.h>
#include <rtxgi/ddgi/DDGIVolume.h>

#include <array>
#include <cstring>
#include <optional>
#include <vector>

namespace mir::vk
{
    namespace
    {
        static_assert(sizeof(rtxgi::DDGIVolumeDescGPUPacked) == 128);
        // Use SDK volume math with renderer-owned Vulkan resources.
        class ProbeVolume final : public rtxgi::DDGIVolumeBase
        {
        public:
            explicit ProbeVolume(const rtxgi::DDGIVolumeDesc &desc) { m_desc = desc; SeedRNG(341); }
            void Destroy() override {}
        };

        void ProbeBarrier(VkCommandBuffer cmd)
        {
            const VkMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT };
            const VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1, .pMemoryBarriers = &barrier };
            vkCmdPipelineBarrier2(cmd, &dependency);
        }
    }

    struct Ddgi::Impl
    {
        Device *mDevice = nullptr;
        VkDescriptorSetLayout mSceneLayout{}, mCameraLayout{}, mSetLayout{};
        VkPipelineLayout mComputeLayout{}, mTraceLayout{};
        VkDescriptorPool mPool{};
        VkDescriptorSet mSets[kFramesInFlight]{};
        VkSampler mSampler{};
        struct Atlas { VkImage image{}; VmaAllocation allocation{}; VkImageView view{}; uint32_t layers = 0; };
        std::array<Atlas, 5> mImages{}; // rays, irradiance, distance, state, variability
        struct Constants { VkBuffer buffer{}; VmaAllocation allocation{}; void *mapped = nullptr; };
        std::array<Constants, kFramesInFlight> mConstants{};
        VkDeviceSize mBudgetOffset = 0;
        struct TraceState
        {
            VkPipeline pipeline{};
            VkBuffer buffer{};
            VmaAllocation allocation{};
            VkStridedDeviceAddressRegionKHR raygen{}, miss{}, hit{}, callable{};
        };
        TraceState mProbe, mCamera;
        std::array<VkPipeline, 6> mCompute{};
        std::unique_ptr<ProbeVolume> mVolume;
        DdgiSettings mSettings;
        std::optional<DdgiSettings> mFailed;
        ProbeUpdateSchedule mSchedule;
        bool mReset = true, mInitializedImages = false, mIncompatible = false;
        uint64_t mImageBytes = 0;

        std::optional<RayTracer::GiShading> Shading(uint32_t frame) const
        {
            const float weight = mSchedule.SamplingWeight();
            if (weight == 0.0f) return std::nullopt;
            return RayTracer::GiShading{ mCamera.pipeline, mTraceLayout, mSets[frame],
                mCamera.raygen, mCamera.miss, mCamera.hit, mCamera.callable, mSettings.intensity, weight };
        }

        void Destroy()
        {
            if (!mDevice) return;
            const VkDevice handle = mDevice->Handle();
            mVolume.reset();
            for (auto &pipeline : mCompute) { if (pipeline) vkDestroyPipeline(handle, pipeline, nullptr); pipeline = {}; }
            for (auto *trace : { &mProbe, &mCamera })
            {
                if (trace->pipeline) vkDestroyPipeline(handle, trace->pipeline, nullptr);
                if (trace->buffer) vmaDestroyBuffer(mDevice->Allocator(), trace->buffer, trace->allocation);
                *trace = {};
            }
            if (mPool) vkDestroyDescriptorPool(handle, mPool, nullptr);
            if (mTraceLayout) vkDestroyPipelineLayout(handle, mTraceLayout, nullptr);
            if (mComputeLayout) vkDestroyPipelineLayout(handle, mComputeLayout, nullptr);
            if (mSetLayout) vkDestroyDescriptorSetLayout(handle, mSetLayout, nullptr);
            if (mSampler) vkDestroySampler(handle, mSampler, nullptr);
            for (auto &atlas : mImages)
            {
                if (atlas.view) vkDestroyImageView(handle, atlas.view, nullptr);
                if (atlas.image) vmaDestroyImage(mDevice->Allocator(), atlas.image, atlas.allocation);
                atlas = {};
            }
            for (auto &slot : mConstants)
            {
                if (slot.buffer) vmaDestroyBuffer(mDevice->Allocator(), slot.buffer, slot.allocation);
                slot = {};
            }
            mPool = {}; mTraceLayout = {}; mComputeLayout = {};
            mSetLayout = {}; mSampler = {}; mSchedule.Reset(); mImageBytes = 0;
            mReset = true; mInitializedImages = false;
        }

        bool CreateAtlas(const rtxgi::DDGIVolumeDesc &desc, rtxgi::EDDGIVolumeTextureType type, Atlas &atlas)
        {
            uint32_t width, height;
            rtxgi::GetDDGIVolumeTextureDimensions(desc, type, width, height, atlas.layers);
            const auto &limits = mDevice->Properties().limits;
            if (width > limits.maxImageDimension2D || height > limits.maxImageDimension2D || atlas.layers > limits.maxImageArrayLayers)
                return false;
            constexpr VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(mDevice->Physical(), format, &properties);
            constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            if ((properties.optimalTilingFeatures & needed) != needed) return false;
            const VkImageCreateInfo info{ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent{ width, height, 1 },
                .mipLevels = 1, .arrayLayers = atlas.layers, .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
            const VmaAllocationCreateInfo allocationInfo{ .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };
            VmaAllocationInfo allocated{};
            if (vmaCreateImage(mDevice->Allocator(), &info, &allocationInfo, &atlas.image, &atlas.allocation, &allocated) != VK_SUCCESS)
                return false;
            mImageBytes += allocated.size;
            if (mImageBytes > DdgiSettings::kMaxEstimatedBytes) return false;
            const VkImageViewCreateInfo viewInfo{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = atlas.image, .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = format,
                .subresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, atlas.layers } };
            return vkCreateImageView(mDevice->Handle(), &viewInfo, nullptr, &atlas.view) == VK_SUCCESS;
        }

        bool CreateDescriptors()
        {
            const VkDevice handle = mDevice->Handle();
            constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            std::array<VkDescriptorSetLayoutBinding, 11> bindings{};
            bindings[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr };
            for (uint32_t i = 1; i <= 5; ++i)
                bindings[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stages, nullptr };
            bindings[6] = { 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr };
            for (uint32_t i = 7; i <= 9; ++i)
                bindings[i] = { i + 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr };
            bindings[10] = { 11, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stages, nullptr };
            const VkDescriptorSetLayoutCreateInfo setInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = uint32_t(bindings.size()), .pBindings = bindings.data() };
            if (vkCreateDescriptorSetLayout(handle, &setInfo, nullptr, &mSetLayout) != VK_SUCCESS) return false;
            const VkPushConstantRange push{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 24 }; // SDK DDGIRootConstants
            const VkPipelineLayoutCreateInfo computeInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1, .pSetLayouts = &mSetLayout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
            if (vkCreatePipelineLayout(handle, &computeInfo, nullptr, &mComputeLayout) != VK_SUCCESS) return false;
            const std::array<VkDescriptorSetLayout, 3> layouts{ mSceneLayout, mCameraLayout, mSetLayout };
            const VkPushConstantRange tracePush{ VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
                0, sizeof(RtPushConstants) };
            const VkPipelineLayoutCreateInfo traceInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = uint32_t(layouts.size()), .pSetLayouts = layouts.data(),
                .pushConstantRangeCount = 1, .pPushConstantRanges = &tracePush };
            if (vkCreatePipelineLayout(handle, &traceInfo, nullptr, &mTraceLayout) != VK_SUCCESS) return false;
            const std::array<VkDescriptorPoolSize, 4> sizes{{
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kFramesInFlight},
                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 * kFramesInFlight},
                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * kFramesInFlight},
                {VK_DESCRIPTOR_TYPE_SAMPLER, kFramesInFlight} }};
            const VkDescriptorPoolCreateInfo poolInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = kFramesInFlight, .poolSizeCount = uint32_t(sizes.size()), .pPoolSizes = sizes.data() };
            if (vkCreateDescriptorPool(handle, &poolInfo, nullptr, &mPool) != VK_SUCCESS) return false;
            const VkSamplerCreateInfo samplerInfo{ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f };
            if (vkCreateSampler(handle, &samplerInfo, nullptr, &mSampler) != VK_SUCCESS) return false;
            mBudgetOffset = AlignUp(VkDeviceSize(sizeof(rtxgi::DDGIVolumeDescGPUPacked)),
                mDevice->Properties().limits.minStorageBufferOffsetAlignment);
            for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
            {
                auto &slot = mConstants[frame];
                VmaAllocationInfo mapped{};
                if (!CreateBuffer(mDevice->Allocator(), mBudgetOffset + 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                    slot.buffer, slot.allocation, &mapped)) return false;
                slot.mapped = mapped.pMappedData;
                if (!AllocateDescriptorSet(handle, mPool, mSetLayout, mSets[frame])) return false;
                const VkDescriptorBufferInfo data{ slot.buffer, 0, sizeof(rtxgi::DDGIVolumeDescGPUPacked) };
                const VkDescriptorBufferInfo budget{ slot.buffer, mBudgetOffset, 16 };
                std::array<VkDescriptorImageInfo, 9> imageInfos{};
                for (uint32_t i = 0; i < 5; ++i) imageInfos[i] = { {}, mImages[i].view, VK_IMAGE_LAYOUT_GENERAL };
                for (uint32_t i = 0; i < 3; ++i) imageInfos[5 + i] = { {}, mImages[1 + i].view, VK_IMAGE_LAYOUT_GENERAL };
                imageInfos[8].sampler = mSampler;
                std::array<VkWriteDescriptorSet, 11> writes{};
                for (size_t i = 0; i < bindings.size(); ++i)
                {
                    writes[i] = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = mSets[frame], .dstBinding = bindings[i].binding, .descriptorCount = 1,
                        .descriptorType = bindings[i].descriptorType };
                    if (i == 0) writes[i].pBufferInfo = &data;
                    else if (i == 6) writes[i].pBufferInfo = &budget;
                    else writes[i].pImageInfo = &imageInfos[i < 6 ? i - 1 : i - 2];
                }
                vkUpdateDescriptorSets(handle, uint32_t(writes.size()), writes.data(), 0, nullptr);
            }
            return true;
        }

        bool CreateCompute()
        {
            constexpr const char *names[]{ "ProbeIrradiance", "ProbeDistance", "ProbeRelocation", "ProbeClassification",
                "ProbeRelocationReset", "ProbeClassificationReset" };
            constexpr const char *entries[]{ "DDGIProbeBlendingCS", "DDGIProbeBlendingCS", "DDGIProbeRelocationCS",
                "DDGIProbeClassificationCS", "DDGIProbeRelocationResetCS", "DDGIProbeClassificationResetCS" };
            for (size_t i = 0; i < mCompute.size(); ++i)
            {
                const auto path = ShaderPath(std::string("rtxgi/") + names[i] + ".spv");
                std::error_code error;
                if (!std::filesystem::is_regular_file(path, error)) return false;
                const VkShaderModule module = CreateShaderModule(mDevice->Handle(), path);
                if (!module) return false;
                const VkComputePipelineCreateInfo info{ .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                    .stage{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                        .module = module, .pName = entries[i] }, .layout = mComputeLayout };
                const VkResult result = vkCreateComputePipelines(mDevice->Handle(), {}, 1, &info, nullptr, &mCompute[i]);
                vkDestroyShaderModule(mDevice->Handle(), module, nullptr);
                if (result != VK_SUCCESS) return false;
            }
            return true;
        }

        bool CreateTrace(const char *shader, const std::array<const char *, 6> &names, TraceState &output)
        {
            constexpr VkShaderStageFlagBits types[]{ VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
                VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                VK_SHADER_STAGE_ANY_HIT_BIT_KHR };
            std::array<VkShaderModule, 6> modules{};
            std::array<VkPipelineShaderStageCreateInfo, 6> stages{};
            for (const char *name : names)
            {
                std::error_code error;
                if (!std::filesystem::is_regular_file(ShaderPath(std::string(shader) + "." + name + ".spv"), error))
                    return false;
            }
            bool loaded = true;
            for (size_t i = 0; i < modules.size(); ++i)
            {
                modules[i] = CreateShaderModule(mDevice->Handle(), ShaderPath(std::string(shader) + "." + names[i] + ".spv"));
                loaded &= modules[i] != VK_NULL_HANDLE;
                stages[i] = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = types[i], .module = modules[i], .pName = names[i] };
            }
            std::array<VkRayTracingShaderGroupCreateInfoKHR, 5> groups{};
            for (uint32_t i = 0; i < 5; ++i)
                groups[i] = { .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                    .type = i < 3 ? VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
                    .generalShader = i < 3 ? i : VK_SHADER_UNUSED_KHR,
                    .closestHitShader = i == 3 ? 3u : VK_SHADER_UNUSED_KHR,
                    .anyHitShader = i >= 3 ? i + 1 : VK_SHADER_UNUSED_KHR,
                    .intersectionShader = VK_SHADER_UNUSED_KHR };
            const VkRayTracingPipelineCreateInfoKHR info{ .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
                .stageCount = uint32_t(stages.size()), .pStages = stages.data(),
                .groupCount = uint32_t(groups.size()), .pGroups = groups.data(), .maxPipelineRayRecursionDepth = 2, .layout = mTraceLayout };
            const VkResult result = loaded ? vkCreateRayTracingPipelinesKHR(mDevice->Handle(), {}, {}, 1, &info, nullptr, &output.pipeline) : VK_ERROR_INITIALIZATION_FAILED;
            for (auto module : modules) if (module) vkDestroyShaderModule(mDevice->Handle(), module, nullptr);
            if (result != VK_SUCCESS) return false;
            const auto &properties = mDevice->RtProperties();
            const uint32_t stride = AlignUp(properties.shaderGroupHandleSize, properties.shaderGroupBaseAlignment);
            std::vector<std::byte> handles(properties.shaderGroupHandleSize * groups.size());
            if (vkGetRayTracingShaderGroupHandlesKHR(mDevice->Handle(), output.pipeline, 0, uint32_t(groups.size()), handles.size(), handles.data()) != VK_SUCCESS)
                return false;
            const VkBufferCreateInfo bufferInfo{ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = stride * groups.size(), .usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
            const VmaAllocationCreateInfo allocationInfo{ .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT, .usage = VMA_MEMORY_USAGE_AUTO };
            VmaAllocationInfo mapped{};
            if (vmaCreateBufferWithAlignment(mDevice->Allocator(), &bufferInfo, &allocationInfo, properties.shaderGroupBaseAlignment,
                &output.buffer, &output.allocation, &mapped) != VK_SUCCESS) return false;
            for (size_t i = 0; i < groups.size(); ++i)
                std::memcpy(static_cast<std::byte *>(mapped.pMappedData) + i * stride,
                    handles.data() + i * properties.shaderGroupHandleSize, properties.shaderGroupHandleSize);
            vmaFlushAllocation(mDevice->Allocator(), output.allocation, 0, VK_WHOLE_SIZE);
            const VkBufferDeviceAddressInfo addressInfo{ .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = output.buffer };
            const VkDeviceAddress address = vkGetBufferDeviceAddress(mDevice->Handle(), &addressInfo);
            output.raygen = { address, stride, stride }; output.miss = { address + stride, stride, 2 * stride };
            output.hit = { address + 3 * stride, stride, 2 * stride };
            return true;
        }

        bool Create()
        {
            const auto &limits = mDevice->Properties().limits;
            if (limits.maxComputeWorkGroupInvocations < 256 || limits.maxComputeWorkGroupSize[0] < 16 ||
                limits.maxComputeWorkGroupSize[1] < 16 ||
                uint64_t(mSettings.raysPerProbe) * mSettings.probesPerFrame > mDevice->RtProperties().maxRayDispatchInvocationCount)
                return false;
            rtxgi::DDGIVolumeDesc desc{};
            desc.probeCounts = { mSettings.probeCounts[0], mSettings.probeCounts[1], mSettings.probeCounts[2] };
            desc.probeSpacing = { mSettings.probeSpacing, mSettings.probeSpacing, mSettings.probeSpacing };
            desc.probeNumRays = mSettings.raysPerProbe;
            desc.probeNumIrradianceTexels = 8; desc.probeNumIrradianceInteriorTexels = 6;
            desc.probeNumDistanceTexels = 16; desc.probeNumDistanceInteriorTexels = 14;
            desc.probeMaxRayDistance = MakeGiTraceVolume(mSettings)->maxRayDistance;
            desc.probeRayDataFormat = desc.probeIrradianceFormat = desc.probeDataFormat = rtxgi::EDDGIVolumeTextureFormat::F32x4;
            desc.probeDistanceFormat = rtxgi::EDDGIVolumeTextureFormat::F32x2;
            desc.probeVariabilityFormat = rtxgi::EDDGIVolumeTextureFormat::F32;
            desc.probeRelocationEnabled = desc.probeClassificationEnabled = true;
            desc.probeMinFrontfaceDistance = mSettings.probeSpacing * 0.1f;
            desc.probeNormalBias = desc.probeViewBias = mSettings.probeSpacing * 0.05f;
            constexpr rtxgi::EDDGIVolumeTextureType types[]{ rtxgi::EDDGIVolumeTextureType::RayData,
                rtxgi::EDDGIVolumeTextureType::Irradiance, rtxgi::EDDGIVolumeTextureType::Distance,
                rtxgi::EDDGIVolumeTextureType::Data, rtxgi::EDDGIVolumeTextureType::Variability };
            for (size_t i = 0; i < mImages.size(); ++i)
                if (!CreateAtlas(desc, types[i], mImages[i])) return false;
            constexpr std::array<const char *, 6> probeEntries{
                "probeRayGen", "probeMiss", "shadowMissMain", "probeClosestHit", "probeAnyHit", "shadowAnyHitMain" };
            constexpr std::array<const char *, 6> cameraEntries{
                "rayGenMain", "missMain", "shadowMissMain", "closestHitMain", "anyHitMain", "shadowAnyHitMain" };
            if (!CreateDescriptors() || !CreateCompute() ||
                !CreateTrace("ddgiProbe", probeEntries, mProbe) || !CreateTrace("raytraceGi", cameraEntries, mCamera)) return false;
            mVolume = std::make_unique<ProbeVolume>(desc);
            MIR_LOG_INFO("DDGI: {} probes, {} rays/probe, {:.2f} MiB probe images", mSettings.ProbeCount(), mSettings.raysPerProbe,
                double(mImageBytes) / (1024.0 * 1024.0));
            return true;
        }
    };

    Ddgi::Ddgi() : mImpl(std::make_unique<Impl>()) {}
    Ddgi::~Ddgi() = default;

    void Ddgi::Initialize(Device &device, VkDescriptorSetLayout sceneLayout, VkDescriptorSetLayout cameraLayout)
    {
        mImpl->mDevice = &device; mImpl->mSceneLayout = sceneLayout; mImpl->mCameraLayout = cameraLayout;
        mImpl->mIncompatible = false;
    }

    void Ddgi::Configure(const DdgiSettings &settings, bool active)
    {
        auto &state = *mImpl;
        if (!state.mDevice) return;
        const bool wanted = active && settings.enabled && MakeGiTraceVolume(settings).has_value();
        const bool changed = settings.origin != state.mSettings.origin || settings.probeCounts != state.mSettings.probeCounts ||
            settings.probeSpacing != state.mSettings.probeSpacing || settings.raysPerProbe != state.mSettings.raysPerProbe;
        if (state.mVolume && (!wanted || changed)) { state.mDevice->WaitIdle(); state.Destroy(); }
        state.mSettings = settings;
        if (!wanted) { state.mFailed.reset(); return; }
        if (!state.mVolume && (!state.mFailed || *state.mFailed != settings))
        {
            if (!state.Create())
            {
                MIR_LOG_WARN("DDGI resources unavailable; preserving existing RT lighting");
                state.Destroy(); state.mFailed = settings;
            }
            else state.mFailed.reset();
        }
    }

    void Ddgi::ResetHistory() { mImpl->mReset = true; }
    bool Ddgi::CanUpdate(WorldPosition camera) const
    {
        const auto &state = *mImpl;
        if (!state.mVolume) return false;
        const auto coverage = MakeGiTraceVolume(state.mSettings);
        return coverage && TryCameraRelativeFloat(coverage->origin, camera).has_value() &&
            uint64_t(state.mSettings.probesPerFrame) * state.mSettings.raysPerProbe <=
                state.mDevice->RtProperties().maxRayDispatchInvocationCount;
    }
    void Ddgi::Shutdown() { mImpl->Destroy(); mImpl->mFailed.reset(); }

    std::optional<RayTracer::GiShading> Ddgi::Record(VkCommandBuffer cmd, uint32_t frame, WorldPosition camera,
        VkDescriptorSet cameraSet, uint32_t cameraOffset, const RayTracer::PreparedScene &scene, bool complete)
    {
        auto &state = *mImpl;
        if (!CanUpdate(camera)) { state.mReset = true; return std::nullopt; }
        // The probe and camera GI pipelines fix one legacy hit pair; a scene
        // with per-material offsets must stay on the base camera trace.
        if (!scene.giCompatible)
        {
            if (!state.mIncompatible)
            {
                MIR_LOG_WARN("DDGI disabled for scenes with per-material RT hit groups; "
                    "falling back to the base camera trace");
            }
            state.mIncompatible = true;
            state.mReset = true;
            return std::nullopt;
        }
        if (state.mIncompatible)
        {
            state.mIncompatible = false;
            state.mReset = true;
            MIR_LOG_INFO("DDGI re-enabled for a fixed-SBT scene");
        }
        const bool update = complete && !scene.truncated;
        if (!update)
        {
            if (state.mReset) return std::nullopt;
            state.mSchedule.SkipUpdate();
            if (state.mSchedule.Expired()) { state.mReset = true; return std::nullopt; }
        }
        const auto coverage = MakeGiTraceVolume(state.mSettings);
        const auto relative = TryCameraRelativeFloat(coverage->origin, camera);
        const uint32_t total = uint32_t(state.mSettings.ProbeCount());
        const uint32_t count = uint32_t(state.mSettings.probesPerFrame);
        if (state.mReset) state.mSchedule.Reset();
        const auto budget = state.mSchedule.Batch(total, count);
        state.mVolume->SetOrigin({ relative->x, relative->y, relative->z });
        if (update)
        {
            state.mVolume->SetProbeHysteresis(budget[3] == 0 ? 0.0f : 0.97f);
            state.mVolume->Update();
        }
        const auto packed = state.mVolume->GetDescGPUPacked();
        auto &constants = state.mConstants[frame];
        std::memcpy(constants.mapped, &packed, sizeof(packed));
        std::memcpy(static_cast<std::byte *>(constants.mapped) + state.mBudgetOffset, budget.data(), sizeof(budget));
        vmaFlushAllocation(state.mDevice->Allocator(), constants.allocation, 0, VK_WHOLE_SIZE);

        // Includes previous-frame atlas reads/writes on the same graphics queue.
        ProbeBarrier(cmd);
        if (!update) return state.Shading(frame);
        if (state.mReset)
        {
            for (auto &atlas : state.mImages)
            {
                ImageBarrier(cmd, atlas.image, VK_IMAGE_ASPECT_COLOR_BIT,
                    state.mInitializedImages ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, state.mInitializedImages ? VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT : 0,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 1, atlas.layers);
                const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, atlas.layers };
                const VkClearColorValue clear{};
                vkCmdClearColorImage(cmd, atlas.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
            }
            state.mInitializedImages = true;
            ProbeBarrier(cmd);
        }
        const uint32_t rootConstants[6]{};
        const auto dispatch = [&](size_t pipeline, uint32_t x, uint32_t y, uint32_t z)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, state.mCompute[pipeline]);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, state.mComputeLayout, 0, 1, &state.mSets[frame], 0, nullptr);
                vkCmdPushConstants(cmd, state.mComputeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rootConstants), rootConstants);
                vkCmdDispatch(cmd, x, y, z);
                ProbeBarrier(cmd);
            };
        state.mDevice->BeginLabel(cmd, "DDGI Probe Update", { 0.3f, 0.9f, 0.3f, 1.0f });
        if (state.mReset) { dispatch(4, (total + 31) / 32, 1, 1); dispatch(5, (total + 31) / 32, 1, 1); }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, state.mProbe.pipeline);
        const std::array<VkDescriptorSet, 3> sets{ scene.descriptors, cameraSet, state.mSets[frame] };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, state.mTraceLayout, 0, uint32_t(sets.size()),
            sets.data(), 1, &cameraOffset);
        vkCmdTraceRaysKHR(cmd, &state.mProbe.raygen, &state.mProbe.miss, &state.mProbe.hit, &state.mProbe.callable, uint32_t(state.mSettings.raysPerProbe), count, 1);
        ProbeBarrier(cmd);
        // Right-handed Y-up SDK layout: atlas X/Z dimensions, Y array layers.
        const auto &counts = state.mSettings.probeCounts;
        dispatch(0, uint32_t(counts[0]), uint32_t(counts[2]), uint32_t(counts[1]));
        dispatch(1, uint32_t(counts[0]), uint32_t(counts[2]), uint32_t(counts[1]));
        dispatch(2, (total + 31) / 32, 1, 1);
        dispatch(3, (total + 31) / 32, 1, 1);
        state.mDevice->EndLabel(cmd);
        state.mReset = false;
        state.mSchedule.Advance(total, count);
        return state.Shading(frame);
    }
}
